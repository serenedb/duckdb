#include "duckdb/formats/range_lines.hpp"

namespace duckdb {

bool RangeLines::Add(idx_t range, idx_t count) {
	if (range >= lines.size()) {
		lines.resize(range + 1);
	}
	lines[range] = lines[range].IsValid() ? lines[range].GetIndex() + count : count;
	if (range < known) {
		known_lines += count;
		return false;
	}
	const idx_t before = known;
	while (known < lines.size() && lines[known].IsValid()) {
		known_lines += lines[known].GetIndex();
		known++;
	}
	return known != before;
}

idx_t RangeLines::Before(idx_t range) const {
	if (range == known) {
		return known_lines;
	}
	idx_t total = 0;
	for (idx_t i = 0; i < MinValue<idx_t>(range, lines.size()); i++) {
		total += lines[i].IsValid() ? lines[i].GetIndex() : 0;
	}
	return total;
}

} // namespace duckdb
