#include "duckdb/formats/pushed_filters.hpp"

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/storage/table/column_segment.hpp"

namespace duckdb {

void PushedFilters::Add(ClientContext &context, const TableFilter &filter, idx_t column) {
	columns.push_back(column);
	states.push_back(TableFilterState::Initialize(context, filter));
}

idx_t PushedFilters::Apply(SelectionVector &selection, DataChunk &chunk, idx_t count, idx_t approved) {
	for (idx_t i = 0; i < columns.size() && approved > 0; i++) {
		ColumnSegment::FilterSelection(selection, chunk.data[columns[i]], *states[i], count, approved);
	}
	return approved;
}

} // namespace duckdb
