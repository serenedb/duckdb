//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/raw_predicates.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"

namespace duckdb {

class Expression;

enum class RawPredicateKind : uint8_t { COMPARE, IN_LIST, IS_NULL, IS_NOT_NULL, LIKE, NOT_LIKE };

struct RawPredicate {
	RawPredicateKind kind;
	ExpressionType compare = ExpressionType::INVALID;
	vector<string> values;
	bool anchored_start = false;
	bool anchored_end = false;
};

bool CollectRawPattern(const Expression &expr, bool negated, vector<RawPredicate> &out);
bool CollectRawPredicates(const Expression &expr, bool string_column, vector<RawPredicate> &out);
bool RawPasses(const vector<RawPredicate> &predicates, const char *value, idx_t size, bool null);

} // namespace duckdb
