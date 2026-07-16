#include "duckdb/optimizer/topn_optimizer.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

TopN::TopN(ClientContext &context_p) : context(context_p) {
}

bool TopN::CanOptimize(LogicalOperator &op, optional_ptr<ClientContext> context) {
	if (op.type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op.Cast<LogicalLimit>();

		if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
			// we need LIMIT to be present AND be a constant value for us to be able to use Top-N
			return false;
		}
		if (limit.offset_val.Type() == LimitNodeType::EXPRESSION_VALUE) {
			// we need offset to be either not set (i.e. limit without offset) OR have offset be
			return false;
		}

		auto child_op = op.children[0].get();
		if (context) {
			// estimate child cardinality if the context is available
			child_op->EstimateCardinality(*context);
		}

		if (child_op->has_estimated_cardinality) {
			// only check if we should switch to full sorting if we have estimated cardinality
			auto constant_limit = static_cast<double>(limit.limit_val.GetConstantValue());
			if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
				constant_limit += static_cast<double>(limit.offset_val.GetConstantValue());
			}
			auto child_card = static_cast<double>(child_op->estimated_cardinality);

			// if the limit is > 0.7% of the child cardinality, sorting the whole table is faster
			bool limit_is_large = constant_limit > 5000;
			if (constant_limit > child_card * 0.007 && limit_is_large) {
				return false;
			}
		}

		while (child_op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			D_ASSERT(!child_op->children.empty());
			child_op = child_op->children[0].get();
		}

		return child_op->type == LogicalOperatorType::LOGICAL_ORDER_BY;
	}
	return false;
}

void TopN::PushdownDynamicFilters(LogicalTopN &op) {
	// pushdown dynamic filters through the Top-N operator
	bool nulls_first = op.orders[0].null_order == OrderByNullType::NULLS_FIRST;
	auto &type = op.orders[0].expression->GetReturnType();
	if (!TypeIsNumeric(type.InternalType()) && type.id() != LogicalTypeId::VARCHAR) {
		// only supported for numeric and varchar types
		return;
	}
	if (op.orders[0].expression->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		// we can only pushdown on ORDER BY [col] currently
		return;
	}
	if (op.dynamic_filter) {
		// dynamic filter is already set
		return;
	}
	auto &colref = op.orders[0].expression->Cast<BoundColumnRefExpression>();
	vector<JoinFilterPushdownColumn> columns;
	JoinFilterPushdownColumn column;
	column.probe_column_index = colref.Binding();
	columns.emplace_back(column);
	vector<PushdownFilterTarget> pushdown_targets;
	JoinFilterPushdownOptimizer::GetPushdownFilterTargets(*op.children[0], std::move(columns), pushdown_targets);
	if (pushdown_targets.empty()) {
		// no pushdown targets
		return;
	}
	// found pushdown targets! generate dynamic filters
	ExpressionType comparison_type;
	if (op.orders[0].type == OrderType::ASCENDING) {
		// for ascending order, we want the lowest N elements, so we filter on C <= [boundary]
		// if we only have a single order clause, we can filter on C < boundary
		comparison_type =
		    op.orders.size() == 1 ? ExpressionType::COMPARE_LESSTHAN : ExpressionType::COMPARE_LESSTHANOREQUALTO;
	} else {
		// for descending order, we want the highest N elements, so we filter on C >= [boundary]
		// if we only have a single order clause, we can filter on C > boundary
		comparison_type =
		    op.orders.size() == 1 ? ExpressionType::COMPARE_GREATERTHAN : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
	}
	Value minimum_value = type.InternalType() == PhysicalType::VARCHAR ? Value("") : Value::MinimumValue(type);
	auto filter_data = make_shared_ptr<DynamicFilterData>(comparison_type, std::move(minimum_value));

	// put the filter into the Top-N clause
	op.dynamic_filter = filter_data;

	for (auto &target : pushdown_targets) {
		auto &get = target.get;
		D_ASSERT(target.columns.size() == 1);
		auto col_binding = target.columns[0].probe_column_index;

		// create the actual dynamic filter
		auto pushed_expr = CreateDynamicFilterExpression(filter_data, type);
		if (nulls_first) {
			auto or_filter = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
			auto is_null = ExpressionFilter::CreateNullCheckExpression(
			    make_uniq<BoundReferenceExpression>(type, idx_t(0)), ExpressionType::OPERATOR_IS_NULL);
			or_filter->GetChildrenMutable().push_back(std::move(is_null));
			or_filter->GetChildrenMutable().push_back(std::move(pushed_expr));
			pushed_expr = std::move(or_filter);
		}

		// push the filter into the table scan
		get.table_filters.PushFilter(
		    col_binding.column_index,
		    make_uniq<ExpressionFilter>(CreateOptionalFilterExpression(std::move(pushed_expr), type)));
	}
}

// Lift the TopN's child projection above the TopN so its computed expressions run
// on the K survivors instead of every input row. Only fires when every sort key is
// a plain passthrough (col-ref) output of the projection, so the TopN can sort the
// projection's child directly. The projection is handed to `projections` so the
// reconstruction loop rebuilds it above the TopN, alongside the ones it collected
// from between LIMIT and ORDER BY. Returns true if it lifted.
static bool LiftProjectionThroughTopN(LogicalTopN &topn, vector<unique_ptr<LogicalOperator>> &projections) {
	auto &proj = topn.children[0]->Cast<LogicalProjection>();

	for (auto &order : topn.orders) {
		if (order.expression->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false; // compound sort key -- cannot rebind onto the child
		}
		auto col = order.expression->Cast<BoundColumnRefExpression>().Binding();
		if (col.table_index != proj.table_index ||
		    proj.expressions[col.column_index]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false; // computed sort-key output -- would need to split the projection (#910)
		}
	}

	bool has_computed = false;
	for (auto &expr : proj.expressions) {
		if (expr->IsVolatile()) {
			return false; // a volatile expr must not be evaluated fewer times
		}
		has_computed |= expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF;
	}
	if (!has_computed) {
		return false; // nothing to defer past the limit
	}

	// Sort on the projection's inputs directly (the col-refs the keys passed through).
	for (auto &order : topn.orders) {
		auto col = order.expression->Cast<BoundColumnRefExpression>().Binding().column_index;
		order.expression = proj.expressions[col]->Copy();
	}

	// The TopN's incoming projection_map (from the ORDER BY) lists the outputs actually
	// needed above it; trailing outputs it omits are the sort-only passthroughs the
	// binder appended for the ORDER BY (e.g. the raw score), vestigial now that the
	// sort reads the child directly. Drop them so no dead column is materialized.
	if (!topn.projection_map.empty()) {
		idx_t keep = 0;
		for (auto &pi : topn.projection_map) {
			if (pi.GetIndex() + 1 > keep) {
				keep = pi.GetIndex() + 1;
			}
		}
		while (proj.expressions.size() > keep) {
			proj.expressions.pop_back();
		}
	}

	// projection_map forwards exactly the child columns the projection still reads
	// (the raw score stays here -- pos = score > 0 needs it as an input).
	auto child_bindings = proj.children[0]->GetColumnBindings();
	column_binding_set_t needed;
	for (auto &e : proj.expressions) {
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *e, [&](const BoundColumnRefExpression &r) { needed.insert(r.Binding()); });
	}
	vector<ProjectionIndex> projection_map;
	for (idx_t i = 0; i < child_bindings.size(); i++) {
		if (needed.count(child_bindings[i])) {
			projection_map.emplace_back(i);
		}
	}
	topn.projection_map = std::move(projection_map);

	// Detach the projection and let the reconstruction loop lift it above the TopN.
	auto projection = std::move(topn.children[0]);
	topn.children[0] = std::move(projection->children[0]); // TopN now reads the scan directly
	projections.push_back(std::move(projection));
	return true;
}

unique_ptr<LogicalOperator> TopN::Optimize(unique_ptr<LogicalOperator> op) {
	if (CanOptimize(*op, &context)) {
		vector<unique_ptr<LogicalOperator>> projections;

		// traverse operator tree and collect all projection nodes until we reach
		// the order by operator

		auto child = std::move(op->children[0]);
		// collect all projections until we get to the order by
		while (child->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			D_ASSERT(!child->children.empty());
			auto tmp = std::move(child->children[0]);
			projections.push_back(std::move(child));
			child = std::move(tmp);
		}
		D_ASSERT(child->type == LogicalOperatorType::LOGICAL_ORDER_BY);
		auto &order_by = child->Cast<LogicalOrder>();

		// Move order by operator into children of limit operator
		op->children[0] = std::move(child);

		auto &limit = op->Cast<LogicalLimit>();
		auto limit_val = limit.limit_val.GetConstantValue();
		idx_t offset_val = 0;
		if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
			offset_val = limit.offset_val.GetConstantValue();
		}
		auto topn = make_uniq<LogicalTopN>(std::move(order_by.orders), limit_val, offset_val);
		topn->projection_map = std::move(order_by.projection_map);
		topn->AddChild(std::move(order_by.children[0]));
		auto cardinality = limit_val;
		if (topn->children[0]->has_estimated_cardinality && topn->children[0]->estimated_cardinality < limit_val) {
			cardinality = topn->children[0]->estimated_cardinality;
		}
		topn->SetEstimatedCardinality(cardinality);
		op = std::move(topn);

		// Lift the TopN's child projection above it so its computed expressions
		// run on the K survivors, not every input row (the ORDER BY + LIMIT case
		// of what LimitPushdown already does for a bare LIMIT). It joins the
		// collected projections and is rebuilt above the TopN by the loop below.
		if (op->children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			LiftProjectionThroughTopN(op->Cast<LogicalTopN>(), projections);
		}

		// reconstruct all projection nodes above limit operator
		while (!projections.empty()) {
			auto node = std::move(projections.back());
			node->children[0] = std::move(op);
			op = std::move(node);
			projections.pop_back();
		}
	}
	if (op->type == LogicalOperatorType::LOGICAL_TOP_N) {
		PushdownDynamicFilters(op->Cast<LogicalTopN>());
	}

	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

} // namespace duckdb
