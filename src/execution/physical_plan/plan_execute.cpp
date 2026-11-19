#include "duckdb/execution/operator/helper/physical_execute.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_execute.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalExecute &op) {
	if (op.prepared->physical_plan) {
		D_ASSERT(op.children.empty());
		auto &plan = op.prepared->physical_plan->Root();
		return Make<PhysicalExecute>(plan, op.prepared, *op.statement, op.custom_plan);
	}

	D_ASSERT(op.children.size() == 1);
	if (op.replaces_plan) {
		PhysicalPlanGenerator inner_planner(context);
		op.prepared->physical_plan = inner_planner.PlanInternal(*op.children[0]);
		op.prepared->types = op.prepared->physical_plan->Root().types;
		op.statement->plan = op.prepared;
		return Make<PhysicalExecute>(op.prepared->physical_plan->Root(), op.prepared, *op.statement, op.custom_plan);
	}
	auto &plan = CreatePlan(*op.children[0]);
	return Make<PhysicalExecute>(plan, op.prepared, *op.statement, op.custom_plan);
}

} // namespace duckdb
