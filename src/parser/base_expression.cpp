#include "duckdb/parser/base_expression.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"

namespace duckdb {

void BaseExpression::Print() const {
	Printer::Print(ToString());
}

// Port of PostgreSQL's FigureColnameInternal.
// Returns: 0 = no name found, 1 = weak name (type cast), 2 = strong name.
// Caller uses "?column?" as fallback when strength is 0.
static int FigureColnameInternal(const BaseExpression &expr, string &name) {
	switch (expr.expression_class) {
	case ExpressionClass::COLUMN_REF: {
		auto &col = expr.Cast<ColumnRefExpression>();
		name = col.GetColumnName();
		return 2;
	}
	case ExpressionClass::FUNCTION: {
		auto &func = expr.Cast<FunctionExpression>();
		if (func.is_operator) {
			// PG: operators like +, -, *, / don't produce column names
			return 0;
		}
		name = StringUtil::Lower(func.function_name);
		return 2;
	}
	case ExpressionClass::CAST: {
		auto &cast = expr.Cast<CastExpression>();
		int strength = 0;
		if (cast.child) {
			strength = FigureColnameInternal(*cast.child, name);
		}
		if (strength <= 1) {
			// Use type name as weak name (PG behavior: SELECT 'x'::text → "text")
			name = StringUtil::Lower(cast.cast_type.ToString());
			return 1;
		}
		return strength;
	}
	case ExpressionClass::COLLATE: {
		auto &collate = expr.Cast<CollateExpression>();
		if (collate.child) {
			return FigureColnameInternal(*collate.child, name);
		}
		break;
	}
	case ExpressionClass::CASE: {
		auto &case_expr = expr.Cast<CaseExpression>();
		int strength = 0;
		if (case_expr.else_expr) {
			strength = FigureColnameInternal(*case_expr.else_expr, name);
		}
		if (strength <= 1) {
			name = "case";
			return 1;
		}
		return strength;
	}
	case ExpressionClass::SUBQUERY: {
		auto &sub = expr.Cast<SubqueryExpression>();
		if (sub.subquery_type == SubqueryType::EXISTS) {
			name = "exists";
			return 2;
		}
		if (sub.subquery_type == SubqueryType::SCALAR) {
			// For scalar subqueries, try to get the column name from the subquery
			// This matches PG's EXPR_SUBLINK handling
			name = "?column?";
			return 0;
		}
		break;
	}
	case ExpressionClass::OPERATOR: {
		auto &op = expr.Cast<OperatorExpression>();
		// NULLIF is represented as an operator in DuckDB
		if (op.type == ExpressionType::OPERATOR_NULLIF) {
			name = "nullif";
			return 2;
		}
		if (op.type == ExpressionType::OPERATOR_COALESCE) {
			name = "coalesce";
			return 2;
		}
		break;
	}
	default:
		break;
	}
	return 0;
}

string BaseExpression::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return ToString();
	}
#endif
	if (!alias.empty()) {
		return alias;
	}
	string name;
	if (FigureColnameInternal(*this, name) > 0) {
		return name;
	}
	return "?column?";
}

bool BaseExpression::Equals(const BaseExpression &other) const {
	if (expression_class != other.expression_class || type != other.type) {
		return false;
	}
	return true;
}

void BaseExpression::Verify() const {
}

} // namespace duckdb
