#include "duckdb/execution/operator/order/physical_top_n.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
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

optional_ptr<PhysicalOperator> PhysicalPlanGenerator::TryConsumeTopN(LogicalOperator &child, idx_t order_count,
                                                                     const vector<ProjectionIndex> &projection_map,
                                                                     const BoundLimitNode &limit,
                                                                     const BoundLimitNode &offset) {
	if (order_count != 1) {
		return nullptr;
	}
	if (limit.Type() != LimitNodeType::CONSTANT_VALUE && limit.Type() != LimitNodeType::EXPRESSION_VALUE) {
		return nullptr;
	}
	if (offset.Type() != LimitNodeType::UNSET && offset.Type() != LimitNodeType::CONSTANT_VALUE &&
	    offset.Type() != LimitNodeType::EXPRESSION_VALUE) {
		return nullptr;
	}
	reference<LogicalOperator> source = child;
	while (source.get().type == LogicalOperatorType::LOGICAL_PROJECTION && OnlyColumnReferences(source.get())) {
		source = *source.get().children[0];
	}
	if (source.get().type != LogicalOperatorType::LOGICAL_GET) {
		return nullptr;
	}
	auto &get = source.get().Cast<LogicalGet>();
	if (!get.function.consume_top_n || !get.bind_data ||
	    !get.function.consume_top_n(context, *get.bind_data, limit, offset)) {
		return nullptr;
	}
	auto columns = RebaseColumns(projection_map, child, get);
	vector<ProjectionIndex> narrowed;
	narrowed.reserve(columns.size());
	for (auto column : columns) {
		narrowed.push_back(get.projection_ids.empty() ? ProjectionIndex(column) : get.projection_ids[column]);
	}
	get.projection_ids = std::move(narrowed);
	get.ResolveOperatorTypes();
	return &CreatePlan(get);
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalTopN &op) {
	D_ASSERT(op.children.size() == 1);
	if (auto consumed =
	        TryConsumeTopN(*op.children[0], op.orders.size(), op.projection_map,
	                       BoundLimitNode::ConstantValue(NumericCast<int64_t>(op.limit)),
	                       BoundLimitNode::ConstantValue(NumericCast<int64_t>(op.offset)))) {
		return *consumed;
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
