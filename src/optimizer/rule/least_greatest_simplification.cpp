#include "duckdb/optimizer/rule/least_greatest_simplification.hpp"

#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

LeastGreatestSimplificationRule::LeastGreatestSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto function = make_uniq<FunctionExpressionMatcher>();
	static const case_insensitive_set_view_t functions {"least", "greatest"};
	function->function = make_uniq<ManyFunctionMatcher>(&functions);
	function->matchers.push_back(make_uniq<ExpressionMatcher>());
	function->matchers.push_back(make_uniq<ExpressionMatcher>());
	function->policy = SetMatcher::Policy::ORDERED;
	root = std::move(function);
}

unique_ptr<Expression> LeastGreatestSimplificationRule::Apply(LogicalOperator &op,
                                                              vector<reference<Expression>> &bindings,
                                                              bool &changes_made, bool is_root) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &children = root.GetChildrenMutable();
	D_ASSERT(children.size() == 2);
	if (children[0]->IsVolatile() || !children[0]->Equals(*children[1])) {
		return nullptr;
	}
	return Expression::PreserveReturnType(root.GetReturnType(), std::move(children[0]));
}

} // namespace duckdb
