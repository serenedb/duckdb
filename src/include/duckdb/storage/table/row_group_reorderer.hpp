//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/row_group_reorderer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/partition_stats.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_order_options.hpp"
#include "duckdb/storage/table/row_group_segment_tree.hpp"
#include "duckdb/storage/table/segment_tree.hpp"

#include <compare>

namespace duckdb {

struct RowGroupOrderKey {
	static constexpr uint64_t NULLS_FIRST_MARKER = 0;
	static constexpr uint64_t VALUE_MARKER = 1;
	static constexpr uint64_t NULLS_LAST_MARKER = 2;
	static constexpr idx_t MARKER_SHIFT = 56;

	uint64_t part0 = VALUE_MARKER << MARKER_SHIFT;
	uint64_t part1 = 0;
	uint64_t part2 = 0;

	static RowGroupOrderKey Null(OrderByNullType null_order);
	static RowGroupOrderKey Get(const BaseStatistics &stats, OrderByStatistics order_by, OrderType order_type,
	                            OrderByNullType null_order);

	bool IsNull() const {
		return part0 >> MARKER_SHIFT != VALUE_MARKER;
	}
	auto operator<=>(const RowGroupOrderKey &) const = default;
};

struct OffsetPruningResult {
	idx_t offset_remainder;
	idx_t pruned_row_group_count;
	idx_t leading_null_group_offset;
};

class RowGroupReorderer {
public:
	RowGroupReorderer(const RowGroupOrderOptions &options_p, TransactionData transaction_p);
	optional_ptr<SegmentNode<RowGroup>> GetRootSegment(RowGroupSegmentTree &row_groups);
	optional_ptr<SegmentNode<RowGroup>> GetNextRowGroup(SegmentNode<RowGroup> &row_group);
	//! The number of rows in the row groups that will be scanned (after pruning)
	idx_t ScanRowCount() const;

	static OffsetPruningResult GetOffsetAfterPruning(OrderByStatistics order_by, OrderType order_type,
	                                                 OrderByNullType null_order, const StorageIndex &column_idx,
	                                                 idx_t row_offset, vector<PartitionStatistics> &stats);

private:
	const RowGroupOrderOptions options;
	const TransactionData transaction;

	idx_t offset;
	bool initialized;
	vector<reference<SegmentNode<RowGroup>>> ordered_row_groups;
};

} // namespace duckdb
