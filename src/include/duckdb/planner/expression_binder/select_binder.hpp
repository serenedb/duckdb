//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/select_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder/base_select_binder.hpp"

namespace duckdb {

//! The SELECT binder is responsible for binding an expression within the SELECT clause of a SQL statement
class SelectBinder : public BaseSelectBinder {
public:
	SelectBinder(Binder &binder, ClientContext &context, BoundSelectNode &node);

	bool TryResolveAliasReference(ColumnRefExpression &colref, idx_t depth, bool root_expression, BindResult &result,
	                              unique_ptr<ParsedExpression> &expr_ptr) override;

protected:
	void ThrowIfUnnestInLambda(const ColumnBinding &column_binding) override;
	BindResult BindUnnest(FunctionExpression &function, idx_t depth, bool root_expression) override;

public:
	idx_t unnest_level = 0;
	//! how deep we are inside another function's arguments. PostgreSQL only expands a
	//! set-returning function at the top level of the select list; nested in an
	//! argument the value is wanted, so generate_series must stay the scalar
	//! LIST-returning overload there (`list_apply(generate_series(1, n), ...)`).
	idx_t function_arg_level = 0;
};

} // namespace duckdb
