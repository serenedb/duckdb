#include "duckdb/parser/expression_depth_check.hpp"

#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

void ExpressionDepthCheck::Verify(ParsedExpression &expr, idx_t depth) {
	deepest = MaxValue(deepest, depth);
	ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { Verify(child, depth + 1); });
	if (expr.GetExpressionClass() != ExpressionClass::SUBQUERY) {
		return;
	}
	auto &subquery = expr.Cast<SubqueryExpression>().SubqueryMutable();
	D_ASSERT(subquery && subquery->node);
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    *subquery->node, [&](unique_ptr<ParsedExpression> &child) { Verify(*child, depth + 1); });
}

} // namespace duckdb
