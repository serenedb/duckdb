//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/json_ranges.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/formats/csv_simd.hpp"

#include <bit>
#include <cstring>

namespace duckdb {

template <class VISIT>
void ScanJsonBlocks(const char *data, idx_t size, uint64_t &escape_carry, uint64_t &in_string, VISIT &&visit) {
	char tail[CSV_BLOCK_BYTES];
	for (idx_t offset = 0; offset < size; offset += CSV_BLOCK_BYTES) {
		const idx_t available = MinValue<idx_t>(CSV_BLOCK_BYTES, size - offset);
		const char *block = data + offset;
		if (available < CSV_BLOCK_BYTES) {
			memset(tail, 0, sizeof(tail));
			memcpy(tail, block, available);
			block = tail;
		}
		const CsvVector vector(block);
		const uint64_t escaped = CsvFindEscaped(vector.Equal('\\'), escape_carry);
		const uint64_t quotes = vector.Equal('"') & ~escaped;
		const uint64_t strings = CsvPrefixXor(quotes) ^ in_string;
		in_string = CsvSpread(strings >> 63);
		if (available < CSV_BLOCK_BYTES) {
			escape_carry = (escaped >> available) & 1;
		}
		if (!visit(offset, vector, strings, quotes)) {
			return;
		}
	}
}

inline idx_t JsonContainerEnd(const char *data, idx_t size) {
	uint64_t escape_carry = 0;
	uint64_t in_string = 0;
	int64_t depth = 0;
	idx_t found = size;
	ScanJsonBlocks(data, size, escape_carry, in_string,
	               [&](idx_t offset, const CsvVector &vector, uint64_t strings, uint64_t) {
		               const uint64_t opens = (vector.Equal('{') | vector.Equal('[')) & ~strings;
		               for (uint64_t marks = opens | ((vector.Equal('}') | vector.Equal(']')) & ~strings); marks;
		                    marks &= marks - 1) {
			               const auto bit = std::countr_zero(marks);
			               depth += (opens >> bit) & 1 ? 1 : -1;
			               if (depth == 0) {
				               found = offset + static_cast<idx_t>(bit) + 1;
				               return false;
			               }
		               }
		               return true;
	               });
	return found;
}

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
