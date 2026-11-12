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
	auto &source_scan = CreatePlan(*op.children[1]).Cast<PhysicalTableScan>();
	source_scan.estimated_cardinality = op.estimated_cardinality;
	if (lookup.to_string) {
		source_scan.function.to_string = lookup.to_string;
	}
	if (lookup.to_string_value) {
		source_scan.function.to_string_value = lookup.to_string_value;
	}
	vector<idx_t> row_id_columns;
	vector<string> key_names;
	for (auto &expr : op.expressions) {
		row_id_columns.push_back(expr->Cast<BoundReferenceExpression>().Index());
		key_names.push_back(expr->ToString());
	}
	return Make<PhysicalLookupJoin>(op.types, ids, source_scan, std::move(lookup), std::move(row_id_columns),
	                                std::move(key_names), op.estimated_cardinality);
}

} // namespace duckdb
