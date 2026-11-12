#include "duckdb/optimizer/rule/concat_flattening.hpp"

#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

bool ConcatFlatteningRule::IsConcatOperator(const BoundFunctionExpression &function) {
	static const auto concat_bind = ConcatOperatorFun::GetFunction().GetBindCallback();
	auto &definition = function.Function().GetDefinition();
	return function.IsOperator() && definition && definition->GetBindCallback() == concat_bind;
}

namespace {

bool IsConcatOperator(const Expression &expr, const LogicalType &type) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION || expr.GetReturnType() != type) {
		return false;
	}
	return ConcatFlatteningRule::IsConcatOperator(expr.Cast<BoundFunctionExpression>());
}

class ConcatChainRootMatcher : public ExpressionMatcher {
public:
	explicit ConcatChainRootMatcher(ExpressionRewriter &rewriter) : rewriter(rewriter) {
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override {
		auto &type = expr.GetReturnType();
		if (!IsConcatOperator(expr, type)) {
			return false;
		}
		auto parent = rewriter.CurrentParent();
		if (parent && IsConcatOperator(*parent, type)) {
			return false;
		}
		for (auto &child : expr.Cast<BoundFunctionExpression>().GetChildren()) {
			if (IsConcatOperator(*child, type)) {
				bindings.push_back(expr);
				return true;
			}
		}
		return false;
	}
	bool CanMatch(ExpressionClass expression_class, ExpressionType expression_type,
	              optional_ptr<const Identifier> function_name) const override {
		return expression_class == ExpressionClass::BOUND_FUNCTION && function_name &&
		       *function_name == ConcatOperatorFun::Name;
	}

private:
	ExpressionRewriter &rewriter;
};

void CollectConcatOperands(unique_ptr<Expression> &expr, const LogicalType &type,
                           vector<unique_ptr<Expression>> &operands) {
	vector<reference<unique_ptr<Expression>>> stack;
	stack.push_back(expr);
	while (!stack.empty()) {
		auto &current = stack.back().get();
		stack.pop_back();
		if (!IsConcatOperator(*current, type)) {
			operands.push_back(std::move(current));
			continue;
		}
		auto &children = current->Cast<BoundFunctionExpression>().GetChildrenMutable();
		for (idx_t i = children.size(); i > 0; i--) {
			stack.push_back(children[i - 1]);
		}
	}
}

} // namespace

ConcatFlatteningRule::ConcatFlatteningRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	root = make_uniq<ConcatChainRootMatcher>(rewriter);
}

unique_ptr<Expression> ConcatFlatteningRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                   bool &changes_made, bool is_root) {
	auto &concat = bindings[0].get().Cast<BoundFunctionExpression>();
	auto type = concat.GetReturnType();
	vector<unique_ptr<Expression>> operands;
	for (auto &child : concat.GetChildrenMutable()) {
		CollectConcatOperands(child, type, operands);
	}
	FunctionBinder binder(rewriter.context);
	auto result = binder.BindScalarFunction(concat.Function().GetDefinition(), std::move(operands), true);
	if (result->GetReturnType() != type) {
		return BoundCastExpression::AddCastToType(rewriter.context, std::move(result), type);
	}
	return result;
}

} // namespace duckdb
