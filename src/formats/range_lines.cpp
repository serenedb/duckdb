#include "duckdb/formats/range_lines.hpp"

namespace duckdb {

void RangeLines::Add(idx_t range, idx_t count) {
	if (range >= lines.size()) {
		lines.resize(range + 1);
	}
	lines[range] = lines[range].IsValid() ? lines[range].GetIndex() + count : count;
}

bool RangeLines::Known(idx_t range) const {
	if (range > lines.size()) {
		return false;
	}
	for (idx_t i = 0; i < range; i++) {
		if (!lines[i].IsValid()) {
			return false;
		}
	}
	return true;
}

idx_t RangeLines::Before(idx_t range) const {
	idx_t total = 0;
	for (idx_t i = 0; i < MinValue<idx_t>(range, lines.size()); i++) {
		total += lines[i].IsValid() ? lines[i].GetIndex() : 0;
	}
	return total;
}

} // namespace duckdb
