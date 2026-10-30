//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/list_transform_unrolling.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

class ListTransformUnrollingRule : public Rule {
public:
	explicit ListTransformUnrollingRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
