//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression_depth_check.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct ExpressionDepthCheck {
	idx_t max_expression_depth;
	vector<pair<reference<ParsedExpression>, idx_t>> pending;
	idx_t deepest = 0;

	void Verify(ParsedExpression &root);
};

} // namespace duckdb
