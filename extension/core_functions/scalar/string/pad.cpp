#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/pair.hpp"

#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

static pair<idx_t, idx_t> PadCountChars(const idx_t len, const char *data, const idx_t size) {
	//  Count how much of str will fit in the output
	idx_t nbytes = FirstNonAscii(data, MinValue(len, size));
	idx_t nchars = nbytes;
	for (; nchars < len && nbytes < size; ++nchars) {
		int32_t codepoint;
		nbytes += DecodeCodepoint(data + nbytes, size - nbytes, codepoint);
	}

	return pair<idx_t, idx_t>(nbytes, nchars);
}

static idx_t PaddingByteCount(const idx_t len, const string_t &pad) {
	//  Count the bytes InsertPadding will write
	auto data = pad.GetData();
	auto size = pad.GetSize();
	if (len == 0 || size == 0) {
		return 0;
	}
	auto pad_chars = PadCountChars(NumericLimits<idx_t>::Maximum(), data, size).second;
	auto remainder = PadCountChars(len % pad_chars, data, size).first;
	return (len / pad_chars) * size + remainder;
}

template <bool LEFT>
static string_t PadString(ExpressionState &state, Vector &result, const string_t &str, const idx_t len,
                          const string_t &pad) {
	const char *name = LEFT ? "LPAD" : "RPAD";
	auto data_str = str.GetData();
	auto written = PadCountChars(len, data_str, str.GetSize());
	auto padding_chars = len - written.second;
	if (padding_chars > 0 && pad.GetSize() == 0) {
		throw InvalidInputException("Insufficient padding in %s.", name);
	}
	auto padding_size = PaddingByteCount(padding_chars, pad);
	auto result_size = written.first + padding_size;
	if (result_size > string_t::MAX_STRING_SIZE) {
		throw OutOfRangeException(
		    "Cannot create a string of size: '%d' in %s, the maximum supported string size is: '%d'", result_size, name,
		    string_t::MAX_STRING_SIZE);
	}
	auto target = StringVector::EmptyString(result, result_size);
	auto target_data = target.GetDataWriteable();
	auto padding_data = LEFT ? target_data : target_data + written.first;
	memcpy(LEFT ? target_data + padding_size : target_data, data_str, written.first);
	if (padding_size > 0) {
		memcpy(padding_data, pad.GetData(), MinValue<idx_t>(pad.GetSize(), padding_size));
		FillRepeated(state, padding_data, pad.GetSize(), padding_size);
	}
	target.Finalize();
	return target;
}

template <bool LEFT>
static void PadFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<string_t, int32_t, string_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, [&](string_t str, int32_t len, string_t pad) {
		    return PadString<LEFT>(state, result, str, UnsafeNumericCast<idx_t>(MaxValue<int32_t>(len, 0)), pad);
	    });
}

ScalarFunction LpadFun::GetFunction() {
	ScalarFunction func({}, LogicalType::VARCHAR, PadFunction<true>);
	func.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("count", LogicalType::INTEGER)
	    .AddParameter("character", LogicalType::VARCHAR);
	func.SetFallible();
	return func;
}

ScalarFunction RpadFun::GetFunction() {
	ScalarFunction func({}, LogicalType::VARCHAR, PadFunction<false>);
	func.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("count", LogicalType::INTEGER)
	    .AddParameter("character", LogicalType::VARCHAR);
	func.SetFallible();
	return func;
}

} // namespace duckdb
