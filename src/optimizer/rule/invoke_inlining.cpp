#include "duckdb/optimizer/rule/invoke_inlining.hpp"

#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

namespace {

struct ColumnUse {
	idx_t references = 0;
	idx_t extracts = 0;
	bool captured = false;
};

enum class ParameterAction : uint8_t { KEEP, SUBSTITUTE, SPLIT };

bool IsCheap(const Expression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_PARAMETER:
	case ExpressionClass::BOUND_COLUMN_REF:
	case ExpressionClass::BOUND_REF:
		return true;
	default:
		return false;
	}
}

bool IsConstructor(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &name = function.Function().GetName();
	return (name == "list_value" || name == "struct_pack") && !function.GetChildren().empty();
}

bool IsInvokedConstructor(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	if (function.Function().GetName() != InvokeFun::Name) {
		return false;
	}
	return IsConstructor(*function.GetChildren()[0]->Cast<BoundLambdaExpression>().LambdaExpr());
}

bool IsConstantExtract(const BoundFunctionExpression &function) {
	auto &name = function.Function().GetName();
	return (name == "list_extract" || name == "array_extract" || name == "struct_extract") &&
	       function.GetChildren()[1]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
}

void CountUses(const Expression &expr, vector<ColumnUse> &uses) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto index = expr.Cast<BoundReferenceExpression>().Index();
		D_ASSERT(index < uses.size());
		uses[index].references++;
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = expr.Cast<BoundFunctionExpression>();
		auto &children = function.GetChildren();
		if (IsConstantExtract(function) && children[0]->GetExpressionClass() == ExpressionClass::BOUND_REF) {
			uses[children[0]->Cast<BoundReferenceExpression>().Index()].extracts++;
		}
		if (function.Function().HasBindLambdaCallback() && !function.Function().GetLazyArguments()) {
			for (idx_t i = 2; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() == ExpressionClass::BOUND_REF) {
					uses[children[i]->Cast<BoundReferenceExpression>().Index()].captured = true;
				}
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { CountUses(child, uses); });
}

unique_ptr<Expression> BindInvoke(ClientContext &context, shared_ptr<Expression> body, vector<Identifier> names,
                                  vector<unique_ptr<Expression>> parameters, vector<unique_ptr<Expression>> extra) {
	auto lambda = make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA, std::move(body),
	                                               parameters.size());
	lambda->SetParameterNames(std::move(names));
	vector<unique_ptr<Expression>> arguments;
	arguments.reserve(parameters.size() + 1);
	arguments.push_back(std::move(lambda));
	for (auto &parameter : parameters) {
		arguments.push_back(std::move(parameter));
	}
	auto result = BindBuiltinScalarFunction(context, InvokeFun::Name, std::move(arguments));
	auto &children = result->GetChildrenMutable();
	for (auto &child : extra) {
		children.push_back(std::move(child));
	}
	return std::move(result);
}

} // namespace

InvokeInliningRule::InvokeInliningRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	func->function = make_uniq<SpecificFunctionMatcher>(InvokeFun::Name);
	func->policy = SetMatcher::Policy::SOME;
	root = std::move(func);
}

unique_ptr<Expression> InvokeInliningRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                 bool &changes_made, bool is_root) {
	auto &invoke = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &children = invoke.GetChildrenMutable();
	auto &lambda = children[0]->Cast<BoundLambdaExpression>();
	const idx_t parameter_count = lambda.ParameterCount();
	auto &body = *lambda.LambdaExpr();
	vector<ColumnUse> uses(children.size() - 1);
	CountUses(body, uses);
	const auto column_of = [&](idx_t parameter) {
		return parameter_count - 1 - parameter;
	};

	bool inline_all = true;
	for (idx_t parameter = 0; parameter < parameter_count && inline_all; parameter++) {
		auto &argument = *children[parameter + 1];
		auto &use = uses[column_of(parameter)];
		inline_all = IsCheap(argument) || (use.references <= 1 && !argument.IsVolatile() && !use.captured);
	}
	if (inline_all) {
		auto result = body.Copy();
		ExpressionIterator::VisitExpressionClassMutable(
		    result, ExpressionClass::BOUND_REF, [&](unique_ptr<Expression> &ref) {
			    const auto column = ref->Cast<BoundReferenceExpression>().Index();
			    ref = children[column < parameter_count ? parameter_count - column : column + 1]->Copy();
		    });
		return result;
	}

	vector<ParameterAction> actions(parameter_count, ParameterAction::KEEP);
	vector<idx_t> first(parameter_count);
	idx_t new_count = 0;
	bool changed = false;
	for (idx_t parameter = 0; parameter < parameter_count; parameter++) {
		auto &argument = *children[parameter + 1];
		auto &use = uses[column_of(parameter)];
		first[parameter] = new_count;
		if (argument.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT ||
		    (use.references == 0 && !argument.IsVolatile())) {
			actions[parameter] = ParameterAction::SUBSTITUTE;
			changed = true;
		} else if (use.references > 0 && use.extracts == use.references && IsConstructor(argument)) {
			actions[parameter] = ParameterAction::SPLIT;
			new_count += argument.Cast<BoundFunctionExpression>().GetChildren().size();
			changed = true;
		} else {
			new_count++;
		}
	}
	vector<optional_idx> extra_columns(children.size() - 1 - parameter_count);
	idx_t extra_count = 0;
	for (idx_t i = 0; i < extra_columns.size(); i++) {
		if (uses[parameter_count + i].references == 0) {
			changed = true;
			continue;
		}
		extra_columns[i] = new_count + extra_count++;
	}

	if (!changed) {
		for (idx_t parameter = 0; parameter < parameter_count; parameter++) {
			auto &use = uses[column_of(parameter)];
			if (use.references == 0 || use.extracts != use.references ||
			    !IsInvokedConstructor(*children[parameter + 1])) {
				continue;
			}
			auto &inner = children[parameter + 1]->Cast<BoundFunctionExpression>();
			auto &inner_children = inner.GetChildrenMutable();
			auto &inner_lambda = inner_children[0]->Cast<BoundLambdaExpression>();
			const idx_t inner_parameter_count = inner_lambda.ParameterCount();
			const idx_t inner_columns = inner_children.size() - 1;
			vector<unique_ptr<Expression>> captures;
			auto &inner_names = inner_lambda.ParameterNames();
			const auto existing_column = [&](const Expression &child) -> unique_ptr<Expression> {
				if (child.IsVolatile()) {
					return nullptr;
				}
				for (idx_t i = 1; i < inner_children.size(); i++) {
					if (!Expression::Equals(*inner_children[i], child)) {
						continue;
					}
					if (i > inner_parameter_count) {
						return make_uniq<BoundReferenceExpression>(inner_children[i]->GetAlias(), child.GetReturnType(),
						                                           i - 1);
					}
					return make_uniq<BoundReferenceExpression>(inner_names[i - 1], child.GetReturnType(),
					                                           inner_parameter_count - i);
				}
				for (idx_t i = 0; i < captures.size(); i++) {
					if (Expression::Equals(*captures[i], child)) {
						return make_uniq<BoundReferenceExpression>(captures[i]->GetAlias(), child.GetReturnType(),
						                                           inner_columns + i);
					}
				}
				return nullptr;
			};
			vector<unique_ptr<Expression>> floated_children;
			floated_children.reserve(children.size());
			floated_children.push_back(children[0]->Copy());
			for (idx_t i = 1; i < children.size(); i++) {
				if (i == parameter + 1) {
					floated_children.push_back(inner_lambda.LambdaExpr()->Copy());
					continue;
				}
				auto &child = children[i];
				auto reference = existing_column(*child);
				if (!reference) {
					reference = make_uniq<BoundReferenceExpression>(child->GetAlias(), child->GetReturnType(),
					                                                inner_columns + captures.size());
					captures.push_back(std::move(child));
				}
				floated_children.push_back(std::move(reference));
			}
			shared_ptr<Expression> floated = invoke.CopyWithChildren(std::move(floated_children));
			vector<unique_ptr<Expression>> inner_parameters;
			vector<unique_ptr<Expression>> inner_extra;
			for (idx_t i = 1; i <= inner_parameter_count; i++) {
				inner_parameters.push_back(std::move(inner_children[i]));
			}
			for (idx_t i = inner_parameter_count + 1; i < inner_children.size(); i++) {
				inner_extra.push_back(std::move(inner_children[i]));
			}
			for (auto &capture : captures) {
				inner_extra.push_back(std::move(capture));
			}
			return BindInvoke(GetContext(), std::move(floated), inner_lambda.ParameterNames(),
			                  std::move(inner_parameters), std::move(inner_extra));
		}
		return nullptr;
	}

	auto &old_names = lambda.ParameterNames();
	D_ASSERT(old_names.size() == parameter_count);
	vector<Identifier> names;
	for (idx_t parameter = 0; parameter < parameter_count; parameter++) {
		switch (actions[parameter]) {
		case ParameterAction::KEEP:
			names.push_back(old_names[parameter]);
			break;
		case ParameterAction::SUBSTITUTE:
			break;
		case ParameterAction::SPLIT: {
			auto elements = children[parameter + 1]->Cast<BoundFunctionExpression>().GetChildren().size();
			for (idx_t i = 0; i < elements; i++) {
				names.emplace_back(old_names[parameter].GetIdentifierName() + "_" + to_string(i + 1));
			}
			break;
		}
		}
	}

	auto new_body = body.Copy();
	ExpressionIterator::VisitExpressionClassMutable(
	    new_body, ExpressionClass::BOUND_REF, [&](unique_ptr<Expression> &ref_expr) {
		    auto &ref = ref_expr->Cast<BoundReferenceExpression>();
		    const auto column = ref.Index();
		    if (column >= parameter_count) {
			    ref.IndexMutable() = extra_columns[column - parameter_count].GetIndex();
			    return;
		    }
		    const auto parameter = parameter_count - 1 - column;
		    auto &argument = *children[parameter + 1];
		    switch (actions[parameter]) {
		    case ParameterAction::KEEP:
			    ref.IndexMutable() = new_count - 1 - first[parameter];
			    return;
		    case ParameterAction::SUBSTITUTE:
			    ref_expr = argument.Copy();
			    return;
		    case ParameterAction::SPLIT: {
			    auto &constructor = argument.Cast<BoundFunctionExpression>();
			    auto &elements = constructor.GetChildren();
			    vector<unique_ptr<Expression>> references;
			    references.reserve(elements.size());
			    for (idx_t i = 0; i < elements.size(); i++) {
				    const auto position = first[parameter] + i;
				    references.push_back(make_uniq<BoundReferenceExpression>(
				        names[position], elements[i]->GetReturnType(), new_count - 1 - position));
			    }
			    ref_expr = constructor.CopyWithChildren(std::move(references));
			    return;
		    }
		    }
	    });

	vector<unique_ptr<Expression>> parameters;
	parameters.reserve(new_count);
	for (idx_t parameter = 0; parameter < parameter_count; parameter++) {
		switch (actions[parameter]) {
		case ParameterAction::KEEP:
			parameters.push_back(std::move(children[parameter + 1]));
			break;
		case ParameterAction::SUBSTITUTE:
			break;
		case ParameterAction::SPLIT:
			for (auto &element : children[parameter + 1]->Cast<BoundFunctionExpression>().GetChildrenMutable()) {
				parameters.push_back(std::move(element));
			}
			break;
		}
	}
	vector<unique_ptr<Expression>> extra;
	extra.reserve(extra_count);
	for (idx_t i = 0; i < extra_columns.size(); i++) {
		if (extra_columns[i].IsValid()) {
			extra.push_back(std::move(children[parameter_count + 1 + i]));
		}
	}
	return BindInvoke(GetContext(), std::move(new_body), std::move(names), std::move(parameters), std::move(extra));
}

} // namespace duckdb
