#include "duckdb/optimizer/rule/list_transform_unrolling.hpp"

#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"

namespace duckdb {

namespace {

bool IsConstantExtract(optional_ptr<Expression> parent) {
	if (!parent || parent->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &extract = parent->Cast<BoundFunctionExpression>();
	auto &name = extract.Function().GetName();
	return (name == "list_extract" || name == "array_extract") && extract.GetChildren()[1]->IsFoldable();
}

} // namespace

ListTransformUnrollingRule::ListTransformUnrollingRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	static const case_insensitive_set_view_t functions {"list_transform", "array_transform", "list_apply",
	                                                    "array_apply", "apply"};
	func->function = make_uniq<ManyFunctionMatcher>(&functions);
	func->policy = SetMatcher::Policy::SOME;
	root = std::move(func);
}

unique_ptr<Expression> ListTransformUnrollingRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                         bool &changes_made, bool is_root) {
	auto &transform = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &children = transform.GetChildrenMutable();
	if (children[0]->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &list = children[0]->Cast<BoundFunctionExpression>();
	auto &elements = list.GetChildrenMutable();
	if (list.Function().GetName() != "list_value" || elements.empty() ||
	    (elements.size() != 1 && !IsConstantExtract(rewriter.CurrentParent()))) {
		return nullptr;
	}
	auto &bind_data = transform.BindInfo()->Cast<ListLambdaBindData>();
	vector<unique_ptr<Expression>> results;
	results.reserve(elements.size());
	for (idx_t i = 0; i < elements.size(); i++) {
		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(children[1]->Copy());
		arguments.push_back(std::move(elements[i]));
		if (bind_data.has_index) {
			arguments.push_back(
			    make_uniq<BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(i + bind_data.index_offset))));
		}
		auto invoke = BindBuiltinScalarFunction(GetContext(), InvokeFun::Name, std::move(arguments));
		for (idx_t extra = 2; extra < children.size(); extra++) {
			invoke->GetChildrenMutable().push_back(children[extra]->Copy());
		}
		results.push_back(std::move(invoke));
	}
	auto result = BindBuiltinScalarFunction(GetContext(), "list_value", std::move(results));
	D_ASSERT(result->GetReturnType() == transform.GetReturnType());
	return std::move(result);
}

} // namespace duckdb
