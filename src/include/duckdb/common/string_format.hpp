//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/string_format.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector.hpp"

#include <string_view>

namespace duckdb {

enum class FormatArgumentType : uint8_t { BOOLEAN, BIGINT, UBIGINT, HUGEINT, UHUGEINT, DOUBLE, STRING };

struct FormatArgument {
	explicit FormatArgument(bool value) : type(FormatArgumentType::BOOLEAN), boolean(value) {
	}
	explicit FormatArgument(int64_t value) : type(FormatArgumentType::BIGINT), bigint(value) {
	}
	explicit FormatArgument(uint64_t value) : type(FormatArgumentType::UBIGINT), ubigint(value) {
	}
	explicit FormatArgument(hugeint_t value) : type(FormatArgumentType::HUGEINT), hugeint(value) {
	}
	explicit FormatArgument(uhugeint_t value) : type(FormatArgumentType::UHUGEINT), uhugeint(value) {
	}
	explicit FormatArgument(double value) : type(FormatArgumentType::DOUBLE), floating(value) {
	}
	explicit FormatArgument(std::string_view value) : type(FormatArgumentType::STRING), boolean(false), string(value) {
	}

	FormatArgumentType type;
	union {
		bool boolean;
		int64_t bigint;
		uint64_t ubigint;
		hugeint_t hugeint;
		uhugeint_t uhugeint;
		double floating;
	};
	std::string_view string;
};

class StringFormat {
public:
	DUCKDB_API static string Printf(std::string_view format, const vector<FormatArgument> &args);
	DUCKDB_API static string Format(std::string_view format, const vector<FormatArgument> &args);
};

} // namespace duckdb
