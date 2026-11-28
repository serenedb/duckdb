//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/function_body.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct FunctionBody {
	optional_idx owner;
	optional_ptr<const vector<string>> config;

	bool operator==(const FunctionBody &other) const = default;
};

} // namespace duckdb
