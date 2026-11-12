#include "duckdb/optimizer/rule/list_extract_list_value_folding.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

ListExtractListValueFoldingRule::ListExtractListValueFoldingRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	static const case_insensitive_set_view_t functions {"list_extract", "array_extract"};
	func->function = make_uniq<ManyFunctionMatcher>(&functions);
	func->policy = SetMatcher::Policy::ORDERED;
	auto list = make_uniq<FunctionExpressionMatcher>();
	list->function = make_uniq<SpecificFunctionMatcher>("list_value");
	list->policy = SetMatcher::Policy::SOME;
	func->matchers.push_back(std::move(list));
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	root = std::move(func);
}

unique_ptr<Expression> ListExtractListValueFoldingRule::Apply(LogicalOperator &op,
                                                              vector<reference<Expression>> &bindings,
                                                              bool &changes_made, bool is_root) {
	auto &extract = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &list = bindings[1].get().Cast<BoundFunctionExpression>();
	auto &index_expr = bindings[2].get();
	if (!index_expr.IsFoldable()) {
		return nullptr;
	}
	Value index;
	if (!ExpressionExecutor::TryEvaluateScalar(GetContext(), index_expr, index) || index.IsNull() ||
	    index.type().id() != LogicalTypeId::BIGINT) {
		return nullptr;
	}
	const auto offset = BigIntValue::Get(index);
	auto &elements = list.GetChildrenMutable();
	const auto length = static_cast<int64_t>(elements.size());
	idx_t position;
	if (offset > 0 && offset <= length) {
		position = static_cast<idx_t>(offset - 1);
	} else if (offset < 0 && offset >= -length) {
		position = static_cast<idx_t>(length + offset);
	} else {
		return nullptr;
	}
	for (idx_t i = 0; i < elements.size(); i++) {
		if (i != position && elements[i]->IsVolatile()) {
			return nullptr;
		}
	}
	return BoundCastExpression::AddCastToType(GetContext(), std::move(elements[position]), extract.GetReturnType());
}

} // namespace duckdb
