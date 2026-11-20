//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/octet_length.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

//! strlen(s) becomes octet_length(s): both are the length of s in bytes, and octet_length - which takes a BLOB as well
//! - is the one name scans, filters and statistics recognize for it
class OctetLengthRule : public Rule {
public:
	explicit OctetLengthRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

//! A test of whether a string or a blob is empty - s = '', s <> '', s > '', length(s) > 0, char_length(s) = 0 - becomes
//! octet_length(s) = 0 or octet_length(s) > 0, which a scan evaluates on the lengths it stores without reading the
//! values, and which statistics decide from the lengths and from the smallest and the largest value
class EmptinessRule : public Rule {
public:
	explicit EmptinessRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;
};

} // namespace duckdb
