//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/json_ranges.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class JsonRangeOrigin : uint8_t { BEFORE_ARRAY, OUTSIDE, STRING, ESCAPED };

constexpr idx_t JSON_RANGE_ORIGINS = 4;

struct JsonRangeEntry;

struct JsonRangeStart {
	JsonRangeOrigin origin = JsonRangeOrigin::BEFORE_ARRAY;
	int64_t depth = 0;

	uint8_t Origin() const {
		return static_cast<uint8_t>(origin);
	}
	JsonRangeStart Next(const JsonRangeEntry &entry) const;
};

struct JsonRangeEntry {
	JsonRangeStart end;
	bool computed = false;
};

inline JsonRangeStart JsonRangeStart::Next(const JsonRangeEntry &entry) const {
	return JsonRangeStart {entry.end.origin, depth + entry.end.depth};
}

JsonRangeEntry JsonRangeTransfer(const char *data, idx_t size, JsonRangeOrigin origin, bool file_start);
void JsonRangeTransfers(const char *data, idx_t size, JsonRangeEntry &outside, JsonRangeEntry &string);
idx_t JsonArraySeparator(const char *data, idx_t size, const JsonRangeStart &start);
idx_t JsonValueStart(const char *data, idx_t size, const JsonRangeStart &start);

} // namespace duckdb
