#include "duckdb/optimizer/rule/octet_length.hpp"

#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

bool IsByteString(const LogicalType &type) {
	return type == LogicalType::VARCHAR || type == LogicalType::BLOB;
}

bool IsCharacterLength(const Identifier &name) {
	return name == LengthFun::Name || name == LenFun::Name || name == CharLengthFun::Name ||
	       name == CharacterLengthFun::Name;
}

unique_ptr<Expression> OctetLength(ClientContext &context, unique_ptr<Expression> input) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(input));
	return BindBuiltinScalarFunction(context, OctetLengthFun::Name, std::move(arguments));
}

//! The comparison of a value with the empty value, which sorts first, as a test of the value being empty (true) or not
optional<bool> EmptyValueComparison(ExpressionType comparison_type) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_GREATERTHAN:
		return false;
	default:
		return nullopt;
	}
}

//! The comparison of a length with zero or one, as a test of the value being empty (true) or not
optional<bool> LengthComparison(ExpressionType comparison_type, const hugeint_t &value) {
	const bool zero = value == hugeint_t(0);
	const bool one = value == hugeint_t(1);
	if ((comparison_type == ExpressionType::COMPARE_EQUAL && zero) ||
	    (comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO && zero) ||
	    (comparison_type == ExpressionType::COMPARE_LESSTHAN && one)) {
		return true;
	}
	if ((comparison_type == ExpressionType::COMPARE_NOTEQUAL && zero) ||
	    (comparison_type == ExpressionType::COMPARE_GREATERTHAN && zero) ||
	    (comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO && one)) {
		return false;
	}
	return nullopt;
}

} // namespace

OctetLengthRule::OctetLengthRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto function = make_uniq<FunctionExpressionMatcher>();
	function->function = make_uniq<SpecificFunctionMatcher>(StrlenFun::Name);
	function->policy = SetMatcher::Policy::SOME;
	root = std::move(function);
}

unique_ptr<Expression> OctetLengthRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                              bool &changes_made, bool is_root) {
	auto &strlen = bindings[0].get().Cast<BoundFunctionExpression>();
	changes_made = true;
	return OctetLength(rewriter.context, std::move(strlen.GetChildrenMutable()[0]));
}

EmptinessRule::EmptinessRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto comparison = make_uniq<ComparisonExpressionMatcher>();
	comparison->policy = SetMatcher::Policy::UNORDERED;
	comparison->matchers.push_back(make_uniq<ExpressionMatcher>());
	comparison->matchers.push_back(make_uniq<ConstantExpressionMatcher>());
	root = std::move(comparison);
}

unique_ptr<Expression> EmptinessRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                            bool &changes_made, bool is_root) {
	auto &comparison = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &side = bindings[1].get();
	auto &constant = bindings[2].get().Cast<BoundConstantExpression>().GetValue();
	if (constant.IsNull()) {
		return nullptr;
	}
	auto comparison_type = comparison.GetExpressionType();
	const bool side_left = &BoundComparisonExpression::Left(comparison) == &side;
	if (!side_left) {
		// the constant is on the left - read the comparison from the side of the other operand
		comparison_type = FlipComparisonExpression(comparison_type);
	}

	optional<bool> empty;
	unique_ptr<Expression> *value = nullptr;
	if (IsByteString(side.GetReturnType()) && constant.type() == side.GetReturnType() &&
	    StringValue::Get(constant).empty()) {
		empty = EmptyValueComparison(comparison_type);
		value = side_left ? &BoundComparisonExpression::LeftMutable(comparison)
		                  : &BoundComparisonExpression::RightMutable(comparison);
	} else if (side.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && constant.type().IsIntegral()) {
		// a string has no characters exactly when it has no bytes
		auto &length = side.Cast<BoundFunctionExpression>();
		if (length.GetChildren().size() == 1 && length.GetChildren()[0]->GetReturnType() == LogicalType::VARCHAR &&
		    IsCharacterLength(length.Function().GetName())) {
			empty = LengthComparison(comparison_type, constant.GetValue<hugeint_t>());
			value = &length.GetChildrenMutable()[0];
		}
	}
	if (!empty) {
		return nullptr;
	}
	changes_made = true;
	return BoundComparisonExpression::Create(
	    *empty ? ExpressionType::COMPARE_EQUAL : ExpressionType::COMPARE_GREATERTHAN,
	    OctetLength(rewriter.context, std::move(*value)), make_uniq<BoundConstantExpression>(Value::BIGINT(0)));
}

} // namespace duckdb
