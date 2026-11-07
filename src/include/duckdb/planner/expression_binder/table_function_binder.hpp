//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/table_function_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

//! The table function binder can bind standard table function parameters (i.e., non-table-in-out functions)
class TableFunctionBinder : public ExpressionBinder {
public:
	TableFunctionBinder(Binder &binder, ClientContext &context, string table_function_name = string(),
	                    string clause = "Table function", bool identifiers_are_strings = false);

public:
	void DisableSQLValueFunctions() {
		accept_sql_value_functions = false;
	}
	void EnableSQLValueFunctions() {
		accept_sql_value_functions = true;
	}

protected:
	BindResult BindLambdaReference(LambdaRefExpression &expr, idx_t depth);
	BindResult BindColumnReference(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression);
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression = false) override;

	string UnsupportedAggregateMessage() override;

private:
	string table_function_name;
	string clause;
	//! Whether sql_value_functions (GetSQLValueFunctionName) are considered when binding column refs
	bool accept_sql_value_functions = true;
	//! Whether unbound identifiers are documented string values for this clause (e.g. COPY/ATTACH options,
	//! where PARTITION_BY (col) is canonical syntax) - these skip the deprecated-conversion warning/error
	bool identifiers_are_strings = false;
	//! Same, scoped to struct literals in arguments: hive_types={'release': DATE} and
	//! columns={id: UBIGINT} use bare identifiers as documented type-name values
	bool inside_struct_literal = false;
};

} // namespace duckdb
