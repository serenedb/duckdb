//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/list_extract_list_value_folding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

class ListExtractListValueFoldingRule : public Rule {
public:
	explicit ListExtractListValueFoldingRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
