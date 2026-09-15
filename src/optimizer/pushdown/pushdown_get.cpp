#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"

namespace duckdb {
static void CollectParameters(Expression &expr, vector<shared_ptr<BoundParameterData>> &parameters) {
	if (expr.GetExpressionType() == ExpressionType::VALUE_PARAMETER) {
		parameters.push_back(expr.Cast<BoundParameterExpression>().ParameterData());
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { CollectParameters(child, parameters); });
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownGet(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_GET);
	auto &get = op->Cast<LogicalGet>();

	// A scan that supports some form of filter push-down would bind a parameter
	// of the filters as a constant: such parameters are invalidated to force a
	// re-bind on execution. A complex-filter scan sees the filters first, and
	// may instead take the parameters over to read them at execution
	// (FunctionData::CachePlanWithParameters), in which case the plan stays
	// cacheable and nothing is invalidated.
	vector<shared_ptr<BoundParameterData>> filter_parameters;
	if (get.function.pushdown_complex_filter || get.function.filter_pushdown) {
		for (auto &filter : filters) {
			if (filter->filter->HasParameter()) {
				CollectParameters(*filter->filter, filter_parameters);
			}
		}
	}
	auto invalidate_parameters = [&]() {
		for (auto &parameter_data : filter_parameters) {
			parameter_data->return_type = LogicalTypeId::INVALID;
		}
	};
	if (get.function.pushdown_complex_filter) {
		// for the remaining filters, check if we can push any of them into the scan as well
		vector<unique_ptr<Expression>> expressions;
		expressions.reserve(filters.size());
		for (auto &filter : filters) {
			expressions.push_back(std::move(filter->filter));
		}
		filters.clear();

		get.function.pushdown_complex_filter(optimizer.context, get, get.bind_data.get(), expressions);

		if (!get.bind_data || !get.bind_data->CachePlanWithParameters()) {
			invalidate_parameters();
		}
		if (expressions.empty()) {
			return op;
		}
		// re-generate the filters
		for (auto &expr : expressions) {
			auto f = make_uniq<Filter>();
			f->filter = std::move(expr);
			f->ExtractBindings();
			filters.push_back(std::move(f));
		}
	} else {
		invalidate_parameters();
	}

	if (get.table_filters.HasFilters() || !get.function.filter_pushdown) {
		// the table function does not support filter pushdown: push a LogicalFilter on top
		return FinishPushdown(std::move(op));
	}
	if (PushFilters() == FilterResult::UNSATISFIABLE) {
		return make_uniq<LogicalEmptyResult>(std::move(op));
	}

	auto &column_ids = get.GetColumnIds();
	//! We generate the table filters that will be executed during the table scan
	vector<FilterPushdownResult> pushdown_results;
	get.table_filters = combiner.GenerateTableScanFilters(column_ids, pushdown_results);

	GenerateFilters();

	for (idx_t i = pushdown_results.size(); i < filters.size(); ++i) {
		// any generated filters have not been pushed down yet
		pushdown_results.push_back(FilterPushdownResult::NO_PUSHDOWN);
	}
	// for any filters we did not manage to push into specialized table filters - try to push them as a generic
	// expression
	for (idx_t i = 0; i < filters.size(); ++i) {
		// get the previous pushdown result
		auto pushdown_result = pushdown_results[i];
		if (pushdown_result != FilterPushdownResult::NO_PUSHDOWN) {
			// this has already been (partially) pushed down - skip
			continue;
		}
		auto &expr = *filters[i]->filter;
		if (expr.IsVolatile()) {
			continue;
		}
		// IN with enough values benefits from a hash join and is handled by InClauseRewriter - skip pushdown.
		// Also skip throwing IN expressions: scan pushdown loses short-circuit evaluation semantics.
		if (expr.GetExpressionType() == ExpressionType::COMPARE_IN) {
			if (expr.CanThrow()) {
				continue;
			}
			auto &in_expr = expr.Cast<BoundOperatorExpression>();
			if (!in_expr.GetChildren().empty() &&
			    in_expr.GetChildren()[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    in_expr.GetChildren().size() - 1 >= InClauseRewriter::IN_CLAUSE_REWRITE_THRESHOLD) {
				continue;
			}
		}
		// Allow pushing down filters that can throw only if there is a single expression
		if (expr.CanThrow() && filters.size() > 1) {
			continue;
		}
		pushdown_result = combiner.TryPushdownGenericExpression(get, expr);
		if (pushdown_result == FilterPushdownResult::PUSHED_DOWN_FULLY) {
			filters.erase_at(i);
			pushdown_results.erase_at(i);
			i--;
		}
	}

	//! Now we try to pushdown the remaining filters to perform zonemap checking
	return FinishPushdown(std::move(op));
}

} // namespace duckdb
