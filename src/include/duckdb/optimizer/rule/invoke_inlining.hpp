//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/invoke_inlining.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

class InvokeInliningRule : public Rule {
public:
	explicit InvokeInliningRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;

	static void InlinePredicate(unique_ptr<Expression> &predicate);
};

} // namespace duckdb
