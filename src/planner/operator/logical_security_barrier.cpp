#include "duckdb/planner/operator/logical_security_barrier.hpp"

namespace duckdb {

LogicalSecurityBarrier::LogicalSecurityBarrier() : LogicalOperator(LogicalOperatorType::LOGICAL_SECURITY_BARRIER) {
}

LogicalSecurityBarrier::LogicalSecurityBarrier(unique_ptr<LogicalOperator> child)
    : LogicalOperator(LogicalOperatorType::LOGICAL_SECURITY_BARRIER) {
	children.push_back(std::move(child));
}

vector<ColumnBinding> LogicalSecurityBarrier::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

void LogicalSecurityBarrier::ResolveTypes() {
	types = children[0]->types;
}

bool LogicalSecurityBarrier::CanCross(const Expression &expr) {
	return !expr.CanThrow() && !expr.IsVolatile();
}

} // namespace duckdb
