//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/sql_compatibility.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class SqlCompatibility : uint8_t { DUCK = 0, POSTGRES = 1 };

} // namespace duckdb
