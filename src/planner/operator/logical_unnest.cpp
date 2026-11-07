#include "duckdb/planner/operator/logical_unnest.hpp"

#include "duckdb/main/config.hpp"

namespace duckdb {

vector<ColumnBinding> LogicalUnnest::GetColumnBindings() {
	return GetDerivedColumnBindings();
}

optional_ptr<LogicalOperator> LogicalUnnest::GetBindingChild() {
	return children[0].get();
}

void LogicalUnnest::DeriveColumnBindings(vector<ColumnBinding> &bindings) {
	for (auto unnest_col_idx : ProjectionIndex::GetIndexes(expressions.size())) {
		bindings.emplace_back(unnest_index, unnest_col_idx);
	}
}

void LogicalUnnest::ResolveTypes() {
	types.insert(types.end(), children[0]->types.begin(), children[0]->types.end());
	for (auto &expr : expressions) {
		types.push_back(expr->GetReturnType());
	}
}

vector<TableIndex> LogicalUnnest::GetTableIndex() const {
	return vector<TableIndex> {unnest_index};
}

string LogicalUnnest::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", unnest_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

} // namespace duckdb
