//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/integer_overflow_mode.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class IntegerOverflowMode : uint8_t { ERROR = 0, WRAP = 1 };

} // namespace duckdb
