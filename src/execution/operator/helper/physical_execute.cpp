#include "duckdb/execution/operator/helper/physical_execute.hpp"

#include "duckdb/parallel/meta_pipeline.hpp"

namespace duckdb {

PhysicalExecute::PhysicalExecute(PhysicalPlan &physical_plan, PhysicalOperator &plan,
                                 shared_ptr<PreparedStatementData> prepared, shared_ptr<PreparedStatementData> stored,
                                 bool custom_plan)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXECUTE, plan.types, idx_t(-1)), plan(plan),
      prepared(std::move(prepared)), stored(std::move(stored)), custom_plan(custom_plan) {
}

vector<const_reference<PhysicalOperator>> PhysicalExecute::GetChildren() const {
	return {plan};
}

void PhysicalExecute::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	if (stored) {
		auto &plans = custom_plan ? stored->custom_plans : stored->generic_plans;
		plans++;
	}
	// EXECUTE statement: build pipeline on child
	meta_pipeline.Build(plan);
}

} // namespace duckdb
