#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

class BoundFunctionExpression;

class ConcatFlatteningRule : public Rule {
public:
	explicit ConcatFlatteningRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;

	static bool IsConcatOperator(const BoundFunctionExpression &function);
};

} // namespace duckdb
