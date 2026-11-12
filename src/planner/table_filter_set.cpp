#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/common/exception.hpp"

#include <ranges>

namespace duckdb {

TableFilterSet::ConstTableFilterIteratorEntry::ConstTableFilterIteratorEntry(
    map<ProjectionIndex, unique_ptr<TableFilter>>::const_iterator it)
    : iterator(it) {
}

ProjectionIndex TableFilterSet::ConstTableFilterIteratorEntry::GetIndex() const {
	return iterator->first;
}

const TableFilter &TableFilterSet::ConstTableFilterIteratorEntry::Filter() const {
	return *iterator->second;
}

TableFilterSet::TableFilterIteratorEntry::TableFilterIteratorEntry(
    map<ProjectionIndex, unique_ptr<TableFilter>>::iterator it)
    : iterator(it) {
}

ProjectionIndex TableFilterSet::TableFilterIteratorEntry::GetIndex() const {
	return iterator->first;
}

TableFilter &TableFilterSet::TableFilterIteratorEntry::Filter() {
	return *iterator->second;
}

const TableFilter &TableFilterSet::TableFilterIteratorEntry::Filter() const {
	return *iterator->second;
}

unique_ptr<TableFilter> TableFilterSet::TableFilterIteratorEntry::TakeFilter() {
	return std::move(iterator->second);
}

bool TableFilterSet::HasFilters() const {
	return !filters.empty();
}
idx_t TableFilterSet::FilterCount() const {
	return filters.size();
}
bool TableFilterSet::HasMultiColumnFilters() const {
	return !multi_column_filters.empty();
}
bool TableFilterSet::HasFilter(ProjectionIndex col_idx) const {
	return filters.find(col_idx) != filters.end();
}

const TableFilter &TableFilterSet::GetFilterByColumnIndex(ProjectionIndex col_idx) const {
	auto filter = TryGetFilterByColumnIndex(col_idx);
	if (!filter) {
		throw InternalException("Table filter set does not have a filter for column idx %d", col_idx);
	}
	return *filter;
}

optional_ptr<const TableFilter> TableFilterSet::TryGetFilterByColumnIndex(ProjectionIndex col_idx) const {
	if (!col_idx.IsValid()) {
		throw InternalException("TableFilterSet::TryGetFilterByColumnIndex called with invalid column index");
	}
	auto entry = filters.find(col_idx);
	if (entry == filters.end()) {
		return nullptr;
	}
	return *entry->second;
}

TableFilter &TableFilterSet::GetFilterByColumnIndexMutable(ProjectionIndex col_idx) {
	auto filter = TryGetFilterByColumnIndexMutable(col_idx);
	if (!filter) {
		throw InternalException("Table filter set does not have a filter for column idx %d", col_idx);
	}
	return *filter;
}

optional_ptr<TableFilter> TableFilterSet::TryGetFilterByColumnIndexMutable(ProjectionIndex col_idx) {
	auto entry = filters.find(col_idx);
	if (entry == filters.end()) {
		return nullptr;
	}
	return *entry->second;
}

void TableFilterSet::RemoveFilterByColumnIndex(ProjectionIndex col_idx) {
	filters.erase(col_idx);
}

void TableFilterSet::SetFilterByColumnIndex(ProjectionIndex col_idx, unique_ptr<TableFilter> filter) {
	filters[col_idx] = std::move(filter);
}

void TableFilterSet::ClearFilters() {
	filters.clear();
	multi_column_filters.clear();
}

bool TableFilterSet::Equals(TableFilterSet &other) {
	if (filters.size() != other.filters.size() || multi_column_filters.size() != other.multi_column_filters.size()) {
		return false;
	}
	for (auto &entry : filters) {
		auto other_entry = other.filters.find(entry.first);
		if (other_entry == other.filters.end()) {
			return false;
		}
		if (!entry.second->Cast<ExpressionFilter>().Equals(other_entry->second->Cast<ExpressionFilter>())) {
			return false;
		}
	}
	for (idx_t filter_idx = 0; filter_idx < multi_column_filters.size(); ++filter_idx) {
		const auto &filter =
		    ExpressionFilter::GetExpressionFilter(*multi_column_filters[filter_idx], "TableFilterSet::Equals");
		const auto &other_filter =
		    ExpressionFilter::GetExpressionFilter(*other.multi_column_filters[filter_idx], "TableFilterSet::Equals");
		if (!filter.Equals(other_filter)) {
			return false;
		}
	}
	return true;
}

bool TableFilterSet::Equals(TableFilterSet *left, TableFilterSet *right) {
	if (left == right) {
		return true;
	}
	if (!left || !right) {
		return false;
	}
	return left->Equals(*right);
}

unique_ptr<TableFilterSet> TableFilterSet::Copy() const {
	auto copy = make_uniq<TableFilterSet>();
	for (auto &it : filters) {
		copy->filters.emplace(it.first, it.second->Cast<ExpressionFilter>().Copy());
	}
	for (const auto &filter : multi_column_filters) {
		copy->multi_column_filters.push_back(
		    ExpressionFilter::GetExpressionFilter(*filter, "TableFilterSet::Copy").Copy());
	}
	return copy;
}

static bool IsConjunctionAnd(const Expression &expr) {
	return expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
}

void TableFilterSet::PushFilter(ProjectionIndex col_idx, unique_ptr<TableFilter> filter) {
	if (!col_idx.IsValid()) {
		throw InternalException("Cannot push a filter over an invalid ProjectionIndex");
	}
	auto &new_filter = ExpressionFilter::GetExpressionFilter(*filter, "TableFilterSet::PushFilter");
	if (!new_filter.column_indexes.empty()) {
		throw InternalException("Cannot push a multi-column filter as a single-column filter");
	}
	auto entry = filters.find(col_idx);
	if (entry == filters.end()) {
		// no filter yet: push the filter directly
		filters[col_idx] = std::move(filter);
		return;
	}
	// there is already a filter: AND it together
	auto &existing = ExpressionFilter::GetExpressionFilter(*entry->second, "TableFilterSet::PushFilter");
	if (IsConjunctionAnd(*existing.expr)) {
		auto &children = existing.expr->Cast<BoundConjunctionExpression>().GetChildrenMutable();
		if (IsConjunctionAnd(*new_filter.expr)) {
			children.append_range(new_filter.expr->Cast<BoundConjunctionExpression>().GetChildrenMutable() |
			                      std::views::as_rvalue);
		} else {
			children.push_back(std::move(new_filter.expr));
		}
		return;
	}
	if (IsConjunctionAnd(*new_filter.expr)) {
		new_filter.expr->Cast<BoundConjunctionExpression>().GetChildrenMutable().push_back(std::move(existing.expr));
		entry->second = std::move(filter);
		return;
	}
	entry->second = make_uniq<ExpressionFilter>(make_uniq<BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(existing.expr), std::move(new_filter.expr)));
}

void TableFilterSet::PushMultiColumnFilter(unique_ptr<TableFilter> filter) {
	const auto &expression_filter =
	    ExpressionFilter::GetExpressionFilter(*filter, "TableFilterSet::PushMultiColumnFilter");
	if (!expression_filter.expr || expression_filter.column_indexes.size() < 2) {
		throw InternalException("Multi-column filter must reference at least two columns");
	}
	for (const auto &column_index : expression_filter.column_indexes) {
		if (!column_index.IsValid()) {
			throw InternalException("Cannot push a multi-column filter over an invalid ProjectionIndex");
		}
	}
	for (const auto &existing_filter : multi_column_filters) {
		if (ExpressionFilter::GetExpressionFilter(*existing_filter, "TableFilterSet::PushMultiColumnFilter")
		        .Equals(expression_filter)) {
			return;
		}
	}
	multi_column_filters.push_back(std::move(filter));
}

const vector<unique_ptr<TableFilter>> &TableFilterSet::GetMultiColumnFilters() const {
	return multi_column_filters;
}

void DynamicTableFilterSet::ClearFilters(const PhysicalOperator &op) {
	lock_guard<mutex> l(lock);
	filters.erase(op);
}

void DynamicTableFilterSet::PushFilter(const PhysicalOperator &op, ProjectionIndex column_index,
                                       unique_ptr<TableFilter> filter) {
	lock_guard<mutex> l(lock);
	auto entry = filters.find(op);
	optional_ptr<TableFilterSet> filter_ptr;
	if (entry == filters.end()) {
		auto filter_set = make_uniq<TableFilterSet>();
		filter_ptr = filter_set.get();
		filters[op] = std::move(filter_set);
	} else {
		filter_ptr = entry->second.get();
	}
	filter_ptr->PushFilter(column_index, std::move(filter));
}

bool DynamicTableFilterSet::HasFilters() const {
	lock_guard<mutex> l(lock);
	return !filters.empty();
}

unique_ptr<TableFilterSet>
DynamicTableFilterSet::GetFinalTableFilters(const PhysicalTableScan &scan,
                                            optional_ptr<TableFilterSet> existing_filters) const {
	lock_guard<mutex> l(lock);
	D_ASSERT(!filters.empty());
	auto result = make_uniq<TableFilterSet>();
	if (existing_filters) {
		for (auto &filter_entry : *existing_filters) {
			result->PushFilter(filter_entry.GetIndex(), filter_entry.Filter().Cast<ExpressionFilter>().Copy());
		}
		for (const auto &filter : existing_filters->GetMultiColumnFilters()) {
			result->PushMultiColumnFilter(
			    ExpressionFilter::GetExpressionFilter(*filter, "DynamicTableFilterSet::GetFinalTableFilters").Copy());
		}
	}
	for (auto &entry : filters) {
		for (auto &filter_entry : *entry.second) {
			result->PushFilter(filter_entry.GetIndex(), filter_entry.Filter().Cast<ExpressionFilter>().Copy());
		}
	}
	if (!result->HasFilters() && !result->HasMultiColumnFilters()) {
		return nullptr;
	}
	return result;
}

} // namespace duckdb
