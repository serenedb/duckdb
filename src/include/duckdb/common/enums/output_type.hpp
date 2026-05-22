//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/output_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class ExplainOutputType : uint8_t { ALL = 0, OPTIMIZED_ONLY = 1, PHYSICAL_ONLY = 2 };

// Column shape of EXPLAIN output: DUCKDB_NATIVE = native two-column {key, value};
// PG = single "QUERY PLAN" column with one row per plan line (PostgreSQL-compatible).
// (DUCKDB is a compile define, so the native value is spelled DUCKDB_NATIVE.)
enum class ExplainFormatShape : uint8_t { DUCKDB_NATIVE = 0, PG = 1 };

} // namespace duckdb
