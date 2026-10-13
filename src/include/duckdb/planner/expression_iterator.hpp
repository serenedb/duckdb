//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

#include <type_traits>

namespace duckdb {

class ExpressionIterator {
public:
	template <class F>
	static bool AnyChild(const Expression &expression, F &&predicate) {
		return VisitChildrenUntil((Expression &)expression, [&](unique_ptr<Expression> &child) {
			return predicate(static_cast<const Expression &>(*child));
		});
	}
	template <class F>
	static bool AllChild(const Expression &expression, F &&predicate) {
		return !VisitChildrenUntil((Expression &)expression, [&](unique_ptr<Expression> &child) {
			return !predicate(static_cast<const Expression &>(*child));
		});
	}

	template <class F>
	static void EnumerateChildren(const Expression &expression, F &&callback) {
		VisitChildrenUntil((Expression &)expression, [&](unique_ptr<Expression> &child) {
			callback(static_cast<const Expression &>(*child));
			return false;
		});
	}
	template <class F>
	static void EnumerateChildren(Expression &expression, F &&callback) {
		VisitChildrenUntil(expression, [&](unique_ptr<Expression> &child) {
			Invoke(callback, child);
			return false;
		});
	}

	template <class F>
	static void EnumerateExpression(unique_ptr<Expression> &expr, F &&callback) {
		if (!expr) {
			return;
		}
		Invoke(callback, expr);
		EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { EnumerateExpression(child, callback); });
	}

	template <class F>
	static void VisitExpressionClass(const Expression &expr, ExpressionClass expr_class, F &&callback) {
		if (expr.GetExpressionClass() == expr_class) {
			callback(expr);
			return;
		}
		EnumerateChildren(expr, [&](const Expression &child) { VisitExpressionClass(child, expr_class, callback); });
	}
	template <class F>
	static void VisitExpressionClassMutable(unique_ptr<Expression> &expr, ExpressionClass expr_class, F &&callback) {
		if (expr->GetExpressionClass() == expr_class) {
			callback(expr);
			return;
		}
		EnumerateChildren(
		    *expr, [&](unique_ptr<Expression> &child) { VisitExpressionClassMutable(child, expr_class, callback); });
	}
	static unique_ptr<Expression> ReplaceExpression(const Expression &expr, const Expression &target,
	                                                const Expression &replacement);

	template <class T, class F>
	static void VisitExpressionMutable(unique_ptr<Expression> &expr, F &&callback) {
		VisitExpressionClassMutable(expr, T::TYPE,
		                            [&](unique_ptr<Expression> &child) { callback(child->Cast<T>(), child); });
	}
	template <class T, class F>
	static void VisitExpression(const Expression &expr, F &&callback) {
		VisitExpressionClass(expr, T::TYPE, [&](const Expression &child) { callback(child.Cast<T>()); });
	}

private:
	template <class F>
	static void Invoke(F &callback, unique_ptr<Expression> &child) {
		if constexpr (std::is_invocable_v<F &, unique_ptr<Expression> &>) {
			callback(child);
		} else {
			callback(*child);
		}
	}

	template <class F>
	static bool VisitChildrenUntil(Expression &expr, F &&callback) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_AGGREGATE: {
			auto &aggr_expr = expr.Cast<BoundAggregateExpression>();
			for (auto &child : aggr_expr.GetChildrenMutable()) {
				if (callback(child)) {
					return true;
				}
			}
			if (aggr_expr.GetFilter()) {
				if (callback(aggr_expr.GetFilterMutable())) {
					return true;
				}
			}
			if (aggr_expr.GetOrderBys()) {
				for (auto &order : aggr_expr.GetOrderBysMutable()->orders) {
					if (callback(order.expression)) {
						return true;
					}
				}
			}
			return false;
		}
		case ExpressionClass::BOUND_CASE: {
			auto &case_expr = expr.Cast<BoundCaseExpression>();
			for (auto &case_check : case_expr.CaseChecksMutable()) {
				if (callback(case_check.when_expr)) {
					return true;
				}
				if (callback(case_check.then_expr)) {
					return true;
				}
			}
			return callback(case_expr.ElseMutable());
		}
		case ExpressionClass::BOUND_CONJUNCTION:
			return VisitAllUntil(expr.Cast<BoundConjunctionExpression>().GetChildrenMutable(), callback);
		case ExpressionClass::BOUND_FUNCTION:
			return VisitAllUntil(expr.Cast<BoundFunctionExpression>().GetChildrenMutable(), callback);
		case ExpressionClass::BOUND_OPERATOR:
			return VisitAllUntil(expr.Cast<BoundOperatorExpression>().GetChildrenMutable(), callback);
		case ExpressionClass::BOUND_SUBQUERY:
			return VisitAllUntil(expr.Cast<BoundSubqueryExpression>().GetChildrenMutable(), callback);
		case ExpressionClass::BOUND_WINDOW: {
			auto &window_expr = expr.Cast<BoundWindowExpression>();
			if (VisitAllUntil(window_expr.PartitionsMutable(), callback)) {
				return true;
			}
			for (auto &order : window_expr.OrderByMutable()) {
				if (callback(order.expression)) {
					return true;
				}
			}
			if (VisitAllUntil(window_expr.GetChildrenMutable(), callback)) {
				return true;
			}
			if (window_expr.FilterMutable() && callback(window_expr.FilterMutable())) {
				return true;
			}
			if (window_expr.StartExprMutable() && callback(window_expr.StartExprMutable())) {
				return true;
			}
			if (window_expr.EndExprMutable() && callback(window_expr.EndExprMutable())) {
				return true;
			}
			for (auto &order : window_expr.ArgOrdersMutable()) {
				if (callback(order.expression)) {
					return true;
				}
			}
			return false;
		}
		case ExpressionClass::BOUND_UNNEST:
			return callback(expr.Cast<BoundUnnestExpression>().ChildMutable());
		case ExpressionClass::BOUND_COLUMN_REF:
		// TODO: enumerate the lambda body here. That would give volatility, CanThrow and Hash for free,
		// but the body's BoundReferenceExpressions index the lambda's own input chunk rather than the
		// outer one, so every pass that rewrites references must first be shown to leave them alone.
		case ExpressionClass::BOUND_LAMBDA:
		case ExpressionClass::BOUND_LAMBDA_REF:
		case ExpressionClass::BOUND_CONSTANT:
		case ExpressionClass::BOUND_DEFAULT:
		case ExpressionClass::BOUND_PARAMETER:
		case ExpressionClass::BOUND_REF:
			// these node types have no children
			return false;
		default:
			throw InternalException("ExpressionIterator used on unbound expression");
		}
	}

	template <class F>
	static bool VisitAllUntil(vector<unique_ptr<Expression>> &children, F &callback) {
		for (auto &child : children) {
			if (callback(child)) {
				return true;
			}
		}
		return false;
	}
};

template <class F>
bool Expression::IsVolatile(F &&child_is_volatile) const {
	return IsNodeVolatile() || ExpressionIterator::AnyChild(*this, child_is_volatile);
}

template <class F>
hash_t Expression::Hash(F &&child_hash) const {
	hash_t hash = NodeHash();
	ExpressionIterator::EnumerateChildren(
	    *this, [&](const Expression &child) { hash += duckdb::Hash<hash_t>(child_hash(child)); });
	return hash;
}

} // namespace duckdb
