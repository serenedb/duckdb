#include "duckdb/storage/table/row_group_reorderer.hpp"

#include "duckdb/common/type_util.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

namespace {

constexpr idx_t STRING_PREFIX_SIZE = StringStatsData::CURRENT_MAX_STRING_MINMAX_SIZE;
static_assert(1 + STRING_PREFIX_SIZE + 1 <= sizeof(RowGroupOrderKey));

template <class T>
bool EncodeNumeric(const BaseStatistics &stats, bool min, OrderModifiers modifiers, data_ptr_t key) {
	if (!NumericStats::HasMinMax(stats)) {
		return false;
	}
	const T value = min ? NumericStats::GetMinUnsafe<T>(stats) : NumericStats::GetMaxUnsafe<T>(stats);
	CreateSortKeyHelpers::CreateSortKey(stats.GetType(), const_data_ptr_cast(&value), modifiers, key);
	return true;
}

bool EncodeString(const BaseStatistics &stats, bool min, OrderModifiers modifiers, data_ptr_t key) {
	if (stats.GetType().id() != LogicalTypeId::VARCHAR) {
		throw InternalException("RowGroupOrderKey: unsupported type %s", stats.GetType().ToString());
	}
	if (!StringStats::HasMinMax(stats)) {
		return false;
	}
	const auto value = min ? StringStats::GetMinUnsafe(stats) : StringStats::GetMaxUnsafe(stats);
	const string_t prefix(value.data(), UnsafeNumericCast<uint32_t>(MinValue<idx_t>(value.size(), STRING_PREFIX_SIZE)));
	CreateSortKeyHelpers::CreateSortKey(stats.GetType(), const_data_ptr_cast(&prefix), modifiers, key);
	return true;
}

bool Encode(const BaseStatistics &stats, bool min, OrderModifiers modifiers, data_ptr_t key) {
	switch (stats.GetType().InternalType()) {
	case PhysicalType::VARCHAR:
		return EncodeString(stats, min, modifiers, key);
	case PhysicalType::BOOL:
	case PhysicalType::INTERVAL:
		throw InternalException("RowGroupOrderKey: unsupported type %s", stats.GetType().ToString());
	default:
		return VisitPhysicalType(stats.GetType().InternalType(),
		                         [&]<class T>() { return EncodeNumeric<T>(stats, min, modifiers, key); });
	}
}

struct RowGroupOffsetEntry {
	idx_t count;
	optionally_owned_ptr<const BaseStatistics> stats;
};

//! The partitions ordered by the statistics of the order column - the key of each, its index and those statistics,
//! sorted by key and then by index
using PartitionMap = vector<pair<RowGroupOrderKey, pair<idx_t, reference<const BaseStatistics>>>>;
using RowGroupOffsetMap = multimap<RowGroupOrderKey, RowGroupOffsetEntry>;

bool IsNullOnly(const BaseStatistics &stats) {
	return !stats.CanHaveNoNull();
}

RowGroupOrderKey StatKey(const BaseStatistics &stats, const OrderByStatistics order_by, const OrderType order_type) {
	return RowGroupOrderKeys::Get(stats, order_by, order_type, OrderByNullType::NULLS_LAST);
}

bool IsNullStatKey(const RowGroupOrderKey &key) {
	return RowGroupOrderKeys::IsNull(key, OrderByNullType::NULLS_LAST);
}

enum class RowGroupPlacement : uint8_t { NULL_ONLY, AMBIGUOUS, ORDERED };

RowGroupPlacement PlaceRowGroup(optional_ptr<const BaseStatistics> stats, const RowGroupOrderOptions &options,
                                RowGroupOrderKey &key) {
	if (!stats) {
		return RowGroupPlacement::AMBIGUOUS;
	}
	if (IsNullOnly(*stats)) {
		return RowGroupPlacement::NULL_ONLY;
	}
	key = StatKey(*stats, options.order_by, options.order_type);
	if (IsNullStatKey(key) || (options.null_order == OrderByNullType::NULLS_FIRST && stats->CanHaveNull())) {
		// No trustworthy ordering statistics, or the row group might still contribute NULLs that must be scanned
		// before definitely non-null groups with NULLS_FIRST.
		return RowGroupPlacement::AMBIGUOUS;
	}
	return RowGroupPlacement::ORDERED;
}

OrderByStatistics Opposite(const OrderByStatistics order_by) {
	return order_by == OrderByStatistics::MAX ? OrderByStatistics::MIN : OrderByStatistics::MAX;
}

idx_t GetMinimumQualifyingTupleCount(idx_t partition, const BaseStatistics &stats, const OrderByColumnType type,
                                     const RowGroupReorderer::PartitionRowCount &row_count,
                                     const RowGroupReorderer::PartitionMinMaxExact &exact) {
	if (!stats.CanHaveNull()) {
		return row_count(partition);
	}

	if (type == OrderByColumnType::NUMERIC) {
		if (!NumericStats::HasMinMax(stats) || !exact(partition)) {
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

bool AddRowGroups(PartitionMap::iterator it, const PartitionMap::iterator end, vector<idx_t> &ordered_row_groups,
                  const idx_t row_limit, const OrderByColumnType column_type, const OrderByStatistics order_by,
                  const OrderType order_type, const RowGroupReorderer::PartitionRowCount &row_count,
                  const RowGroupReorderer::PartitionMinMaxExact &exact) {
	const auto opposite_stat_type = Opposite(order_by);

	auto last_unresolved_entry = it;
	auto &last_stats = it->second.second.get();
	auto last_unresolved_boundary = StatKey(last_stats, opposite_stat_type, order_type);

	// Try to find row groups that can be excluded with limit
	idx_t qualifying_tuples = 0;
	idx_t qualify_later = 0;

	idx_t last_unresolved_row_group_sum =
	    GetMinimumQualifyingTupleCount(it->second.first, last_stats, column_type, row_count, exact);
	for (; it != end; ++it) {
		auto &current_key = it->first;
		const auto row_group = it->second.first;

		while (last_unresolved_entry != it) {
			if (current_key <= last_unresolved_boundary) {
				const auto previous = std::prev(it);
				const bool holds_boundary_row = exact(previous->second.first);
				if (current_key != previous->first) {
					// Row groups overlap: we can only guarantee one additional qualifying tuple
					qualifying_tuples += qualify_later;
					qualify_later = 0;
					qualifying_tuples += holds_boundary_row;
				} else if (holds_boundary_row) {
					// Row groups have the same order value, we can only guarantee a qualifying tuple later
					qualify_later++;
				}

				break;
			}
			// Row groups do not overlap: we can guarantee that the tuples qualify
			qualifying_tuples = last_unresolved_row_group_sum;
			qualify_later = 0;
			++last_unresolved_entry;
			auto &upcoming_stats = last_unresolved_entry->second.second.get();

			last_unresolved_row_group_sum += GetMinimumQualifyingTupleCount(
			    last_unresolved_entry->second.first, upcoming_stats, column_type, row_count, exact);
			last_unresolved_boundary = StatKey(upcoming_stats, opposite_stat_type, order_type);
		}
		if (qualifying_tuples >= row_limit) {
			return true;
		}
		ordered_row_groups.push_back(row_group);
	}
	return false;
}

bool SetRowGroupVector(PartitionMap &row_group_map, const optional_idx row_limit, const idx_t row_group_offset,
                       const OrderByStatistics order_by, const OrderType order_type,
                       const OrderByColumnType column_type, vector<idx_t> &ordered_row_groups,
                       const RowGroupReorderer::PartitionRowCount &row_count,
                       const RowGroupReorderer::PartitionMinMaxExact &exact) {
	const auto end = row_group_map.end();
	auto it = std::next(row_group_map.begin(), NumericCast<int64_t>(MinValue(row_group_offset, row_group_map.size())));
	if (it == end) {
		return false;
	}
	if (row_limit.IsValid()) {
		return AddRowGroups(it, end, ordered_row_groups, row_limit.GetIndex(), column_type, order_by, order_type,
		                    row_count, exact);
	}
	for (; it != end; ++it) {
		ordered_row_groups.push_back(it->second.first);
	}
	return false;
}

void AppendRowGroups(const vector<idx_t> &source, idx_t offset, vector<idx_t> &target) {
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

RowGroupOrderKey RowGroupOrderKeys::Get(const BaseStatistics &stats, OrderByStatistics order_by, OrderType order_type,
                                        OrderByNullType null_order) {
	auto key = RowGroupOrderKey();
	if (!Encode(stats, order_by == OrderByStatistics::MIN, OrderModifiers(order_type, null_order),
	            data_ptr_cast(&key.part0))) {
		return Null(null_order);
	}
	key.ByteSwap();
	return key;
}

RowGroupOrderKey RowGroupOrderKeys::Get(const Value &value, OrderType order_type, OrderByNullType null_order) {
	if (value.IsNull()) {
		return Null(null_order);
	}
	auto key = RowGroupOrderKey();
	const OrderModifiers modifiers(order_type, null_order);
	if (value.type().InternalType() == PhysicalType::VARCHAR) {
		// the key of a string holds its prefix, like the keys of string statistics do
		auto &str = StringValue::Get(value);
		const string_t prefix(str.data(), UnsafeNumericCast<uint32_t>(MinValue<idx_t>(str.size(), STRING_PREFIX_SIZE)));
		CreateSortKeyHelpers::CreateSortKey(value.type(), const_data_ptr_cast(&prefix), modifiers,
		                                    data_ptr_cast(&key.part0));
	} else {
		VisitPhysicalType(value.type().InternalType(), [&]<class T>() {
			const auto data = value.GetValueUnsafe<T>();
			CreateSortKeyHelpers::CreateSortKey(value.type(), const_data_ptr_cast(&data), modifiers,
			                                    data_ptr_cast(&key.part0));
		});
	}
	key.ByteSwap();
	return key;
}

RowGroupOrderKey RowGroupOrderKeys::Null(OrderByNullType null_order) {
	auto key = RowGroupOrderKey();
	CreateSortKeyHelpers::CreateSortKey(LogicalType::SQLNULL, nullptr, OrderModifiers(OrderType::ASCENDING, null_order),
	                                    data_ptr_cast(&key.part0));
	key.ByteSwap();
	return key;
}

bool RowGroupOrderKeys::IsNull(const RowGroupOrderKey &key, OrderByNullType null_order) {
	const auto first_byte = static_cast<data_t>(key.part0 >> 56);
	return CreateSortKeyHelpers::IsNullSortKey(&first_byte, null_order);
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
                                                             const bool single_order_key,
                                                             vector<PartitionStatistics> &stats) {
	RowGroupOffsetMap ordered_row_groups;
	idx_t new_row_offset = row_offset;
	idx_t leading_null_group_offset = 0;
	bool encountered_maybe_null_group = false;
	const auto stop = [&]() -> OffsetPruningResult {
		if (leading_null_group_offset > 0 && !single_order_key) {
			return {row_offset, 0, 0};
		}
		return {new_row_offset, 0, leading_null_group_offset};
	};

	for (auto &partition_stats : stats) {
		if (partition_stats.count_type == CountType::COUNT_APPROXIMATE || !partition_stats.partition_row_group) {
			return {row_offset, 0, 0};
		}

		if (!partition_stats.partition_row_group->MinMaxIsExact(storage_index)) {
			return {row_offset, 0, 0};
		}
		auto column_stats = partition_stats.partition_row_group->GetColumnStatistics(storage_index);
		if (!column_stats) {
			return stop();
		}
		if (null_order == OrderByNullType::NULLS_FIRST && IsNullOnly(*column_stats)) {
			if (new_row_offset < partition_stats.count) {
				return stop();
			}
			new_row_offset -= partition_stats.count;
			leading_null_group_offset++;
			continue;
		}
		auto comparison_value = StatKey(*column_stats, order_by, order_type);
		if (IsNullStatKey(comparison_value)) {
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
			return stop();
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
		return stop();
	}

	auto result = FindOffsetPrunableChunks(ordered_row_groups, order_by, order_type, new_row_offset);
	result.leading_null_group_offset = leading_null_group_offset;
	return result;
}

vector<idx_t> RowGroupReorderer::OrderPartitions(const RowGroupOrderOptions &options,
                                                 const vector<optional_ptr<const BaseStatistics>> &statistics,
                                                 const PartitionRowCount &row_count,
                                                 const PartitionMinMaxExact &exact) {
	vector<idx_t> null_only_groups;
	vector<idx_t> ambiguous_groups;
	PartitionMap row_group_map;
	row_group_map.reserve(statistics.size());
	for (idx_t i = 0; i < statistics.size(); i++) {
		auto stats = statistics[i];
		RowGroupOrderKey comparison_value;
		switch (PlaceRowGroup(stats, options, comparison_value)) {
		case RowGroupPlacement::NULL_ONLY:
			null_only_groups.push_back(i);
			break;
		case RowGroupPlacement::AMBIGUOUS:
			ambiguous_groups.push_back(i);
			break;
		case RowGroupPlacement::ORDERED:
			row_group_map.emplace_back(comparison_value, make_pair(i, std::cref(*stats)));
			break;
		}
	}
	std::sort(row_group_map.begin(), row_group_map.end(), [](const auto &lhs, const auto &rhs) {
		return std::tie(lhs.first, lhs.second.first) < std::tie(rhs.first, rhs.second.first);
	});

	vector<idx_t> ordered_row_groups;
	if (options.null_order == OrderByNullType::NULLS_FIRST) {
		AppendRowGroups(null_only_groups, options.leading_null_group_offset, ordered_row_groups);
		AppendRowGroups(ambiguous_groups, 0, ordered_row_groups);
		SetRowGroupVector(row_group_map, options.row_limit, options.row_group_offset, options.order_by,
		                  options.order_type, options.column_type, ordered_row_groups, row_count, exact);
	} else {
		auto pruned_by_limit =
		    SetRowGroupVector(row_group_map, options.row_limit, options.row_group_offset, options.order_by,
		                      options.order_type, options.column_type, ordered_row_groups, row_count, exact);
		AppendRowGroups(ambiguous_groups, 0, ordered_row_groups);
		// Once an ascending NULLS LAST limit is already satisfied by earlier non-null row groups, null-only
		// groups cannot contribute to the ordered prefix anymore.
		if (!pruned_by_limit || options.order_type != OrderType::ASCENDING) {
			AppendRowGroups(null_only_groups, 0, ordered_row_groups);
		}
	}
	return ordered_row_groups;
}

optional_ptr<SegmentNode<RowGroup>> RowGroupReorderer::GetRootSegment(RowGroupSegmentTree &row_groups) {
	if (initialized) {
		if (ordered_row_groups.empty()) {
			return nullptr;
		}
		return ordered_row_groups[0].get();
	}

	initialized = true;

	vector<reference<SegmentNode<RowGroup>>> nodes;
	vector<unique_ptr<BaseStatistics>> owned_statistics;
	vector<optional_ptr<const BaseStatistics>> statistics;
	for (auto &row_group : row_groups.SegmentNodes()) {
		nodes.push_back(row_group);
		owned_statistics.push_back(row_group.GetNode().GetStatistics(options.column_idx));
		statistics.emplace_back(owned_statistics.back().get());
	}
	const auto order = OrderPartitions(
	    options, statistics,
	    [&](idx_t partition) { return nodes[partition].get().GetNode().GetVisibleRowCount(transaction); },
	    [&](idx_t partition) {
		    return nodes[partition].get().GetNode().MinMaxIsExact(transaction, options.column_idx);
	    });
	for (auto partition : order) {
		ordered_row_groups.push_back(nodes[partition]);
	}

	if (ordered_row_groups.empty()) {
		return nullptr;
	}

	return ordered_row_groups[0].get();
}

} // namespace duckdb
