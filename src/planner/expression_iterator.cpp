#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

static void ReplaceExpressionInPlace(unique_ptr<Expression> &expr, const Expression &target,
                                     const Expression &replacement) {
	if (expr->Equals(target)) {
		expr = replacement.Copy();
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { ReplaceExpressionInPlace(child, target, replacement); });
}

unique_ptr<Expression> ExpressionIterator::ReplaceExpression(const Expression &expr, const Expression &target,
                                                             const Expression &replacement) {
	auto result = expr.Copy();
	ReplaceExpressionInPlace(result, target, replacement);
	return result;
}

} // namespace duckdb
