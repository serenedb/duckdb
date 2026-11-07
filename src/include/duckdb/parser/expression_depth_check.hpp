//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression_depth_check.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct ExpressionDepthCheck {
	idx_t deepest = 0;

	void Verify(ParsedExpression &expr, idx_t depth = 1);
};

} // namespace duckdb
