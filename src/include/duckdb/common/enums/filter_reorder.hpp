//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/filter_reorder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class FilterReorder : uint8_t { NEVER = 0, SAFE = 1, FAST = 2, ALWAYS = 3 };

} // namespace duckdb
