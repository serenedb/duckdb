#include "duckdb/formats/json_ranges.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/formats/csv_simd.hpp"

#include <bit>
#include <cstring>

namespace duckdb {

namespace {

struct JsonSpan {
	uint64_t escape_carry = 0;
	uint64_t in_string = 0;
	int64_t delta = 0;
	int64_t flipped_delta = 0;
};

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

JsonSpan ScanSpan(const char *data, idx_t size, uint64_t escape_carry, uint64_t in_string) {
	int64_t opens = 0;
	int64_t closes = 0;
	int64_t opens_outside = 0;
	int64_t closes_outside = 0;
	ScanJsonBlocks(data, size, escape_carry, in_string,
	               [&](idx_t, const CsvVector &vector, uint64_t strings, uint64_t) {
		               const uint64_t open = vector.Equal('{') | vector.Equal('[');
		               const uint64_t close = vector.Equal('}') | vector.Equal(']');
		               opens += std::popcount(open);
		               closes += std::popcount(close);
		               opens_outside += std::popcount(open & ~strings);
		               closes_outside += std::popcount(close & ~strings);
		               return true;
	               });
	JsonSpan span;
	span.escape_carry = escape_carry;
	span.in_string = in_string;
	span.delta = opens_outside - closes_outside;
	span.flipped_delta = (opens - opens_outside) - (closes - closes_outside);
	return span;
}

JsonRangeOrigin EndOrigin(bool in_string, uint64_t escape_carry) {
	if (!in_string) {
		return JsonRangeOrigin::OUTSIDE;
	}
	return escape_carry ? JsonRangeOrigin::ESCAPED : JsonRangeOrigin::STRING;
}

JsonRangeEntry Entry(JsonRangeOrigin origin, int64_t depth) {
	JsonRangeEntry entry;
	entry.end.origin = origin;
	entry.end.depth = depth;
	entry.computed = true;
	return entry;
}

} // namespace

void JsonRangeTransfers(const char *data, idx_t size, JsonRangeEntry &outside, JsonRangeEntry &string) {
	const auto span = ScanSpan(data, size, 0, 0);
	const bool in_string = span.in_string != 0;
	outside = Entry(EndOrigin(in_string, span.escape_carry), span.delta);
	string = Entry(EndOrigin(!in_string, span.escape_carry), span.flipped_delta);
}

JsonRangeEntry JsonRangeTransfer(const char *data, idx_t size, JsonRangeOrigin origin, bool file_start) {
	if (origin == JsonRangeOrigin::OUTSIDE || origin == JsonRangeOrigin::STRING) {
		JsonRangeEntry outside;
		JsonRangeEntry string;
		JsonRangeTransfers(data, size, outside, string);
		return origin == JsonRangeOrigin::OUTSIDE ? outside : string;
	}
	idx_t position = 0;
	uint64_t escape_carry = 0;
	uint64_t in_string = 0;
	if (origin == JsonRangeOrigin::ESCAPED) {
		escape_carry = 1;
		in_string = ~uint64_t(0);
	} else {
		if (file_start) {
			StringUtil::SkipBOM(data, size, position);
		}
		while (position < size && StringUtil::CharacterIsSpace(data[position])) {
			position++;
		}
		if (position == size) {
			return Entry(JsonRangeOrigin::BEFORE_ARRAY, 0);
		}
	}
	const auto span = ScanSpan(data + position, size - position, escape_carry, in_string);
	return Entry(EndOrigin(span.in_string != 0, span.escape_carry), span.delta);
}

idx_t JsonArraySeparator(const char *data, idx_t size, const JsonRangeStart &start) {
	idx_t position = 0;
	int64_t depth = start.depth;
	uint64_t escape_carry = start.origin == JsonRangeOrigin::ESCAPED ? 1 : 0;
	uint64_t in_string =
	    start.origin == JsonRangeOrigin::STRING || start.origin == JsonRangeOrigin::ESCAPED ? ~uint64_t(0) : 0;
	if (start.origin == JsonRangeOrigin::BEFORE_ARRAY) {
		while (position < size && StringUtil::CharacterIsSpace(data[position])) {
			position++;
		}
		if (position == size || data[position] != '[') {
			return DConstants::INVALID_INDEX;
		}
		position++;
		depth = 1;
	} else if (depth <= 0) {
		return DConstants::INVALID_INDEX;
	}
	idx_t found = DConstants::INVALID_INDEX;
	ScanJsonBlocks(data + position, size - position, escape_carry, in_string,
	               [&](idx_t offset, const CsvVector &vector, uint64_t strings, uint64_t) {
		               const uint64_t open = (vector.Equal('{') | vector.Equal('[')) & ~strings;
		               const uint64_t close = (vector.Equal('}') | vector.Equal(']')) & ~strings;
		               const uint64_t comma = vector.Equal(',') & ~strings;
		               for (uint64_t marks = open | close | comma; marks; marks &= marks - 1) {
			               const auto bit = std::countr_zero(marks);
			               const uint64_t mark = uint64_t(1) << bit;
			               if (open & mark) {
				               depth++;
			               } else if (close & mark) {
				               if (--depth <= 0) {
					               return false;
				               }
			               } else if (depth == 1) {
				               found = position + offset + static_cast<idx_t>(bit);
				               return false;
			               }
		               }
		               return true;
	               });
	return found;
}

idx_t JsonValueStart(const char *data, idx_t size, const JsonRangeStart &start) {
	int64_t depth = start.depth;
	uint64_t escape_carry = start.origin == JsonRangeOrigin::ESCAPED ? 1 : 0;
	uint64_t in_string =
	    start.origin == JsonRangeOrigin::STRING || start.origin == JsonRangeOrigin::ESCAPED ? ~uint64_t(0) : 0;
	idx_t found = DConstants::INVALID_INDEX;
	ScanJsonBlocks(data, size, escape_carry, in_string,
	               [&](idx_t offset, const CsvVector &vector, uint64_t strings, uint64_t quotes) {
		               const uint64_t open = (vector.Equal('{') | vector.Equal('[')) & ~strings;
		               const uint64_t close = (vector.Equal('}') | vector.Equal(']')) & ~strings;
		               const uint64_t opening = quotes & strings;
		               for (uint64_t marks = open | close | opening; marks; marks &= marks - 1) {
			               const auto bit = std::countr_zero(marks);
			               const uint64_t mark = uint64_t(1) << bit;
			               if (close & mark) {
				               depth--;
				               continue;
			               }
			               if (depth == 0) {
				               found = offset + static_cast<idx_t>(bit);
				               return false;
			               }
			               if (open & mark) {
				               depth++;
			               }
		               }
		               return true;
	               });
	return found;
}

} // namespace duckdb
