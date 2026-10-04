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
	static constexpr idx_t LEVELS_PER_TOKEN = 2;

	idx_t max_expression_depth;
	vector<pair<reference<ParsedExpression>, idx_t>> pending;
	idx_t deepest = 0;

	static bool CanExceed(idx_t token_count, idx_t max_expression_depth) {
		return token_count * LEVELS_PER_TOKEN > max_expression_depth;
	}
	void Verify(ParsedExpression &root);
};

} // namespace duckdb
