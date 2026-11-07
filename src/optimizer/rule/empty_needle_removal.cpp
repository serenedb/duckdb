
#include "duckdb/optimizer/rule/empty_needle_removal.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/rule/concat_flattening.hpp"

namespace duckdb {

static bool IsEmptyString(ClientContext &context, Expression &expr) {
	if (!expr.IsFoldable()) {
		return false;
	}
	Value value;
	return ExpressionExecutor::TryEvaluateScalar(context, expr, value) && !value.IsNull() &&
	       value.type().id() == LogicalTypeId::VARCHAR && StringValue::Get(value).empty();
}

static unique_ptr<Expression> TryRemoveEmptyStringConcat(ClientContext &context, BoundFunctionExpression &root) {
	if (root.GetReturnType().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	auto &children = root.GetChildrenMutable();
	vector<bool> empty(children.size());
	idx_t empty_count = 0;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		empty[child_idx] = IsEmptyString(context, *children[child_idx]);
		empty_count += empty[child_idx];
	}
	if (empty_count == 0 || empty_count == children.size()) {
		return nullptr;
	}
	vector<unique_ptr<Expression>> remaining;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		if (!empty[child_idx]) {
			remaining.push_back(std::move(children[child_idx]));
		}
	}
	if (remaining.size() == 1) {
		return Expression::PreserveReturnType(root.GetReturnType(), std::move(remaining[0]));
	}
	FunctionBinder binder(context);
	auto result = binder.BindScalarFunction(root.Function().GetDefinition(), std::move(remaining), root.IsOperator());
	if (result->GetReturnType() != root.GetReturnType()) {
		return BoundCastExpression::AddCastToType(context, std::move(result), root.GetReturnType());
	}
	return result;
}

EmptyNeedleRemovalRule::EmptyNeedleRemovalRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on a FunctionExpression that has a foldable ConstantExpression
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->policy = SetMatcher::Policy::SOME;

	static const case_insensitive_set_view_t functions {"prefix", "contains", "suffix", "||"};
	func->function = make_uniq<ManyFunctionMatcher>(&functions);
	root = std::move(func);
}

unique_ptr<Expression> EmptyNeedleRemovalRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                     bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	if (root.Function().GetName() == "||") {
		if (!ConcatFlatteningRule::IsConcatOperator(root)) {
			return nullptr;
		}
		return TryRemoveEmptyStringConcat(GetContext(), root);
	}
	D_ASSERT(root.GetChildren().size() == 2);
	auto &prefix_expr = bindings[2].get();

	// the constant_expr is a scalar expression that we have to fold
	if (!prefix_expr.IsFoldable()) {
		return nullptr;
	}
	D_ASSERT(root.GetReturnType().id() == LogicalTypeId::BOOLEAN);

	Value prefix_value;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), prefix_expr, prefix_value)) {
		return nullptr;
	}

	if (prefix_value.IsNull()) {
		return make_uniq<BoundConstantExpression>(Value(LogicalType::BOOLEAN));
	}

	D_ASSERT(prefix_value.type() == prefix_expr.GetReturnType());
	if (prefix_value.type().InternalType() != PhysicalType::VARCHAR) {
		return nullptr;
	}
	auto &needle_string = StringValue::Get(prefix_value);

	// PREFIX('xyz', '') is TRUE
	// PREFIX(NULL, '') is NULL
	// so rewrite PREFIX(x, '') to TRUE_OR_NULL(x)
	if (needle_string.empty()) {
		return ExpressionRewriter::ConstantOrNull(GetContext(), std::move(root.GetChildrenMutable()[0]),
		                                          Value::BOOLEAN(true));
	}
	return nullptr;
}

NoopReplaceRemovalRule::NoopReplaceRemovalRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	func->policy = SetMatcher::Policy::ORDERED;
	func->function = make_uniq<SpecificFunctionMatcher>("replace");
	root = std::move(func);
}

unique_ptr<Expression> NoopReplaceRemovalRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                     bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	D_ASSERT(root.GetChildren().size() == 3);
	auto &needle_expr = bindings[2].get();
	auto &replacement_expr = bindings[3].get();
	if (!needle_expr.IsFoldable() || !replacement_expr.IsFoldable()) {
		return nullptr;
	}

	Value needle;
	Value replacement;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), needle_expr, needle) ||
	    !ExpressionExecutor::TryEvaluateScalar(GetContext(), replacement_expr, replacement)) {
		return nullptr;
	}
	if (needle.IsNull() || replacement.IsNull() || needle.type().id() != LogicalTypeId::VARCHAR ||
	    replacement.type().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	if (StringValue::Get(needle) != StringValue::Get(replacement)) {
		return nullptr;
	}
	return Expression::PreserveReturnType(root.GetReturnType(), std::move(root.GetChildrenMutable()[0]));
}

} // namespace duckdb
