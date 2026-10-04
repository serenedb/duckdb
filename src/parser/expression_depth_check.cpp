#include "duckdb/parser/expression_depth_check.hpp"

#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

void ExpressionDepthCheck::Verify(ParsedExpression &root) {
	pending.clear();
	pending.emplace_back(root, 1);
	while (!pending.empty()) {
		auto entry = pending.back();
		pending.pop_back();
		auto &expr = entry.first.get();
		auto depth = entry.second;
		if (depth > max_expression_depth) {
			ParserException::ThrowMaxExpressionDepth(max_expression_depth);
		}
		deepest = MaxValue(deepest, depth);
		auto push = [&](unique_ptr<ParsedExpression> &child) {
			if (child) {
				pending.emplace_back(*child, depth + 1);
			}
		};
		ParsedExpressionIterator::EnumerateChildren(expr, push);
		if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
			auto &subquery = expr.Cast<SubqueryExpression>().SubqueryMutable();
			if (subquery && subquery->node) {
				ParsedExpressionIterator::EnumerateQueryNodeChildren(*subquery->node, push);
			}
		}
	}
}

} // namespace duckdb
