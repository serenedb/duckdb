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
	auto &source_scan = source.Cast<PhysicalTableScan>();
	source_scan.estimated_cardinality = ids.estimated_cardinality;
	if (lookup.to_string) {
		source_scan.function.to_string = lookup.to_string;
	}
	if (lookup.to_string_value) {
		source_scan.function.to_string_value = lookup.to_string_value;
	}
	vector<idx_t> row_id_columns;
	for (auto &expr : op.expressions) {
		row_id_columns.push_back(expr->Cast<BoundReferenceExpression>().Index());
	}
	return Make<PhysicalLookupJoin>(op.types, ids, source_scan, std::move(lookup), std::move(row_id_columns),
	                                ids.estimated_cardinality);
}

} // namespace duckdb
