//===----------------------------------------------------------------------===//
//                         DuckDB
//
// unicode_properties.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace duckdb {
namespace text {

struct PropertyRange {
	uint32_t first;
	uint32_t last;
};

class UnicodeProperties {
public:
	static bool Lookup(std::string_view expression, std::vector<PropertyRange> &ranges);
};

} // namespace text
} // namespace duckdb
