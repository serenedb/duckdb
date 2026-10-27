
#include "duckdb/optimizer/rule/constant_folding.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//! The ConstantFoldingExpressionMatcher matches on any scalar expression (i.e. Expression::IsFoldable is true)
class ConstantFoldingExpressionMatcher : public FoldableConstantMatcher {
public:
	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override {
		// we also do not match on ConstantExpressions, because we cannot fold those any further
		if (expr.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			return false;
		}
		return FoldableConstantMatcher::Match(expr, bindings);
	}
	bool CanMatch(ExpressionClass expression_class, ExpressionType expression_type,
	              optional_ptr<const Identifier> function_name) const override {
		return expression_type != ExpressionType::VALUE_CONSTANT &&
		       FoldableConstantMatcher::CanMatch(expression_class, expression_type, function_name);
	}
};

ConstantFoldingRule::ConstantFoldingRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto op = make_uniq<ConstantFoldingExpressionMatcher>();
	root = std::move(op);
}

static bool EvaluatesEveryChild(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION:
		return !ExpressionIterator::AnyChild(expr, [](const Expression &child) {
			return child.GetExpressionClass() == ExpressionClass::BOUND_LAMBDA ||
			       (child.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
			        child.Cast<BoundConstantExpression>().GetValue().IsNull());
		});
	case ExpressionClass::BOUND_OPERATOR:
		return expr.GetExpressionType() != ExpressionType::OPERATOR_COALESCE &&
		       expr.GetExpressionType() != ExpressionType::OPERATOR_TRY;
	default:
		return false;
	}
}

unique_ptr<Expression> ConstantFoldingRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                  bool &changes_made, bool is_root) {
	auto &root = bindings[0].get();
	// the root is a scalar expression that we have to fold
	D_ASSERT(root.IsFoldable() && root.GetExpressionType() != ExpressionType::VALUE_CONSTANT);

	if (rewriter.ChildThrows() && EvaluatesEveryChild(root)) {
		rewriter.MarkThrows();
		return nullptr;
	}
	// use an ExpressionExecutor to execute the expression
	Value result_value;
	auto fold_result = ExpressionExecutor::EvaluateFoldable(GetContext(), root, result_value);
	if (fold_result == ExpressionExecutor::FoldResult::THROWS) {
		rewriter.MarkThrows();
	}
	if (fold_result != ExpressionExecutor::FoldResult::FOLDED) {
		return nullptr;
	}
	D_ASSERT(result_value.type().InternalType() == root.GetReturnType().InternalType());
	// now get the value from the result vector and insert it back into the plan as a constant expression
	return make_uniq<BoundConstantExpression>(result_value);
}

} // namespace duckdb
