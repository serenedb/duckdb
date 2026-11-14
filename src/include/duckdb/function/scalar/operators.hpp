//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/operators.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

bool ArithmeticWraps(const BoundFunctionExpression &arithmetic);

struct AddFunction {
	static ScalarFunction GetFunction(const LogicalType &type);
	static ScalarFunction GetFunction(const LogicalType &left_type, const LogicalType &right_type);
};

struct SubtractFunction {
	static ScalarFunction GetFunction(const LogicalType &type);
	static ScalarFunction GetFunction(const LogicalType &left_type, const LogicalType &right_type);
};

} // namespace duckdb
