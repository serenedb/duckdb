//===----------------------------------------------------------------------===//
//                         DuckDB
//
// property_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "text_unit.hpp"

#include <cstdint>

namespace duckdb {
namespace text {

extern const TextUnit property_units[];
extern const uint32_t property_unit_count;
extern const uint32_t property_values_unit;

} // namespace text
} // namespace duckdb
