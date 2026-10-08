#include "duckdb/execution/operator/order/physical_top_n.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

static bool OnlyColumnReferences(const LogicalOperator &op) {
	for (auto &expr : op.expressions) {
		if (expr->GetExpressionType() != ExpressionType::BOUND_REF) {
			return false;
		}
	}
	return true;
}

static vector<idx_t> RebaseColumns(const vector<ProjectionIndex> &columns, LogicalOperator &from,
                                   const LogicalOperator &to) {
	vector<idx_t> result;
	if (columns.empty()) {
		for (idx_t i = 0; i < from.types.size(); i++) {
			result.push_back(i);
		}
	} else {
		for (auto &idx : columns) {
			result.push_back(idx.GetIndex());
		}
	}
	for (reference<LogicalOperator> node = from; &node.get() != &to; node = *node.get().children[0]) {
		auto &expressions = node.get().expressions;
		for (auto &i : result) {
			i = expressions[i]->Cast<BoundReferenceExpression>().Index();
		}
	}
	return result;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalTopN &op) {
	D_ASSERT(op.children.size() == 1);
	reference<LogicalOperator> source = *op.children[0];
	while (source.get().type == LogicalOperatorType::LOGICAL_PROJECTION && OnlyColumnReferences(source.get())) {
		source = *source.get().children[0];
	}
	if (op.orders.size() == 1 && source.get().type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = source.get().Cast<LogicalGet>();
		if (get.function.consume_top_n && get.bind_data &&
		    get.function.consume_top_n(context, *get.bind_data, NumericCast<idx_t>(op.limit),
		                               NumericCast<idx_t>(op.offset))) {
			auto columns = RebaseColumns(op.projection_map, *op.children[0], get);
			vector<ProjectionIndex> narrowed;
			narrowed.reserve(columns.size());
			for (auto column : columns) {
				narrowed.push_back(get.projection_ids.empty() ? ProjectionIndex(column) : get.projection_ids[column]);
			}
			get.projection_ids = std::move(narrowed);
			get.ResolveOperatorTypes();
			return CreatePlan(get);
		}
	}
	auto &plan = CreatePlan(*op.children[0]);
	auto projections = PhysicalJoin::FillProjectionMap(plan, op.projection_map);
	auto &top_n =
	    Make<PhysicalTopN>(op.types, std::move(op.orders), NumericCast<idx_t>(op.limit), NumericCast<idx_t>(op.offset),
	                       std::move(op.dynamic_filter), op.estimated_cardinality, std::move(projections));
	top_n.children.push_back(plan);
	return top_n;
}

} // namespace duckdb
