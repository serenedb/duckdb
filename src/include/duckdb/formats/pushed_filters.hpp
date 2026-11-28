//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/pushed_filters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {

class ClientContext;
class DataChunk;
class TableFilter;

class PushedFilters {
public:
	void Add(ClientContext &context, const TableFilter &filter, idx_t column);
	bool Empty() const {
		return columns.empty();
	}
	idx_t Count() const {
		return columns.size();
	}
	idx_t Column(idx_t i) const {
		return columns[i];
	}
	idx_t Apply(SelectionVector &selection, DataChunk &chunk, idx_t count, idx_t approved);

private:
	vector<idx_t> columns;
	vector<unique_ptr<TableFilterState>> states;
};

} // namespace duckdb
