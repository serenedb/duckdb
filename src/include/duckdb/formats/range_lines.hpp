//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/range_lines.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class RangeLines {
public:
	bool Add(idx_t range, idx_t count);
	bool Known(idx_t range) const {
		return range <= known;
	}
	idx_t Before(idx_t range) const;

private:
	vector<optional_idx> lines;
	idx_t known = 0;
	idx_t known_lines = 0;
};

} // namespace duckdb
