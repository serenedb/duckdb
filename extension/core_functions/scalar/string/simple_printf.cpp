////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "core_functions/scalar/simple_printf.hpp"

#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

namespace duckdb {

namespace {

struct Piece {
	bool is_literal;
	idx_t begin;
	idx_t size;
	idx_t arg;
};

bool IsInteger(const LogicalType &type) {
	return type.id() == LogicalTypeId::BIGINT || type.id() == LogicalTypeId::UBIGINT;
}

bool IsSupported(const LogicalType &type) {
	return IsInteger(type) || type.id() == LogicalTypeId::VARCHAR;
}

bool Compile(const string_t &format, DataChunk &args, bool printf_style, string &literals, vector<Piece> &pieces) {
	auto data = format.GetData();
	auto size = format.GetSize();
	idx_t next_arg = 1;
	auto add_literal = [&](char c) {
		if (pieces.empty() || !pieces.back().is_literal) {
			pieces.push_back({true, literals.size(), 0, 0});
		}
		literals += c;
		pieces.back().size++;
	};
	for (idx_t i = 0; i < size; i++) {
		char c = data[i];
		if (printf_style) {
			if (c != '%') {
				add_literal(c);
				continue;
			}
			if (i + 1 >= size) {
				return false;
			}
			char spec = data[++i];
			if (spec == '%') {
				add_literal('%');
				continue;
			}
			if (next_arg >= args.ColumnCount()) {
				return false;
			}
			auto &type = args.data[next_arg].GetType();
			if (!((spec == 'd' || spec == 'i') && IsInteger(type)) &&
			    !(spec == 's' && type.id() == LogicalTypeId::VARCHAR)) {
				return false;
			}
		} else {
			if (c == '{' || c == '}') {
				if (i + 1 < size && data[i + 1] == c) {
					add_literal(c);
					i++;
					continue;
				}
				if (c == '}' || i + 1 >= size || data[i + 1] != '}') {
					return false;
				}
				i++;
				if (next_arg >= args.ColumnCount() || !IsSupported(args.data[next_arg].GetType())) {
					return false;
				}
			} else {
				add_literal(c);
				continue;
			}
		}
		pieces.push_back({false, 0, 0, next_arg++});
	}
	return next_arg == args.ColumnCount();
}

void AppendArgument(const UnifiedVectorFormat &format, const LogicalType &type, idx_t idx, std::string &out) {
	char digits[24];
	char *end = digits + sizeof(digits);
	switch (type.id()) {
	case LogicalTypeId::BIGINT: {
		auto value = UnifiedVectorFormat::GetData<int64_t>(format)[idx];
		auto magnitude = value < 0 ? uint64_t(0) - uint64_t(value) : uint64_t(value);
		auto begin = NumericHelper::FormatUnsigned<uint64_t>(magnitude, end);
		if (value < 0) {
			*--begin = '-';
		}
		out.append(begin, end);
		break;
	}
	case LogicalTypeId::UBIGINT: {
		auto begin = NumericHelper::FormatUnsigned<uint64_t>(UnifiedVectorFormat::GetData<uint64_t>(format)[idx], end);
		out.append(begin, end);
		break;
	}
	default: {
		auto &value = UnifiedVectorFormat::GetData<string_t>(format)[idx];
		out.append(value.GetData(), value.GetSize());
		break;
	}
	}
}

} // namespace

bool SimplePrintf::TryExecute(DataChunk &args, Vector &result, bool printf_style) {
	auto &format_vector = args.data[0];
	if (format_vector.GetVectorType() != VectorType::CONSTANT_VECTOR || ConstantVector::IsNull(format_vector)) {
		return false;
	}
	string literals;
	vector<Piece> pieces;
	if (!Compile(ConstantVector::GetData<string_t>(format_vector)[0], args, printf_style, literals, pieces)) {
		return false;
	}
	const idx_t count = args.size();
	vector<UnifiedVectorFormat> formats(args.ColumnCount());
	for (idx_t col = 1; col < args.ColumnCount(); col++) {
		args.data[col].ToUnifiedFormat(formats[col]);
	}
	auto writer = FlatVector::Writer<string_t>(result, count);
	std::string out;
	for (idx_t row = 0; row < count; row++) {
		bool valid = true;
		for (idx_t col = 1; col < args.ColumnCount(); col++) {
			if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
				valid = false;
				break;
			}
		}
		if (!valid) {
			writer.WriteNull();
			continue;
		}
		out.clear();
		for (auto &piece : pieces) {
			if (piece.is_literal) {
				out.append(literals, piece.begin, piece.size);
			} else {
				auto &format = formats[piece.arg];
				AppendArgument(format, args.data[piece.arg].GetType(), format.sel->get_index(row), out);
			}
		}
		writer.WriteValue(string_t(out.data(), UnsafeNumericCast<uint32_t>(out.size())));
	}
	return true;
}

} // namespace duckdb
