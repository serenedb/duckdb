#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/execution/operator/set/physical_cte.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"

namespace duckdb {

#ifdef D_ASSERT_IS_ENABLED
static bool ContainsRecursiveCTE(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsRecursiveCTE(*child)) {
			return true;
		}
	}
	return false;
}
#endif

struct SubtreeShape {
	bool has_side_effects = false;
	bool contains_recursive_cte = false;
};

template <class SHAPES>
static SubtreeShape GetSubtreeShape(const LogicalOperator &op, SHAPES &shapes) {
	SubtreeShape result;
	result.has_side_effects = op.HasOwnSideEffects();
	result.contains_recursive_cte = op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE;
	bool first_child_has_side_effects = false;
	for (idx_t child_idx = 0; child_idx < op.children.size(); child_idx++) {
		auto child_shape = GetSubtreeShape(*op.children[child_idx], shapes);
		if (child_idx == 0) {
			first_child_has_side_effects = child_shape.has_side_effects;
		}
		result.has_side_effects |= child_shape.has_side_effects;
		result.contains_recursive_cte |= child_shape.contains_recursive_cte;
	}
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		shapes[op.Cast<LogicalMaterializedCTE>().table_index] = {first_child_has_side_effects,
		                                                         result.contains_recursive_cte};
	}
	return result;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalMaterializedCTE &op) {
	D_ASSERT(op.children.size() == 2);

	auto shape = materialized_cte_shapes.find(op.table_index);
	if (shape == materialized_cte_shapes.end()) {
		GetSubtreeShape(op, materialized_cte_shapes);
		shape = materialized_cte_shapes.find(op.table_index);
	}
	D_ASSERT_V(shape->second.definition_has_side_effects == op.children[0]->HasSideEffects());
	D_ASSERT_V(shape->second.contains_recursive_cte ==
	           (ContainsRecursiveCTE(*op.children[0]) || ContainsRecursiveCTE(*op.children[1])));
	auto cte_body_has_side_effects = shape->second.definition_has_side_effects;
	auto use_exchange = planning_recursive_cte_depth == 0 && !shape->second.contains_recursive_cte;

	// Create the working_table that the PhysicalCTE will use for evaluation.
	auto working_table = make_shared_ptr<ColumnDataCollection>(context, op.children[0]->types);

	// Add the ColumnDataCollection to the context of this PhysicalPlanGenerator
	recursive_cte_tables[op.table_index] = working_table;
	materialized_ctes[op.table_index] = vector<const_reference<PhysicalOperator>>();

	// Create the plan for the left side. This is the materialization.
	auto &left = CreatePlan(*op.children[0]);
	const auto cte_body_order = OrderPreservationRecursive(left);
	const auto preserve_cte_order = PreserveInsertionOrder(left);
	const auto use_batch_index = preserve_cte_order && UseBatchIndex(left);
	const auto source_order = preserve_cte_order ? cte_body_order : OrderPreservationType::NO_ORDER;
	materialized_cte_orders[op.table_index] = source_order;

	shared_ptr<PipelineBroadcastExchange> exchange;
	if (use_exchange) {
		auto completion_mode = cte_body_has_side_effects
		                           ? PipelineBroadcastExchangeCompletionMode::RUN_TO_COMPLETION
		                           : PipelineBroadcastExchangeCompletionMode::STOP_WHEN_UNCONSUMED;
		exchange = make_shared_ptr<PipelineBroadcastExchange>(context, op.children[0]->types, completion_mode,
		                                                      source_order, use_batch_index);
		materialized_cte_exchanges[op.table_index] = exchange;
	}

	// Initialize an empty vector to collect the scan operators.
	auto &right = CreatePlan(*op.children[1]);

	auto &cte = Make<PhysicalCTE>(op.ctename, op.table_index, right.types, left, right, op.estimated_cardinality);
	auto &cast_cte = cte.Cast<PhysicalCTE>();
	cast_cte.working_table = working_table;
	cast_cte.exchange = exchange;
	cast_cte.cte_scans = materialized_ctes[op.table_index];
	cast_cte.cte_body_has_side_effects = cte_body_has_side_effects;
	cast_cte.preserve_order = preserve_cte_order;
	cast_cte.use_batch_index = use_batch_index;
	cast_cte.parallel = !preserve_cte_order || use_batch_index;
	return cte;
}

} // namespace duckdb
