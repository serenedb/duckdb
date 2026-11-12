#include "duckdb/planner/operator/logical_window.hpp"

#include "duckdb/main/config.hpp"

namespace duckdb {

vector<ColumnBinding> LogicalWindow::GetColumnBindings() {
	return GetDerivedColumnBindings();
}

optional_ptr<LogicalOperator> LogicalWindow::GetBindingChild() {
	return children[0].get();
}

void LogicalWindow::DeriveColumnBindings(vector<ColumnBinding> &bindings) {
	for (auto window_col_idx : ProjectionIndex::GetIndexes(expressions.size())) {
		bindings.emplace_back(window_index, window_col_idx);
	}
}

void LogicalWindow::ResolveTypes() {
	types.insert(types.end(), children[0]->types.begin(), children[0]->types.end());
	for (auto &expr : expressions) {
		types.push_back(expr->GetReturnType());
	}
}

vector<TableIndex> LogicalWindow::GetTableIndex() const {
	return vector<TableIndex> {window_index};
}

string LogicalWindow::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", window_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

} // namespace duckdb
