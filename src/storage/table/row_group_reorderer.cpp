#include "duckdb/storage/table/row_group_reorderer.hpp"

#include "duckdb/common/bswap.hpp"
#include "duckdb/common/radix.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

namespace {

constexpr idx_t STRING_PREFIX_SIZE = StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE;

template <class T>
bool EncodeNumeric(const BaseStatistics &stats, bool min, data_ptr_t bytes) {
	if (!NumericStats::HasMinMax(stats)) {
		return false;
	}
	Radix::EncodeData<T>(bytes, min ? NumericStats::GetMinUnsafe<T>(stats) : NumericStats::GetMaxUnsafe<T>(stats));
	return true;
}

bool EncodeString(const BaseStatistics &stats, bool min, data_ptr_t bytes) {
	if (!StringStats::HasMinMax(stats)) {
		return false;
	}
	const auto value = min ? StringStats::GetMinUnsafe(stats) : StringStats::GetMaxUnsafe(stats);
	const idx_t size = value.size();
	memcpy(bytes, value.data(), MinValue(size, STRING_PREFIX_SIZE));
	bytes[STRING_PREFIX_SIZE] = static_cast<data_t>(MinValue(size, STRING_PREFIX_SIZE + 1));
	return true;
}

bool Encode(const BaseStatistics &stats, bool min, data_ptr_t bytes) {
	switch (stats.GetType().InternalType()) {
	case PhysicalType::INT8:
		return EncodeNumeric<int8_t>(stats, min, bytes);
	case PhysicalType::INT16:
		return EncodeNumeric<int16_t>(stats, min, bytes);
	case PhysicalType::INT32:
		return EncodeNumeric<int32_t>(stats, min, bytes);
	case PhysicalType::INT64:
		return EncodeNumeric<int64_t>(stats, min, bytes);
	case PhysicalType::INT128:
		return EncodeNumeric<hugeint_t>(stats, min, bytes);
	case PhysicalType::UINT8:
		return EncodeNumeric<uint8_t>(stats, min, bytes);
	case PhysicalType::UINT16:
		return EncodeNumeric<uint16_t>(stats, min, bytes);
	case PhysicalType::UINT32:
		return EncodeNumeric<uint32_t>(stats, min, bytes);
	case PhysicalType::UINT64:
		return EncodeNumeric<uint64_t>(stats, min, bytes);
	case PhysicalType::UINT128:
		return EncodeNumeric<uhugeint_t>(stats, min, bytes);
	case PhysicalType::FLOAT:
		return EncodeNumeric<float>(stats, min, bytes);
	case PhysicalType::DOUBLE:
		return EncodeNumeric<double>(stats, min, bytes);
	case PhysicalType::VARCHAR:
		return EncodeString(stats, min, bytes);
	default:
		throw InternalException("RowGroupOrderKey: unsupported type %s", stats.GetType().ToString());
	}
}

struct RowGroupSegmentNodeEntry {
	reference<SegmentNode<RowGroup>> row_group;
	unique_ptr<BaseStatistics> stats;
};

struct RowGroupOffsetEntry {
	idx_t count;
	unique_ptr<BaseStatistics> stats;
};

using RowGroupMap = multimap<RowGroupOrderKey, RowGroupSegmentNodeEntry>;
using RowGroupOffsetMap = multimap<RowGroupOrderKey, RowGroupOffsetEntry>;

bool IsNullOnly(const BaseStatistics &stats) {
	return !stats.CanHaveNoNull();
}

RowGroupOrderKey StatKey(const BaseStatistics &stats, const OrderByStatistics order_by, const OrderType order_type) {
	return RowGroupOrderKey::Get(stats, order_by, order_type, OrderByNullType::NULLS_LAST);
}

OrderByStatistics Opposite(const OrderByStatistics order_by) {
	return order_by == OrderByStatistics::MAX ? OrderByStatistics::MIN : OrderByStatistics::MAX;
}

idx_t GetMinimumQualifyingTupleCount(RowGroup &row_group, BaseStatistics &stats, const OrderByColumnType type,
                                     TransactionData transaction) {
	if (!stats.CanHaveNull()) {
		return row_group.GetVisibleRowCount(transaction);
	}

	if (type == OrderByColumnType::NUMERIC) {
		if (!NumericStats::HasMinMax(stats)) {
			return 0;
		}
		if (NumericStats::IsConstant(stats)) {
			return 1;
		}
		return 2;
	}
	// We cannot check if the min/max for StringStats have actually been set. As the strings may be truncated, we
	// also cannot assume that min and max are the same
	return 0;
}

bool AddRowGroups(RowGroupMap::iterator it, const RowGroupMap::iterator end,
                  vector<reference<SegmentNode<RowGroup>>> &ordered_row_groups, const idx_t row_limit,
                  const OrderByColumnType column_type, const OrderByStatistics order_by, const OrderType order_type,
                  TransactionData transaction) {
	const auto opposite_stat_type = Opposite(order_by);

	auto last_unresolved_entry = it;
	auto &last_stats = it->second.stats;
	auto last_unresolved_boundary = StatKey(*last_stats, opposite_stat_type, order_type);

	// Try to find row groups that can be excluded with limit
	idx_t qualifying_tuples = 0;
	idx_t qualify_later = 0;

	idx_t last_unresolved_row_group_sum =
	    GetMinimumQualifyingTupleCount(it->second.row_group.get().GetNode(), *last_stats, column_type, transaction);
	for (; it != end; ++it) {
		auto &current_key = it->first;
		auto &row_group = it->second.row_group;

		while (last_unresolved_entry != it) {
			if (current_key <= last_unresolved_boundary) {
				if (current_key != std::prev(it)->first) {
					// Row groups overlap: we can only guarantee one additional qualifying tuple
					qualifying_tuples += qualify_later;
					qualify_later = 0;
					qualifying_tuples++;
				} else {
					// Row groups have the same order value, we can only guarantee a qualifying tuple later
					qualify_later++;
				}

				break;
			}
			// Row groups do not overlap: we can guarantee that the tuples qualify
			qualifying_tuples = last_unresolved_row_group_sum;
			++last_unresolved_entry;
			auto &upcoming_row_group = last_unresolved_entry->second.row_group.get().GetNode();
			auto &upcoming_stats = *last_unresolved_entry->second.stats;

			last_unresolved_row_group_sum +=
			    GetMinimumQualifyingTupleCount(upcoming_row_group, upcoming_stats, column_type, transaction);
			last_unresolved_boundary = StatKey(upcoming_stats, opposite_stat_type, order_type);
		}
		if (qualifying_tuples >= row_limit) {
			return true;
		}
		ordered_row_groups.emplace_back(row_group);
	}
	return false;
}

bool SetRowGroupVector(RowGroupMap &row_group_map, const optional_idx row_limit, const idx_t row_group_offset,
                       const OrderByStatistics order_by, const OrderType order_type,
                       const OrderByColumnType column_type,
                       vector<reference<SegmentNode<RowGroup>>> &ordered_row_groups, TransactionData transaction) {
	const auto end = row_group_map.end();
	auto it = std::next(row_group_map.begin(), NumericCast<int64_t>(MinValue(row_group_offset, row_group_map.size())));
	if (it == end) {
		return false;
	}
	if (row_limit.IsValid()) {
		return AddRowGroups(it, end, ordered_row_groups, row_limit.GetIndex(), column_type, order_by, order_type,
		                    transaction);
	}
	for (; it != end; ++it) {
		ordered_row_groups.push_back(it->second.row_group);
	}
	return false;
}

void AppendRowGroups(const vector<reference<SegmentNode<RowGroup>>> &source, idx_t offset,
                     vector<reference<SegmentNode<RowGroup>>> &target) {
	for (idx_t i = offset; i < source.size(); i++) {
		target.push_back(source[i]);
	}
}

OffsetPruningResult FindOffsetPrunableChunks(const RowGroupOffsetMap &row_groups, const OrderByStatistics order_by,
                                             const OrderType order_type, const idx_t row_offset) {
	auto it = row_groups.begin();
	const auto end = row_groups.end();
	if (it == end) {
		return {row_offset, 0, 0};
	}
	const auto opposite_stat_type = Opposite(order_by);

	auto last_unresolved_entry = it;
	auto last_unresolved_boundary = StatKey(*it->second.stats, opposite_stat_type, order_type);

	// Try to find row groups that can be excluded with offset
	idx_t seen_tuples = 0;
	idx_t new_row_offset = row_offset;
	idx_t pruned_row_group_count = 0;

	for (; it != end; ++it) {
		auto &current_key = it->first;
		auto tuple_count = it->second.count;
		seen_tuples += tuple_count;

		while (last_unresolved_entry != it) {
			if (current_key <= last_unresolved_boundary) {
				break;
			}
			// Row groups do not overlap
			auto &pruned_stats = last_unresolved_entry->second.stats;
			if (pruned_stats->CanHaveNull()) {
				// Offset pruning can only remove a contiguous prefix of row groups. Once the first unresolved
				// row group might still contribute NULLs to the ordered prefix, we cannot prune past it.
				return {new_row_offset, pruned_row_group_count, 0};
			}
			// This row group has exactly row_group.count valid values. We can exclude those.
			pruned_row_group_count++;
			new_row_offset -= last_unresolved_entry->second.count;

			++last_unresolved_entry;
			if (last_unresolved_entry == end) {
				return {new_row_offset, pruned_row_group_count, 0};
			}
			auto &upcoming_stats = *last_unresolved_entry->second.stats;
			last_unresolved_boundary = StatKey(upcoming_stats, opposite_stat_type, order_type);
		}

		if (seen_tuples > row_offset) {
			break;
		}
	}

	return {new_row_offset, pruned_row_group_count, 0};
}

} // namespace

RowGroupOrderKey RowGroupOrderKey::Null(OrderByNullType null_order) {
	RowGroupOrderKey key;
	key.part0 = (null_order == OrderByNullType::NULLS_FIRST ? NULLS_FIRST_MARKER : NULLS_LAST_MARKER) << MARKER_SHIFT;
	return key;
}

RowGroupOrderKey RowGroupOrderKey::Get(const BaseStatistics &stats, OrderByStatistics order_by, OrderType order_type,
                                       OrderByNullType null_order) {
	data_t bytes[sizeof(RowGroupOrderKey)] = {VALUE_MARKER};
	if (!Encode(stats, order_by == OrderByStatistics::MIN, bytes + 1)) {
		return Null(null_order);
	}
	RowGroupOrderKey key;
	key.part0 = BSwapIfLE(Load<uint64_t>(bytes));
	key.part1 = BSwapIfLE(Load<uint64_t>(bytes + sizeof(uint64_t)));
	key.part2 = BSwapIfLE(Load<uint64_t>(bytes + 2 * sizeof(uint64_t)));
	if (order_type == OrderType::DESCENDING) {
		key.part0 ^= ~(~uint64_t(0) << MARKER_SHIFT);
		key.part1 = ~key.part1;
		key.part2 = ~key.part2;
	}
	return key;
}

RowGroupReorderer::RowGroupReorderer(const RowGroupOrderOptions &options_p, TransactionData transaction_p)
    : options(options_p), transaction(transaction_p), offset(0), initialized(false) {
}

idx_t RowGroupReorderer::ScanRowCount() const {
	D_ASSERT(initialized);
	idx_t count = 0;
	for (auto &row_group : ordered_row_groups) {
		count += row_group.get().GetNode().count;
	}
	return count;
}

optional_ptr<SegmentNode<RowGroup>> RowGroupReorderer::GetNextRowGroup(SegmentNode<RowGroup> &row_group) {
	D_ASSERT(RefersToSameObject(ordered_row_groups[offset].get(), row_group));
	if (offset >= ordered_row_groups.size() - 1) {
		return nullptr;
	}
	return ordered_row_groups[++offset].get();
}

OffsetPruningResult RowGroupReorderer::GetOffsetAfterPruning(const OrderByStatistics order_by,
                                                             const OrderType order_type,
                                                             const OrderByNullType null_order,
                                                             const StorageIndex &storage_index, const idx_t row_offset,
                                                             vector<PartitionStatistics> &stats) {
	RowGroupOffsetMap ordered_row_groups;
	idx_t new_row_offset = row_offset;
	idx_t leading_null_group_offset = 0;
	bool encountered_maybe_null_group = false;

	for (auto &partition_stats : stats) {
		if (partition_stats.count_type == CountType::COUNT_APPROXIMATE || !partition_stats.partition_row_group) {
			return {row_offset, 0, 0};
		}

		if (!partition_stats.partition_row_group->MinMaxIsExact(storage_index)) {
			return {row_offset, 0, 0};
		}
		auto column_stats = partition_stats.partition_row_group->GetColumnStatistics(storage_index);
		if (!column_stats) {
			return {new_row_offset, 0, leading_null_group_offset};
		}
		if (null_order == OrderByNullType::NULLS_FIRST && IsNullOnly(*column_stats)) {
			if (new_row_offset < partition_stats.count) {
				return {new_row_offset, 0, leading_null_group_offset};
			}
			new_row_offset -= partition_stats.count;
			leading_null_group_offset++;
			continue;
		}
		auto comparison_value = StatKey(*column_stats, order_by, order_type);
		if (comparison_value.IsNull()) {
			if (null_order == OrderByNullType::NULLS_LAST && IsNullOnly(*column_stats)) {
				// With NULLS_LAST, null-stats row groups are scanned after all non-null row groups.
				// They fall outside the offset range being pruned here, so skip them.
				continue;
			}
			// The row group might still contribute ordered values, but we cannot position it safely.
			if (null_order == OrderByNullType::NULLS_FIRST) {
				encountered_maybe_null_group = true;
				continue;
			}
			return {new_row_offset, 0, leading_null_group_offset};
		}
		if (null_order == OrderByNullType::NULLS_FIRST && column_stats->CanHaveNull()) {
			// Groups that might contain NULLs are scanned before definitely non-null groups with NULLS_FIRST.
			// Without exact NULL counts, they block further offset pruning into the non-null region.
			encountered_maybe_null_group = true;
			continue;
		}
		auto entry = RowGroupOffsetEntry {partition_stats.count, std::move(column_stats)};
		ordered_row_groups.emplace(comparison_value, std::move(entry));
	}
	if (null_order == OrderByNullType::NULLS_FIRST && encountered_maybe_null_group) {
		return {new_row_offset, 0, leading_null_group_offset};
	}

	auto result = FindOffsetPrunableChunks(ordered_row_groups, order_by, order_type, new_row_offset);
	result.leading_null_group_offset = leading_null_group_offset;
	return result;
}

optional_ptr<SegmentNode<RowGroup>> RowGroupReorderer::GetRootSegment(RowGroupSegmentTree &row_groups) {
	if (initialized) {
		if (ordered_row_groups.empty()) {
			return nullptr;
		}
		return ordered_row_groups[0].get();
	}

	initialized = true;

	vector<reference<SegmentNode<RowGroup>>> null_only_groups;
	vector<reference<SegmentNode<RowGroup>>> ambiguous_groups;
	RowGroupMap row_group_map;
	for (auto &row_group : row_groups.SegmentNodes()) {
		auto stats = row_group.GetNode().GetStatistics(options.column_idx);
		if (!stats) {
			ambiguous_groups.push_back(row_group);
			continue;
		}
		if (IsNullOnly(*stats)) {
			null_only_groups.push_back(row_group);
			continue;
		}
		auto comparison_value = StatKey(*stats, options.order_by, options.order_type);
		if (comparison_value.IsNull() || (options.null_order == OrderByNullType::NULLS_FIRST && stats->CanHaveNull())) {
			// No trustworthy ordering statistics, or the row group might still contribute NULLs that must be scanned
			// before definitely non-null groups with NULLS_FIRST.
			ambiguous_groups.push_back(row_group);
			continue;
		}
		auto entry = RowGroupSegmentNodeEntry {row_group, std::move(stats)};
		row_group_map.emplace(comparison_value, std::move(entry));
	}

	if (options.null_order == OrderByNullType::NULLS_FIRST) {
		AppendRowGroups(null_only_groups, options.leading_null_group_offset, ordered_row_groups);
		AppendRowGroups(ambiguous_groups, 0, ordered_row_groups);
		SetRowGroupVector(row_group_map, options.row_limit, options.row_group_offset, options.order_by,
		                  options.order_type, options.column_type, ordered_row_groups, transaction);
	} else {
		auto pruned_by_limit =
		    SetRowGroupVector(row_group_map, options.row_limit, options.row_group_offset, options.order_by,
		                      options.order_type, options.column_type, ordered_row_groups, transaction);
		AppendRowGroups(ambiguous_groups, 0, ordered_row_groups);
		// Once an ascending NULLS LAST limit is already satisfied by earlier non-null row groups, null-only
		// groups cannot contribute to the ordered prefix anymore.
		if (!pruned_by_limit || options.order_type != OrderType::ASCENDING) {
			AppendRowGroups(null_only_groups, 0, ordered_row_groups);
		}
	}

	if (ordered_row_groups.empty()) {
		return nullptr;
	}

	return ordered_row_groups[0].get();
}

} // namespace duckdb
