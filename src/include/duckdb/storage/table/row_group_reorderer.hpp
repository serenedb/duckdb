//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/row_group_reorderer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_order_options.hpp"
#include "duckdb/storage/table/row_group_segment_tree.hpp"
#include "duckdb/storage/table/segment_tree.hpp"

#include <functional>

namespace duckdb {

using RowGroupOrderKey = SortKey<SortKeyType::NO_PAYLOAD_FIXED_24>;

struct RowGroupOrderKeys {
	static RowGroupOrderKey Get(const BaseStatistics &stats, OrderByStatistics order_by, OrderType order_type,
	                            OrderByNullType null_order);
	static RowGroupOrderKey Get(const Value &value, OrderType order_type, OrderByNullType null_order);
	static RowGroupOrderKey Null(OrderByNullType null_order);
	static bool IsNull(const RowGroupOrderKey &key, OrderByNullType null_order);
};

struct OffsetPruningResult {
	idx_t offset_remainder;
	idx_t pruned_row_group_count;
	idx_t leading_null_group_offset;
};

class RowGroupReorderer {
public:
	//! The number of rows a scan reads from one of its partitions - asked only for some of them, as it can be costly
	using PartitionRowCount = std::function<idx_t(idx_t partition)>;
	using PartitionMinMaxExact = std::function<bool(idx_t partition)>;

	RowGroupReorderer(const RowGroupOrderOptions &options_p, TransactionData transaction_p);
	//! The partitions (row groups) of a scan in the order the options ask for, without those OFFSET pruning skipped
	//! and those the row limit makes unnecessary. statistics[i] holds the statistics of the order column in partition
	//! i, nullptr when they are unknown
	static vector<idx_t> OrderPartitions(const RowGroupOrderOptions &options,
	                                     const vector<optional_ptr<const BaseStatistics>> &statistics,
	                                     const PartitionRowCount &row_count, const PartitionMinMaxExact &exact);
	optional_ptr<SegmentNode<RowGroup>> GetRootSegment(RowGroupSegmentTree &row_groups);
	optional_ptr<SegmentNode<RowGroup>> GetNextRowGroup(SegmentNode<RowGroup> &row_group);
	//! The number of rows in the row groups that will be scanned (after pruning)
	idx_t ScanRowCount() const;

	static OffsetPruningResult GetOffsetAfterPruning(OrderByStatistics order_by, OrderType order_type,
	                                                 OrderByNullType null_order, const StorageIndex &column_idx,
	                                                 idx_t row_offset, bool single_order_key,
	                                                 vector<PartitionStatistics> &stats);

private:
	const RowGroupOrderOptions options;
	const TransactionData transaction;

	idx_t offset;
	bool initialized;
	vector<reference<SegmentNode<RowGroup>>> ordered_row_groups;
};

} // namespace duckdb
