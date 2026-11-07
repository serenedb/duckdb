#include "duckdb/optimizer/relation_statistics/relation_statistics.hpp"

namespace duckdb {

DistinctCount::DistinctCount(idx_t distinct_count, DistinctCountSource source)
    : distinct_count(distinct_count), source(source) {
}

RelationColumnStats::RelationColumnStats(ColumnBinding binding, DistinctCount distinct_count, Identifier name)
    : binding(binding), distinct_count(distinct_count), name(std::move(name)) {
}

RelationStats::RelationStats() : cardinality(1), filter_strength(1), stats_initialized(false) {
}

optional_idx RelationStats::FindColumn(ColumnBinding binding) const {
	for (idx_t column_idx = 0; column_idx < columns.size(); column_idx++) {
		if (columns[column_idx].binding == binding) {
			return column_idx;
		}
	}
	return {};
}

optional_ptr<const RelationColumnStats> RelationStats::GetColumnStats(ColumnBinding binding) const {
	auto column_idx = FindColumn(binding);
	if (!column_idx.IsValid()) {
		return nullptr;
	}
	return columns[column_idx.GetIndex()];
}

RelationColumnLookup::RelationColumnLookup(const RelationStats &stats) : stats(stats) {
	static constexpr idx_t LINEAR_SCAN_COLUMNS = 8;
	if (stats.columns.size() <= LINEAR_SCAN_COLUMNS) {
		return;
	}
	positions.reserve(stats.columns.size());
	for (idx_t column_idx = 0; column_idx < stats.columns.size(); column_idx++) {
		positions.emplace(stats.columns[column_idx].binding, column_idx);
	}
}

optional_ptr<const RelationColumnStats> RelationColumnLookup::GetColumnStats(ColumnBinding binding) const {
	if (positions.empty()) {
		return stats.GetColumnStats(binding);
	}
	auto entry = positions.find(binding);
	if (entry == positions.end()) {
		return nullptr;
	}
	return stats.columns[entry->second];
}

bool RelationStats::MatchesBindings(const vector<ColumnBinding> &bindings) const {
	if (columns.size() != bindings.size()) {
		return false;
	}
	for (idx_t column_idx = 0; column_idx < columns.size(); column_idx++) {
		if (columns[column_idx].binding != bindings[column_idx]) {
			return false;
		}
	}
	return true;
}

void RelationStats::Verify(const vector<ColumnBinding> &bindings) const {
	D_ASSERT(!stats_initialized || MatchesBindings(bindings));
}

} // namespace duckdb
