//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {
class Expression;

template <class T>
struct ExpressionHashFunction {
	using skip_absl_hash_eq_consistent = void;
	uint64_t operator()(const const_reference<T> &expr) const {
		return (uint64_t)expr.get().Hash();
	}
};

template <class T>
struct ExpressionEquality {
	using skip_absl_hash_eq_consistent = void;
	bool operator()(const const_reference<T> &a, const const_reference<T> &b) const {
		return a.get().Equals(b.get());
	}
};

template <class T>
struct HashedExpression {
	const_reference<T> expr;
	hash_t hash;
};

template <class T>
struct HashedExpressionHash {
	using skip_absl_hash_eq_consistent = void;
	hash_t operator()(const HashedExpression<T> &key) const {
		return key.hash;
	}
};

template <class T>
struct HashedExpressionEquality {
	using skip_absl_hash_eq_consistent = void;
	bool operator()(const HashedExpression<T> &a, const HashedExpression<T> &b) const {
		return &a.expr.get() == &b.expr.get() || (a.hash == b.hash && a.expr.get().Equals(b.expr.get()));
	}
};

template <class T, class V>
using hashed_expression_map_t =
    unordered_map<HashedExpression<T>, V, HashedExpressionHash<T>, HashedExpressionEquality<T>>;

template <class T>
using hashed_expression_set_t =
    unordered_set<HashedExpression<T>, HashedExpressionHash<T>, HashedExpressionEquality<T>>;

template <typename T>
using expression_map_t =
    unordered_map<const_reference<Expression>, T, ExpressionHashFunction<Expression>, ExpressionEquality<Expression>>;

using expression_set_t =
    unordered_set<const_reference<Expression>, ExpressionHashFunction<Expression>, ExpressionEquality<Expression>>;

template <typename T>
using parsed_expression_map_t =
    unordered_map<const_reference<ParsedExpression>, T, ExpressionHashFunction<ParsedExpression>,
                  ExpressionEquality<ParsedExpression>>;

using parsed_expression_set_t =
    unordered_set<const_reference<ParsedExpression>, ExpressionHashFunction<ParsedExpression>,
                  ExpressionEquality<ParsedExpression>>;

} // namespace duckdb
