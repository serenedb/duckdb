#include "duckdb/optimizer/expression_rewriter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct RewriteFrame {
	reference<unique_ptr<Expression>> expr;
	bool is_root;
	bool children_visited;
	bool frozen = false;
	optional_ptr<Expression> parent;
	optional_idx parent_frame;
	bool child_throws = false;
	bool child_not_foldable = false;
};

static bool ApplyRule(LogicalOperator &op, const vector<reference<Rule>> &rules, unique_ptr<Expression> &expr,
                      bool is_root) {
	vector<reference<Expression>> bindings;
	for (auto &rule : rules) {
		bindings.clear();
		if (rule.get().root->Match(*expr, bindings)) {
			// the rule matches! try to apply it
			bool rule_made_change = false;
			auto alias = expr->GetAlias();
#ifdef D_ASSERT_IS_ENABLED
			auto return_type = expr->GetReturnType();
#endif
			auto result = rule.get().Apply(op, bindings, rule_made_change, is_root);
			if (result) {
				D_ASSERT(result->GetReturnType() != return_type ||
				         result->GetReturnType().EqualsIncludingCollation(return_type));
				// the base node changed: the rule applied changes
				if (!alias.empty()) {
					result->SetAlias(std::move(alias));
				}
				expr = std::move(result);
				return true;
			}
			if (rule_made_change) {
				return true;
			}
			// else nothing changed, continue to the next rule
		}
	}
	return false;
}

static void CollectChildren(Expression &expr, vector<reference<unique_ptr<Expression>>> &children) {
	children.clear();
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) { children.push_back(child); });
}

static bool FreezesChildren(const vector<rewrite_barrier_t> &barriers, const Expression &parent, bool parent_frozen) {
	for (auto barrier : barriers) {
		if (barrier(parent, parent_frozen)) {
			return true;
		}
	}
	return false;
}

vector<reference<Rule>> ExpressionRewriter::SelectRules(ExpressionClass expression_class,
                                                        ExpressionType expression_type,
                                                        optional_ptr<const Identifier> function_name) const {
	vector<reference<Rule>> result;
	for (auto &rule : rules) {
		if (rule->root->CanMatch(expression_class, expression_type, function_name)) {
			result.push_back(*rule);
		}
	}
	return result;
}

const vector<reference<Rule>> &ExpressionRewriter::RulesFor(const Expression &expr) {
	const auto expression_class = expr.GetExpressionClass();
	const auto expression_type = expr.GetExpressionType();
	auto [rule_set, inserted] = rule_sets.try_emplace(
	    static_cast<uint16_t>(static_cast<uint16_t>(expression_class) << 8 | static_cast<uint16_t>(expression_type)));
	if (expression_class != ExpressionClass::BOUND_FUNCTION) {
		if (inserted) {
			rule_set->second.rules = SelectRules(expression_class, expression_type, nullptr);
		}
		return rule_set->second.rules;
	}
	auto &name = expr.Cast<BoundFunctionExpression>().Function().GetName();
	auto [function_rules, function_inserted] = rule_set->second.function_rules.try_emplace(name);
	if (function_inserted) {
		function_rules->second = SelectRules(expression_class, expression_type, name);
	}
	return function_rules->second;
}

unique_ptr<Expression> ExpressionRewriter::ApplyRules(LogicalOperator &op, const vector<rewrite_barrier_t> &barriers,
                                                      unique_ptr<Expression> expr, bool &changes_made, bool is_root) {
	vector<RewriteFrame> stack;
	vector<reference<unique_ptr<Expression>>> children;
	stack.push_back({expr, is_root, false});

	while (!stack.empty()) {
		auto &frame = stack.back();
		auto &current_expr = frame.expr.get();
		D_ASSERT(current_expr);
		if (!frame.children_visited) {
			frame.children_visited = true;
			frame.child_throws = false;
			frame.child_not_foldable = false;
			// Rewrite the subtree bottom-up so parent rules see already-simplified children.
			CollectChildren(*current_expr, children);
			const bool children_frozen = FreezesChildren(barriers, *current_expr, frame.frozen);
			const auto parent = frame.frozen ? nullptr : current_expr.get();
			const optional_idx parent_frame(stack.size() - 1);
			for (idx_t i = children.size(); i > 0; i--) {
				stack.push_back({children[i - 1], false, false, children_frozen, parent, parent_frame});
			}
			continue;
		}
		if (frame.frozen) {
			stack.pop_back();
			continue;
		}
		current_parent = frame.parent;
		child_throws = frame.child_throws;
		child_not_foldable = frame.child_not_foldable;
		throws = false;
		not_foldable = false;
		if (ApplyRule(op, RulesFor(*current_expr), current_expr, frame.is_root)) {
			changes_made = true;
			frame.children_visited = false;
			continue;
		}
		if (frame.parent_frame.IsValid()) {
			auto &parent_frame = stack[frame.parent_frame.GetIndex()];
			parent_frame.child_throws |= throws;
			parent_frame.child_not_foldable |= not_foldable;
		}
		stack.pop_back();
	}
	return expr;
}

unique_ptr<Expression> ExpressionRewriter::ConstantOrNull(ClientContext &context, unique_ptr<Expression> child,
                                                          Value value) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child));
	return ConstantOrNull(context, std::move(children), std::move(value));
}

unique_ptr<Expression> ExpressionRewriter::ConstantOrNull(ClientContext &context,
                                                          vector<unique_ptr<Expression>> children, Value value) {
	children.insert(children.begin(), make_uniq<BoundConstantExpression>(std::move(value)));
	// the bind of constant_or_null derives the return type and the bind data from the leading constant
	return BindBuiltinScalarFunction(context, ConstantOrNullFun::Name, std::move(children));
}

void ExpressionRewriter::VisitOperator(LogicalOperator &op) {
	VisitOperatorChildren(op);
	this->op = &op;

	if (rule_sets_rule_count != rules.size()) {
		rule_sets.clear();
		rule_sets_rule_count = rules.size();
	}
	to_apply_barriers.clear();
	for (auto &extension : OptimizerExtension::Iterate(context)) {
		if (extension.rewrite_barrier) {
			to_apply_barriers.push_back(extension.rewrite_barrier);
		}
	}

	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		// For FILTER we want to ensure we always visit the split predicates
		// Rewrites can add more predicates, so we need to loop
		auto &filter = op.Cast<LogicalFilter>();
		idx_t visited_until = 0;
		do {
			for (idx_t i = visited_until; i < filter.expressions.size(); i++) {
				VisitExpression(&filter.expressions[i]);
			}
			visited_until = filter.expressions.size();
		} while (LogicalFilter::SplitPredicates(filter.expressions));
	} else {
		VisitOperatorExpressions(op);
	}
}

void ExpressionRewriter::VisitExpression(unique_ptr<Expression> *expression) {
	bool changes_made = false;
	*expression = ApplyRules(*op, to_apply_barriers, std::move(*expression), changes_made, true);
	current_parent = nullptr;
}

ClientContext &Rule::GetContext() const {
	return rewriter.context;
}

} // namespace duckdb
