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
	void Add(idx_t range, idx_t count);
	bool Known(idx_t range) const;
	idx_t Before(idx_t range) const;

private:
	vector<optional_idx> lines;
};

} // namespace duckdb
