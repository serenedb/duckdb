#include "duckdb/execution/operator/helper/physical_execute.hpp"

#include "duckdb/parallel/meta_pipeline.hpp"

namespace duckdb {

PhysicalExecute::PhysicalExecute(PhysicalPlan &physical_plan, PhysicalOperator &plan,
                                 shared_ptr<PreparedStatementData> prepared, PreparedStatementEntry &statement,
                                 bool custom_plan)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXECUTE, plan.types, idx_t(-1)), plan(plan),
      prepared(std::move(prepared)), statement(statement), custom_plan(custom_plan) {
}

vector<const_reference<PhysicalOperator>> PhysicalExecute::GetChildren() const {
	return {plan};
}

void PhysicalExecute::CountExecution() const {
	statement.CountExecution(custom_plan);
}

void PhysicalExecute::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	CountExecution();
	// EXECUTE statement: build pipeline on child
	meta_pipeline.Build(plan);
}

} // namespace duckdb
