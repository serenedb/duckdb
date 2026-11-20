//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/copy_encoding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/function/encoding_function.hpp"

#include <optional>
#include <string_view>

namespace duckdb {

std::optional<TextEncoding> CopyOutputEncoding(const string &name);

class CopyEncoder {
public:
	explicit CopyEncoder(TextEncoding encoding);

	void Encode(std::string_view input, string &out);

private:
	const TextEncoding encoding;
	string carry;
};

} // namespace duckdb
