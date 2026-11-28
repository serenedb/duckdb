#include "reader/string_column_reader.hpp"

#include <stddef.h>
#include <utility>

#include "simdutf.h"
#include "utf8proc_wrapper.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"

namespace duckdb {
class Vector;
struct SelectionVector;

//===--------------------------------------------------------------------===//
// String Column Reader
//===--------------------------------------------------------------------===//
StringColumnReader::StringColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
    : ColumnReader(reader, schema), string_column_type(GetStringColumnType(Type())) {
	fixed_width_string_length = 0;
	if (schema.parquet_type == Type::FIXED_LEN_BYTE_ARRAY) {
		fixed_width_string_length = schema.type_length;
	}
}

bool StringColumnReader::IsValid(const char *str_data, uint32_t str_len, const bool is_varchar) {
	if (!is_varchar) {
		return true;
	}
	return Utf8Proc::IsValid(str_data, str_len);
}

bool StringColumnReader::IsValid(const string &str, bool is_varchar) {
	return IsValid(str.c_str(), str.size(), is_varchar);
}

string_t StringColumnReader::VerifyString(const char *str_data, uint32_t str_len, const bool is_varchar) const {
	if (!is_varchar) {
		return string_t(str_data, str_len);
	}
	if (Utf8Proc::IsValid(str_data, str_len)) {
		return string_t(str_data, str_len);
	}

	switch (reader.parquet_options.utf8_validation_option) {
	case StringColumnReader::Utf8ValidationOption::STRICT_UTF8:
		throw InvalidInputException(
		    "Invalid string encoding found in Parquet file \"%s\": value \"%s\" is not valid UTF8!",
		    reader.GetFileName(), Blob::ToString(string_t(str_data, str_len)));

	case StringColumnReader::Utf8ValidationOption::REPLACE_UTF8: {
		if (!current_plain_result) {
			throw InternalException("VerifyString: REPLACE mode requires current_plain_result to be set");
		}
		auto target = StringVector::EmptyString(*current_plain_result, str_len);
		auto output = target.GetDataWriteable();
		memcpy(output, str_data, str_len);
		Utf8Proc::MakeValid(output, str_len);
		target.Finalize();
		return target;
	}

	case StringColumnReader::Utf8ValidationOption::IGNORE_UTF8: {
		if (!current_plain_result) {
			throw InternalException("VerifyString: IGNORE mode requires current_plain_result to be set");
		}
		auto new_str = Utf8Proc::RemoveInvalid(str_data, str_len);
		auto target = StringVector::EmptyString(*current_plain_result, new_str.size());
		auto output = target.GetDataWriteable();
		memcpy(output, new_str.data(), new_str.size()); // NOLINT(bugprone-not-null-terminated-result)
		target.Finalize();
		return target;
	}

	default:
		throw InternalException("Unimplemented Utf8ValidationOption");
	}
}

string_t StringColumnReader::VerifyString(const char *str_data, uint32_t str_len) const {
	switch (string_column_type) {
	case StringColumnType::VARCHAR:
		return VerifyString(str_data, str_len, true);
	case StringColumnType::JSON: {
		const auto error = StringUtil::ValidateJSON(str_data, str_len);
		if (!error.empty()) {
			throw InvalidInputException("Invalid JSON found in Parquet file: %s", error);
		}
		return string_t(str_data, str_len);
	}
	case StringColumnType::OTHER:
		return string_t(str_data, str_len);
	default:
		throw InternalException("Unimplemented StringColumnType");
	}
}

class ParquetStringVectorBuffer : public AuxiliaryDataHolder {
public:
	explicit ParquetStringVectorBuffer(shared_ptr<ResizeableBuffer> buffer_p) : buffer(std::move(buffer_p)) {
	}

	bool CertifiesImmutablePayloads() const override {
		return true;
	}

private:
	shared_ptr<ResizeableBuffer> buffer;
};

void StringColumnReader::ReferenceBlock(Vector &result, shared_ptr<ResizeableBuffer> &block) {
	StringVector::AddAuxiliaryData(result, make_uniq<ParquetStringVectorBuffer>(block));
}

void StringColumnReader::Plain(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                               idx_t result_offset, Vector &result) {
	ReferenceBlock(result, plain_data);
	current_plain_result = &result;
	PlainTemplated<string_t, StringParquetValueConversion>(*plain_data, defines, num_values, result_offset, result);
	current_plain_result = nullptr;
}

void StringColumnReader::PlainSelect(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                                     Vector &result, const SelectionVector &sel, idx_t count, idx_t result_offset) {
	ReferenceBlock(result, plain_data);
	current_plain_result = &result;
	PlainSelectTemplated<string_t, StringParquetValueConversion>(*plain_data, defines, num_values, result, sel, count,
	                                                             result_offset);
	current_plain_result = nullptr;
}

void StringColumnReader::PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) {
	PlainSkipTemplated<StringParquetValueConversion>(plain_data, defines, num_values);
}

void StringColumnReader::PlainPage(const ByteBuffer &values) {
	ascii_page = string_column_type == StringColumnType::VARCHAR &&
	             simdutf::validate_ascii(const_char_ptr_cast(values.ptr), values.len);
}

bool StringColumnReader::PlainFilters(const TableFilter &filter) {
	if (raw_filter.get() != &filter) {
		raw_filter = &filter;
		raw_predicates.clear();
		auto &expression_filter = ExpressionFilter::GetExpressionFilter(filter, "read_parquet");
		const bool string_column = string_column_type == StringColumnType::VARCHAR && !Type().HasAlias();
		if (!expression_filter.column_indexes.empty() || fixed_width_string_length != 0 ||
		    !CollectRawPredicates(*expression_filter.expr, string_column, raw_predicates)) {
			raw_predicates.clear();
		}
		raw_contains = RawContains::TryCreate(raw_predicates);
	}
	return !raw_predicates.empty();
}

void StringColumnReader::PlainFilterContains(ByteBuffer &values, uint8_t *defines, idx_t num_values, Vector &result,
                                             SelectionVector &passed, idx_t &count) {
	const auto max_define = MaxDefine();
	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	const auto base = const_char_ptr_cast(values.ptr);
	uint32_t starts[STANDARD_VECTOR_SIZE];
	uint32_t ends[STANDARD_VECTOR_SIZE];
	sel_t rows[STANDARD_VECTOR_SIZE];
	idx_t value_count = 0;
	for (idx_t row = 0; row < num_values; row++) {
		if (defines && defines[row] != max_define) {
			validity.SetInvalid(row);
			continue;
		}
		const auto length = values.read<uint32_t>();
		values.available(length);
		starts[value_count] = UnsafeNumericCast<uint32_t>(const_char_ptr_cast(values.ptr) - base);
		ends[value_count] = starts[value_count] + length;
		rows[value_count] = UnsafeNumericCast<sel_t>(row);
		value_count++;
		values.inc(length);
	}
	uint32_t matches[STANDARD_VECTOR_SIZE];
	count = raw_contains->Find(base, starts, ends, value_count, matches);
	for (idx_t i = 0; i < count; i++) {
		const auto value = matches[i];
		passed.set_index(i, rows[value]);
		data[rows[value]] = string_t(base + starts[value], ends[value] - starts[value]);
	}
}

void StringColumnReader::PlainFilter(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                                     Vector &result, SelectionVector &sel, idx_t &approved_tuple_count) {
	const auto max_define = MaxDefine();
	ReferenceBlock(result, plain_data);
	auto &values = *plain_data;
	SelectionVector passed(num_values);
	idx_t count = 0;
	if (raw_contains && ascii_page) {
		PlainFilterContains(values, defines, num_values, result, passed, count);
		approved_tuple_count = count;
		if (count < num_values) {
			sel.Initialize(passed);
		}
		return;
	}
	current_plain_result = &result;
	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	for (idx_t row = 0; row < num_values; row++) {
		passed.set_index(count, row);
		if (defines && defines[row] != max_define) {
			validity.SetInvalid(row);
			count += RawPasses(raw_predicates, nullptr, 0, true);
			continue;
		}
		const auto length = values.read<uint32_t>();
		values.available(length);
		string_t value;
		if (ascii_page) {
			MakeString(const_char_ptr_cast(values.ptr), length, const_char_ptr_cast(values.ptr) + values.len, value);
		} else {
			value = VerifyString(const_char_ptr_cast(values.ptr), length);
		}
		values.inc(length);
		if (RawPasses(raw_predicates, value.GetData(), value.GetSize(), false)) {
			data[row] = value;
			count++;
		}
	}
	current_plain_result = nullptr;
	approved_tuple_count = count;
	if (count < num_values) {
		sel.Initialize(passed);
	}
}

} // namespace duckdb
