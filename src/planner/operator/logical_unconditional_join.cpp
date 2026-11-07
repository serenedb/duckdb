#include "duckdb/planner/operator/logical_unconditional_join.hpp"

namespace duckdb {

LogicalUnconditionalJoin::LogicalUnconditionalJoin(LogicalOperatorType logical_type, unique_ptr<LogicalOperator> left,
                                                   unique_ptr<LogicalOperator> right)
    : LogicalOperator(logical_type) {
	D_ASSERT(left);
	D_ASSERT(right);
	children.push_back(std::move(left));
	children.push_back(std::move(right));
}

vector<ColumnBinding> LogicalUnconditionalJoin::GetColumnBindings() {
	return GetDerivedColumnBindings();
}

optional_ptr<LogicalOperator> LogicalUnconditionalJoin::GetBindingChild() {
	return children[0].get();
}

void LogicalUnconditionalJoin::DeriveColumnBindings(vector<ColumnBinding> &bindings) {
	auto right_bindings = children[1]->GetColumnBindings();
	bindings.insert(bindings.end(), right_bindings.begin(), right_bindings.end());
}

void LogicalUnconditionalJoin::ResolveTypes() {
	types.insert(types.end(), children[0]->types.begin(), children[0]->types.end());
	types.insert(types.end(), children[1]->types.begin(), children[1]->types.end());
}

} // namespace duckdb
