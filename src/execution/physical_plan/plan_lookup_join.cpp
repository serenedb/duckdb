#include "duckdb/execution/operator/join/physical_lookup_join.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_lookup_join.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalLookupJoin &op) {
	D_ASSERT(op.children.size() == 2);
	auto &ids = CreatePlan(*op.children[0]);
	auto &get = op.GetSource();
	auto lookup = get.function.get_lookup_function(context, get);
	auto &source = CreatePlan(*op.children[1]);
	if (source.type != PhysicalOperatorType::TABLE_SCAN) {
		throw InternalException("LOOKUP_JOIN source must be planned as a table scan");
	}
	vector<idx_t> row_id_columns;
	for (auto &expr : op.expressions) {
		row_id_columns.push_back(expr->Cast<BoundReferenceExpression>().index);
	}
	return Make<PhysicalLookupJoin>(op.types, ids, source.Cast<PhysicalTableScan>(), std::move(lookup),
	                                std::move(row_id_columns), op.estimated_cardinality);
}

} // namespace duckdb
