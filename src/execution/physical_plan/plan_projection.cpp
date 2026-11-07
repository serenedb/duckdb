#include "duckdb/execution/operator/join/physical_lookup_join.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

static bool TryProjectColumns(PhysicalOperator &plan, const vector<idx_t> &columns) {
	switch (plan.type) {
	case PhysicalOperatorType::TABLE_SCAN: {
		auto &scan = plan.Cast<PhysicalTableScan>();
		if (!scan.function.projection_pushdown || !scan.function.filter_prune) {
			return false;
		}
		vector<idx_t> projection_ids;
		for (auto column : columns) {
			projection_ids.push_back(scan.projection_ids.empty() ? column : scan.projection_ids[column]);
		}
		scan.projection_ids = std::move(projection_ids);
		return true;
	}
	case PhysicalOperatorType::LOOKUP_JOIN: {
		auto &join = plan.Cast<PhysicalLookupJoin>();
		vector<idx_t> output_columns;
		for (auto column : columns) {
			output_columns.push_back(join.output_columns[column]);
		}
		join.output_columns = std::move(output_columns);
		return true;
	}
	default:
		return false;
	}
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalProjection &op) {
	D_ASSERT(op.children.size() == 1);
	auto &plan = CreatePlan(*op.children[0]);

#ifdef DEBUG
	for (auto &expr : op.expressions) {
		D_ASSERT(!expr->IsWindow());
		D_ASSERT(!expr->IsAggregate());
	}
#endif
	if (plan.GetTypes().size() == op.types.size()) {
		// check if this projection can be omitted entirely
		// this happens if a projection simply emits the columns in the same order
		// e.g. PROJECTION(#0, #1, #2, #3, ...)
		bool omit_projection = true;
		for (idx_t i = 0; i < op.types.size(); i++) {
			if (op.expressions[i]->GetExpressionType() == ExpressionType::BOUND_REF) {
				auto &bound_ref = op.expressions[i]->Cast<BoundReferenceExpression>();
				if (bound_ref.Index() == i) {
					continue;
				}
			}
			omit_projection = false;
			break;
		}
		if (omit_projection) {
			// the projection only directly projects the child' columns: omit it entirely
			return plan;
		}
	}
	vector<idx_t> columns;
	for (auto &expr : op.expressions) {
		if (expr->GetExpressionType() != ExpressionType::BOUND_REF) {
			break;
		}
		columns.push_back(expr->Cast<BoundReferenceExpression>().Index());
	}
	if (!columns.empty() && columns.size() == op.expressions.size() && TryProjectColumns(plan, columns)) {
		plan.types = op.types;
		return plan;
	}

	auto &proj = Make<PhysicalProjection>(op.types, std::move(op.expressions), op.estimated_cardinality);
	proj.children.push_back(plan);
	return proj;
}

} // namespace duckdb
