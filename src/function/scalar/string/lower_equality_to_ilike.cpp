////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "duckdb/function/scalar/lower_equality_to_ilike.hpp"

#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

bool IsPlainVarchar(const LogicalType &type) {
	return type.id() == LogicalTypeId::VARCHAR && StringType::GetCollation(type).empty();
}

bool IsFoldedPattern(const Value &value) {
	if (value.IsNull() || !IsPlainVarchar(value.type())) {
		return false;
	}
	for (auto c : StringValue::Get(value)) {
		auto byte = static_cast<unsigned char>(c);
		if (byte >= 0x80 || (byte >= 'A' && byte <= 'Z') || c == '%' || c == '_' || c == '\\') {
			return false;
		}
	}
	return true;
}

bool IsLowerCall(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func = expr.Cast<BoundFunctionExpression>();
	auto &name = func.Function().GetName();
	return (name == "lower" || name == "lcase") && func.GetChildren().size() == 1 &&
	       IsPlainVarchar(func.GetChildren()[0]->GetReturnType()) && IsPlainVarchar(func.GetReturnType());
}

} // namespace

unique_ptr<Expression> LowerEqualityToILike::TryRewrite(const Expression &expr) {
	auto type = expr.GetExpressionType();
	if (!BoundComparisonExpression::IsComparison(expr) ||
	    (type != ExpressionType::COMPARE_EQUAL && type != ExpressionType::COMPARE_NOTEQUAL)) {
		return nullptr;
	}
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	const Expression *lower = &BoundComparisonExpression::Left(comparison);
	const Expression *constant = &BoundComparisonExpression::Right(comparison);
	if (!IsLowerCall(*lower)) {
		std::swap(lower, constant);
	}
	if (!IsLowerCall(*lower) || constant->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	auto &value = constant->Cast<BoundConstantExpression>().GetValue();
	if (!IsFoldedPattern(value)) {
		return nullptr;
	}
	vector<unique_ptr<Expression>> children;
	children.push_back(lower->Cast<BoundFunctionExpression>().GetChildren()[0]->Copy());
	children.push_back(make_uniq<BoundConstantExpression>(value));
	BoundScalarFunction function(type == ExpressionType::COMPARE_EQUAL ? ILikeFun::GetFunction()
	                                                                   : NotILikeFun::GetFunction());
	return make_uniq<BoundFunctionExpression>(std::move(function), std::move(children), nullptr);
}

} // namespace duckdb
