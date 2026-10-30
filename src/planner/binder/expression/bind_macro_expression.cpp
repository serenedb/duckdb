#include <absl/algorithm/container.h>
#include <absl/cleanup/cleanup.h>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"

namespace duckdb {

namespace {

constexpr idx_t MAX_ARGUMENT_LITERAL_SIZE = 1 << 12;

bool IsTrivialMacroArgument(const ParsedExpression &argument,
                            const identifier_map_t<unique_ptr<Expression>> &bound_arguments) {
	switch (argument.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		auto &column = argument.Cast<ColumnRefExpression>();
		return column.IsQualified() || !bound_arguments.count(column.GetColumnName());
	}
	case ExpressionClass::CONSTANT:
	case ExpressionClass::LAMBDA_REF:
	case ExpressionClass::PARAMETER:
	case ExpressionClass::POSITIONAL_REFERENCE:
	case ExpressionClass::STAR:
	case ExpressionClass::LAMBDA:
	case ExpressionClass::DEFAULT:
		return true;
	case ExpressionClass::CAST:
		return argument.Cast<CastExpression>().Child().GetExpressionClass() == ExpressionClass::CONSTANT;
	case ExpressionClass::FUNCTION: {
		auto &function = argument.Cast<FunctionExpression>();
		return function.FunctionName() == "struct_pack" &&
		       absl::c_all_of(function.GetArguments(), [](const FunctionArgument &field) {
			       return field.GetExpression().GetExpressionClass() == ExpressionClass::COLUMN_REF;
		       });
	}
	default:
		return false;
	}
}

optional_ptr<SelectNode> SingleItemSelect(ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::SUBQUERY) {
		return nullptr;
	}
	auto &subquery = expr.Cast<SubqueryExpression>();
	auto &node = subquery.SubqueryMutable()->node;
	if (subquery.GetSubqueryType() != SubqueryType::SCALAR || node->type != QueryNodeType::SELECT_NODE) {
		return nullptr;
	}
	auto &select = node->Cast<SelectNode>();
	if (!select.cte_map.map.empty() || select.select_list.size() != 1 || !select.modifiers.empty() ||
	    select.from_table->type != TableReferenceType::EMPTY_FROM || select.where_clause ||
	    !select.groups.group_expressions.empty() || !select.groups.grouping_sets.empty() || select.having ||
	    select.qualify || select.sample || select.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return nullptr;
	}
	return select;
}

bool HasExactResultType(const MacroFunction &macro) {
	if (macro.return_types.empty()) {
		return false;
	}
	return !TypeVisitor::Contains(macro.ResultType(), [](const LogicalType &type) {
		switch (type.id()) {
		case LogicalTypeId::INVALID:
		case LogicalTypeId::SQLNULL:
		case LogicalTypeId::UNKNOWN:
		case LogicalTypeId::ANY:
		case LogicalTypeId::TEMPLATE:
		case LogicalTypeId::UNBOUND:
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
			return true;
		default:
			return false;
		}
	});
}

bool IsLambdaParameterList(const LambdaExpression &lambda, DummyBinding &parameters,
                           vector<reference<const ParsedExpression>> &column_refs) {
	string error_message;
	column_refs = lambda.ExtractColumnRefExpressions(error_message);
	if (!error_message.empty()) {
		return false;
	}
	if (lambda.GetLambdaSyntaxType() == LambdaSyntaxType::LAMBDA_KEYWORD) {
		return true;
	}
	for (auto &column_ref_expr : column_refs) {
		auto &column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
		if (!column_ref.IsQualified() && parameters.HasMatchingBinding(column_ref.GetColumnName())) {
			return false;
		}
	}
	return true;
}

} // namespace

MacroFunctionLookup ExpressionBinder::LookupMacroFunction(const FunctionExpression &function) {
	auto &name = function.FunctionName();
	if (IsUnnestFunction(name) || IsSelectListSetReturningFunction(name)) {
		return MacroFunctionLookup();
	}
	auto &qualified_name = function.GetQualifiedName();
	auto &functions = binder.global_binder_state->macro_functions;
	auto cached = functions.find(qualified_name);
	if (cached != functions.end()) {
		return cached->second;
	}
	MacroFunctionLookup lookup;
	auto entry = GetCatalogEntry(EntryLookupInfo(CatalogType::SCALAR_FUNCTION_ENTRY, qualified_name),
	                             OnEntryNotFound::RETURN_NULL);
	if (entry) {
		switch (entry->type) {
		case CatalogType::SCALAR_FUNCTION_ENTRY: {
			lookup.kind = MacroFunctionKind::FUNCTION;
			for (auto &overload : entry->Cast<ScalarFunctionCatalogEntry>().functions.functions) {
				if (overload->GetStability() == FunctionStability::VOLATILE) {
					lookup.kind = MacroFunctionKind::VOLATILE_FUNCTION;
				}
			}
			break;
		}
		case CatalogType::MACRO_ENTRY:
			lookup.kind = MacroFunctionKind::MACRO;
			lookup.macro = entry->Cast<ScalarMacroCatalogEntry>();
			break;
		case CatalogType::AGGREGATE_FUNCTION_ENTRY:
			lookup.kind = MacroFunctionKind::AGGREGATE;
			break;
		default:
			break;
		}
	}
	functions.emplace(qualified_name, lookup);
	return lookup;
}

void ExpressionBinder::AnalyzeMacroBody(ParsedExpression &expr, bool shareable, vector<identifier_set_t> &lambda_params,
                                        DummyBinding &parameters, MacroBodyUse &use) {
	auto analyze = [&](ParsedExpression &child, bool child_shareable) {
		AnalyzeMacroBody(child, child_shareable, lambda_params, parameters, use);
	};
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		auto &column_ref = expr.Cast<ColumnRefExpression>();
		if (LambdaExpression::IsLambdaParameter(lambda_params, column_ref.GetName())) {
			return;
		}
		bool is_parameter;
		if (column_ref.IsQualified()) {
			auto &names = column_ref.ColumnNames();
			is_parameter = names[names.size() - 2].StartsWith(DummyBinding::DUMMY_NAME);
		} else {
			is_parameter = parameters.HasMatchingBinding(column_ref.GetColumnName());
		}
		if (!is_parameter) {
			return;
		}
		auto parameter = use.parameters.find(column_ref.GetColumnName());
		if (parameter == use.parameters.end()) {
			return;
		}
		parameter->second.references++;
		parameter->second.shareable &= shareable;
		for (auto &names : lambda_params) {
			parameter->second.lambda_names.insert(names.begin(), names.end());
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		auto &function = expr.Cast<FunctionExpression>();
		auto lookup = LookupMacroFunction(function);
		optional_ptr<const MacroBodyUse> inner_use;
		optional_ptr<const ScalarMacroFunction> inner_def;
		switch (lookup.kind) {
		case MacroFunctionKind::FUNCTION:
			break;
		case MacroFunctionKind::VOLATILE_FUNCTION:
			use.is_volatile = true;
			break;
		case MacroFunctionKind::MACRO:
			for (auto &overload : lookup.macro->macros) {
				auto &overload_use = GetMacroBodyUse(overload->Cast<ScalarMacroFunction>(), lookup.macro->name);
				use.is_volatile |= overload_use.is_volatile;
				use.lambda_body &= overload_use.lambda_body;
				use.level_independent &= overload_use.level_independent;
				if (lookup.macro->macros.size() == 1) {
					inner_use = &overload_use;
					inner_def = &overload->Cast<ScalarMacroFunction>();
				}
			}
			if (!inner_use) {
				shareable = false;
			}
			break;
		case MacroFunctionKind::AGGREGATE:
		case MacroFunctionKind::OTHER:
			use.lambda_body = false;
			use.level_independent = false;
			shareable = false;
			break;
		}
		idx_t position = 0;
		for (auto &argument : function.GetArgumentsMutable()) {
			auto &child = *argument.GetExpressionMutable();
			bool child_shareable = shareable;
			if (inner_use) {
				Identifier parameter_name;
				if (child.IsNamedParameter()) {
					parameter_name = child.GetAlias();
				} else if (position < inner_def->parameters.size()) {
					parameter_name = inner_def->parameters[position++]->Cast<ColumnRefExpression>().GetColumnName();
				}
				auto parameter = inner_use->parameters.find(parameter_name);
				child_shareable &= parameter != inner_use->parameters.end() && parameter->second.shareable;
			}
			if (child.GetExpressionClass() != ExpressionClass::LAMBDA) {
				analyze(child, child_shareable);
				continue;
			}
			auto &lambda = child.Cast<LambdaExpression>();
			vector<reference<const ParsedExpression>> column_refs;
			if (!IsLambdaParameterList(lambda, parameters, column_refs)) {
				analyze(*lambda.LeftMutable(), child_shareable);
				analyze(*lambda.RightMutable(), child_shareable);
				continue;
			}
			lambda_params.emplace_back();
			for (auto &column_ref : column_refs) {
				lambda_params.back().insert(column_ref.get().Cast<ColumnRefExpression>().GetName());
			}
			analyze(*lambda.RightMutable(), child_shareable);
			lambda_params.pop_back();
		}
		if (function.Filter()) {
			analyze(*function.FilterMutable(), false);
		}
		if (function.OrderBy()) {
			for (auto &order : function.OrderByMutable()->orders) {
				analyze(*order.expression, false);
			}
		}
		return;
	}
	case ExpressionClass::SUBQUERY: {
		use.lambda_body = false;
		auto &subquery = expr.Cast<SubqueryExpression>().Subquery();
		const auto level_independent = use.level_independent;
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *subquery->node, [&](unique_ptr<ParsedExpression> &child) { analyze(*child, false); });
		use.level_independent = level_independent;
		ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { analyze(child, false); });
		return;
	}
	case ExpressionClass::WINDOW:
	case ExpressionClass::STAR:
		use.lambda_body = false;
		use.level_independent = false;
		ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { analyze(child, false); });
		return;
	default:
		ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) { analyze(child, shareable); });
		return;
	}
}

const MacroBodyUse &ExpressionBinder::GetMacroBodyUse(const ScalarMacroFunction &macro_def, const Identifier &name) {
	auto &uses = binder.global_binder_state->macro_body_uses;
	auto cached = uses.find(&macro_def);
	if (cached != uses.end()) {
		return cached->second;
	}
	auto &use = uses[&macro_def];
	use.is_volatile = true;
	use.lambda_body = false;
	use.level_independent = false;

	vector<Identifier> names;
	names.reserve(macro_def.parameters.size());
	for (auto &parameter : macro_def.parameters) {
		names.push_back(parameter->Cast<ColumnRefExpression>().GetColumnName());
	}
	vector<LogicalType> types(names.size(), LogicalType::UNKNOWN);
	DummyBinding parameters(std::move(types), std::move(names), name.GetIdentifierName());
	auto analyze = [&](const ParsedExpression &expression) {
		MacroBodyUse result;
		for (auto &parameter : parameters.GetColumnNames()) {
			result.parameters[parameter];
		}
		auto body = expression.Copy();
		auto dummy_binder = Binder::CreateBinder(context);
		dummy_binder->macro_binding = &parameters;
		ExpressionBinder::QualifyColumnNames(*dummy_binder, body);
		vector<identifier_set_t> lambda_params;
		AnalyzeMacroBody(*body, true, lambda_params, parameters, result);
		return result;
	};
	if (!macro_def.query_body) {
		auto select = SingleItemSelect(*macro_def.expression);
		if (select) {
			auto &item = *select->select_list[0];
			auto result = analyze(item);
			if (result.level_independent && !result.is_volatile) {
				result.select_item = item;
				use = std::move(result);
				return use;
			}
		}
	}
	use = analyze(*macro_def.expression);
	return use;
}

BindResult ExpressionBinder::BindMacroArgumentType(unique_ptr<ParsedExpression> &argument, idx_t depth) {
	auto &probing = binder.global_binder_state->macro_type_probing;
	probing++;
	absl::Cleanup end_probe = [&probing] {
		probing--;
	};
	return BindExpression(argument, depth);
}

bool ExpressionBinder::IsLambdaSafeMacroArgument(ParsedExpression &argument) {
	MacroArgumentScan scan;
	ScanMacroArgument(argument, scan);
	return scan.lambda_body;
}

bool ExpressionBinder::IsLevelIndependentMacroArgument(ParsedExpression &argument) {
	MacroArgumentScan scan;
	ScanMacroArgument(argument, scan);
	return scan.level_independent;
}

vector<bool> ExpressionBinder::MacroArgumentsBindInPlace(FunctionExpression &function,
                                                         ScalarMacroCatalogEntry &macro_func) {
	vector<bool> result;
	const bool typed = absl::c_any_of(macro_func.macros, [](const unique_ptr<MacroFunction> &macro) {
		return absl::c_any_of(macro->types,
		                      [](const LogicalType &type) { return type.id() != LogicalTypeId::UNKNOWN; });
	});
	if (!typed) {
		return result;
	}
	const bool select_item = absl::c_any_of(macro_func.macros, [&](const unique_ptr<MacroFunction> &macro) {
		return GetMacroBodyUse(macro->Cast<ScalarMacroFunction>(), macro_func.name).select_item;
	});
	auto &arguments = function.GetArgumentsMutable();
	result.reserve(arguments.size());
	idx_t position = 0;
	for (auto &argument : arguments) {
		auto &expression = *argument.GetExpressionMutable();
		const bool named = expression.IsNamedParameter();
		bool in_place = true;
		for (auto &macro : macro_func.macros) {
			auto &macro_def = macro->Cast<ScalarMacroFunction>();
			Identifier parameter;
			if (named) {
				parameter = expression.GetAlias();
			} else if (position < macro_def.parameters.size()) {
				parameter = macro_def.parameters[position]->Cast<ColumnRefExpression>().GetColumnName();
			} else {
				continue;
			}
			auto &use = GetMacroBodyUse(macro_def, macro_func.name);
			auto entry = use.parameters.find(parameter);
			if (entry == use.parameters.end() ? named : !entry->second.shareable) {
				in_place = false;
				break;
			}
		}
		if (!named) {
			position++;
		}
		result.push_back(in_place && (!select_item || IsLevelIndependentMacroArgument(expression)));
	}
	return result;
}

void ExpressionBinder::ReplaceMacroParametersInLambda(FunctionExpression &function,
                                                      vector<identifier_set_t> &lambda_params) {
	for (auto &child : function.GetArgumentsMutable()) {
		if (child.GetExpression().GetExpressionClass() != ExpressionClass::LAMBDA) {
			ReplaceMacroParameters(child.GetExpressionMutable(), lambda_params);
			continue;
		}

		// Special-handling for LHS lambda parameters.
		// We do not replace them, and we add them to the lambda_params vector.
		auto &lambda_expr = child.GetExpressionMutable()->Cast<LambdaExpression>();
		string error_message;
		auto column_ref_expressions = lambda_expr.ExtractColumnRefExpressions(error_message);

		// The LHS is not a valid lambda parameter list, or a single-arrow LHS
		// names a macro parameter: this arrow is a JSON extract, not a lambda.
		bool treat_as_json_extract = !error_message.empty();
		if (!treat_as_json_extract && lambda_expr.GetLambdaSyntaxType() != LambdaSyntaxType::LAMBDA_KEYWORD) {
			for (const auto &column_ref_expr : column_ref_expressions) {
				const auto &column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
				if (!column_ref.IsQualified() && macro_binding->HasMatchingBinding(column_ref.GetColumnName())) {
					treat_as_json_extract = true;
					break;
				}
			}
		}
		if (treat_as_json_extract) {
			// Possibly a JSON function, replace both LHS and RHS.
			ReplaceMacroParameters(lambda_expr.LeftMutable(), lambda_params);
			ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);
			continue;
		}

		// Push the lambda parameter names of this level.
		lambda_params.emplace_back();
		for (const auto &column_ref_expr : column_ref_expressions) {
			const auto &column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
			lambda_params.back().emplace(column_ref.GetName());
		}

		// Only replace in the RHS of the expression.
		ReplaceMacroParameters(lambda_expr.RightMutable(), lambda_params);

		lambda_params.pop_back();
	}
}

void ExpressionBinder::ReplaceMacroParameters(unique_ptr<ParsedExpression> &expr,
                                              vector<identifier_set_t> &lambda_params) {
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		// If the expression is a column reference, we replace it with its argument.
		auto &col_ref = expr->Cast<ColumnRefExpression>();
		if (LambdaExpression::IsLambdaParameter(lambda_params, col_ref.GetName())) {
			return;
		}

		bool bind_macro_parameter = false;
		if (col_ref.IsQualified()) {
			// the table qualifier is the component directly before the column name
			auto &names = col_ref.ColumnNames();
			if (names[names.size() - 2].StartsWith(DummyBinding::DUMMY_NAME)) {
				bind_macro_parameter = true;
			}
		} else {
			bind_macro_parameter = macro_binding->HasMatchingBinding(col_ref.GetColumnName());
		}

		if (bind_macro_parameter) {
			D_ASSERT(macro_binding->HasMatchingBinding(col_ref.GetColumnName()));
			expr = macro_binding->ParamToArg(col_ref);
		}
		return;
	}
	case ExpressionClass::FUNCTION: {
		// Special-handling for lambdas, which are inside function expressions.
		auto &function = expr->Cast<FunctionExpression>();
		if (function.IsLambdaFunction()) {
			return ReplaceMacroParametersInLambda(function, lambda_params);
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &sq = (expr->Cast<SubqueryExpression>()).Subquery();
		ParsedExpressionIterator::EnumerateQueryNodeChildren(
		    *sq->node, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
		break;
	}
	default:
		break;
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ReplaceMacroParameters(child, lambda_params); });
}

// Find aggregate expression children
void ExpressionBinder::FindAggregateExprs(unique_ptr<ParsedExpression> &expr,
                                          vector<reference<unique_ptr<ParsedExpression>>> &exprs) {
	if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &fn_expr = expr->Cast<FunctionExpression>();

		// Look up the function in the catalog, check to see if it is actually an aggregate function
		EntryLookupInfo fn_entry(CatalogType::AGGREGATE_FUNCTION_ENTRY, fn_expr.GetQualifiedName());
		auto entry = GetCatalogEntry(fn_entry, OnEntryNotFound::RETURN_NULL);

		if (entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
			exprs.push_back(expr);
			return;
		}
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child_expr) { FindAggregateExprs(child_expr, exprs); });
}

void ExpressionBinder::UnfoldWindowMacroExpression(unique_ptr<ParsedExpression> &expr, ScalarMacroFunction &macro_def) {
	auto macro_copy = macro_def.expression->Copy();
	vector<reference<unique_ptr<ParsedExpression>>> aggregate_exprs;
	FindAggregateExprs(macro_copy, aggregate_exprs);

	// Only allowed if the macro body has a single aggregate expression
	if (aggregate_exprs.size() != 1) {
		throw BinderException("Window function macro bodies must contain exactly one aggregate function");
	}

	// The window spec is pushed down to the aggregate function target within the macro body
	unique_ptr<ParsedExpression> &agg_expr_ref = aggregate_exprs[0];
	auto &agg_fn_expr = agg_expr_ref->Cast<FunctionExpression>();

	// Transfer the macro function attributes
	auto &window_expr = expr->Cast<WindowExpression>();
	window_expr.SetQualifiedName(agg_fn_expr.GetQualifiedName());
	window_expr.GetArgumentsMutable().clear();
	for (auto &arg : agg_fn_expr.GetArgumentsMutable()) {
		window_expr.GetArgumentsMutable().push_back(std::move(arg));
	}
	if (!window_expr.Distinct()) {
		window_expr.DistinctMutable() = agg_fn_expr.Distinct();
	}
	if (window_expr.Filter() && agg_fn_expr.Filter()) {
		// Two FILTER clauses: combine
		window_expr.FilterMutable() =
		    make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(window_expr.FilterMutable()),
		                                     std::move(agg_fn_expr.FilterMutable()));
	} else if (agg_fn_expr.Filter()) {
		//	One FILTER from the MACRO
		window_expr.FilterMutable() = std::move(agg_fn_expr.FilterMutable());
	}
	// Transfer argument ORDER BYs
	if (agg_fn_expr.OrderBy()) {
		auto clone = agg_fn_expr.OrderBy()->Copy();
		window_expr.ArgOrdersMutable() = std::move(clone->Cast<OrderModifier>().orders);
	}

	// Replace the aggregate expression with the new window expression
	agg_expr_ref = std::move(expr);
	expr = std::move(macro_copy);
}

void ExpressionBinder::ScanMacroArgument(ParsedExpression &argument, MacroArgumentScan &scan) {
	switch (argument.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
		scan.constant = false;
		scan.names.insert(argument.Cast<ColumnRefExpression>().ColumnNames()[0]);
		return;
	case ExpressionClass::LAMBDA_REF:
	case ExpressionClass::POSITIONAL_REFERENCE:
		scan.constant = false;
		return;
	case ExpressionClass::PARAMETER:
		scan.constant = false;
		scan.has_parameter = true;
		return;
	case ExpressionClass::SUBQUERY: {
		scan.constant = false;
		scan.lambda_body = false;
		auto &subquery = argument.Cast<SubqueryExpression>().SubqueryMutable();
		const auto level_independent = scan.level_independent;
		ParsedExpressionIterator::EnumerateQueryNodeChildren(*subquery->node, [&](unique_ptr<ParsedExpression> &child) {
			if (!scan.blocked && !scan.is_volatile) {
				ScanMacroArgument(*child, scan);
			}
		});
		scan.level_independent = level_independent;
		break;
	}
	case ExpressionClass::WINDOW:
		scan.constant = false;
		scan.lambda_body = false;
		scan.level_independent = false;
		break;
	case ExpressionClass::STAR:
		scan.blocked = true;
		scan.lambda_body = false;
		scan.level_independent = false;
		return;
	case ExpressionClass::FUNCTION: {
		auto lookup = LookupMacroFunction(argument.Cast<FunctionExpression>());
		switch (lookup.kind) {
		case MacroFunctionKind::FUNCTION:
			break;
		case MacroFunctionKind::VOLATILE_FUNCTION:
			scan.is_volatile = true;
			break;
		case MacroFunctionKind::MACRO:
			for (auto &overload : lookup.macro->macros) {
				auto &overload_use = GetMacroBodyUse(overload->Cast<ScalarMacroFunction>(), lookup.macro->name);
				scan.is_volatile |= overload_use.is_volatile;
				scan.lambda_body &= overload_use.lambda_body;
				scan.level_independent &= overload_use.level_independent;
			}
			break;
		case MacroFunctionKind::AGGREGATE:
			scan.constant = false;
			scan.lambda_body = false;
			scan.level_independent = false;
			break;
		case MacroFunctionKind::OTHER:
			scan.blocked = true;
			scan.lambda_body = false;
			scan.level_independent = false;
			return;
		}
		break;
	}
	default:
		break;
	}
	ParsedExpressionIterator::EnumerateChildren(argument, [&](ParsedExpression &child) {
		if (scan.lambda_body || scan.level_independent || (!scan.blocked && !scan.is_volatile)) {
			ScanMacroArgument(child, scan);
		}
	});
}

bool ExpressionBinder::IsSetReturningFunction(const FunctionExpression &function) {
	if (function.FunctionName() == "unnest" || function.FunctionName() == "unlist") {
		return true;
	}
	if (GetCatalogEntry(EntryLookupInfo(CatalogType::SCALAR_FUNCTION_ENTRY, function.GetQualifiedName()),
	                    OnEntryNotFound::RETURN_NULL)) {
		return false;
	}
	return GetCatalogEntry(EntryLookupInfo(CatalogType::TABLE_FUNCTION_ENTRY, function.GetQualifiedName()),
	                       OnEntryNotFound::RETURN_NULL) != nullptr;
}

bool ExpressionBinder::InlineSqlFunctionBody(ScalarMacroFunction &function) {
	if (!function.query_body) {
		return false;
	}
	auto select = SingleItemSelect(*function.expression);
	if (!select) {
		return false;
	}
	auto &item = select->select_list[0];
	bool blocked = false;
	for (auto expression_class : {ExpressionClass::WINDOW, ExpressionClass::STAR, ExpressionClass::SUBQUERY}) {
		ParsedExpressionIterator::VisitExpressionClass(*item, expression_class,
		                                               [&](const ParsedExpression &) { blocked = true; });
	}
	ParsedExpressionIterator::VisitExpressionClass(*item, ExpressionClass::FUNCTION, [&](const ParsedExpression &expr) {
		blocked = blocked || IsSetReturningFunction(expr.Cast<FunctionExpression>());
	});
	if (blocked) {
		return false;
	}
	vector<reference<unique_ptr<ParsedExpression>>> aggregates;
	FindAggregateExprs(item, aggregates);
	if (!aggregates.empty()) {
		return false;
	}
	auto result = std::move(item);
	result->ClearAlias();
	function.expression = std::move(result);
	function.query_body = false;
	return true;
}

bool ExpressionBinder::ReturnsAtMostOneRow(SelectNode &node, bool from_parameters_only) {
	bool blocked = false;
	for (auto &item : node.select_list) {
		ParsedExpressionIterator::VisitExpressionClass(
		    *item, ExpressionClass::FUNCTION, [&](const ParsedExpression &expr) {
			    blocked = blocked || IsSetReturningFunction(expr.Cast<FunctionExpression>());
		    });
	}
	if (blocked) {
		return false;
	}
	if (from_parameters_only) {
		return true;
	}
	if (!node.groups.group_expressions.empty() || !node.groups.grouping_sets.empty() ||
	    node.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return false;
	}
	bool has_aggregate = false;
	for (auto &item : node.select_list) {
		ParsedExpressionIterator::VisitExpressionClass(*item, ExpressionClass::WINDOW,
		                                               [&](const ParsedExpression &) { blocked = true; });
		vector<reference<unique_ptr<ParsedExpression>>> aggregates;
		FindAggregateExprs(item, aggregates);
		has_aggregate = has_aggregate || !aggregates.empty();
	}
	return has_aggregate && !blocked;
}

void ExpressionBinder::ExpandSqlFunctionBody(DummyBinding &binding, vector<unique_ptr<ParsedExpression>> &arguments,
                                             unique_ptr<ParsedExpression> &expr) {
	const Identifier alias("__params_" + to_string(binder.global_binder_state->shared_macro_arguments++));
	auto &names = binding.GetColumnNames();
	vector<unique_ptr<ParsedExpression>> values;
	values.reserve(arguments.size());
	for (idx_t i = 0; i < arguments.size(); i++) {
		values.push_back(std::move(arguments[i]));
		arguments[i] = make_uniq<ColumnRefExpression>(names[i], alias);
	}
	macro_binding = &binding;
	vector<identifier_set_t> lambda_params;
	ReplaceMacroParameters(expr, lambda_params);
	AddSqlFunctionParameters(expr, alias, names, std::move(values));
}

void ExpressionBinder::AddSqlFunctionParameters(unique_ptr<ParsedExpression> &expr, const Identifier &alias,
                                                const vector<Identifier> &names,
                                                vector<unique_ptr<ParsedExpression>> arguments) {
	auto has_limit = [](const QueryNode &query) {
		return absl::c_any_of(query.modifiers, [](const unique_ptr<ResultModifier> &modifier) {
			return modifier->type == ResultModifierType::LIMIT_MODIFIER ||
			       modifier->type == ResultModifierType::LEGACY_LIMIT_PERCENT_MODIFIER;
		});
	};
	auto add_limit = [](QueryNode &query) {
		auto limit = make_uniq<LimitModifier>();
		limit->limit = ConstantExpression::Integer(1);
		query.modifiers.push_back(std::move(limit));
	};
	auto &subquery = expr->Cast<SubqueryExpression>();
	auto &body = subquery.SubqueryMutable()->node;
	bool first_row = false;
	if (body->type != QueryNodeType::SELECT_NODE) {
		if (!has_limit(*body)) {
			add_limit(*body);
			first_row = true;
		}
		auto inner = make_uniq<SelectStatement>();
		inner->node = std::move(body);
		auto wrapper = make_uniq<SelectNode>();
		wrapper->select_list.push_back(make_uniq<StarExpression>());
		wrapper->from_table = make_uniq<SubqueryRef>(std::move(inner));
		body = std::move(wrapper);
	}
	auto &node = body->Cast<SelectNode>();
	const bool from_parameters_only = !node.from_table || node.from_table->type == TableReferenceType::EMPTY_FROM;
	if (!arguments.empty()) {
		auto parameters = make_uniq<SelectNode>();
		parameters->from_table = make_uniq<EmptyTableRef>();
		parameters->select_list.reserve(arguments.size());
		for (idx_t i = 0; i < arguments.size(); i++) {
			arguments[i]->SetAlias(names[i]);
			parameters->select_list.push_back(std::move(arguments[i]));
		}
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(parameters);
		auto parameters_ref = make_uniq<SubqueryRef>(std::move(statement), alias);
		for (auto &item : node.select_list) {
			if (item->GetExpressionClass() != ExpressionClass::STAR) {
				continue;
			}
			auto &star = item->Cast<StarExpression>();
			if (!star.RelationName().empty()) {
				continue;
			}
			for (auto &name : names) {
				star.ExcludeListMutable().insert(QualifiedColumnName(alias, name));
			}
		}
		if (from_parameters_only) {
			node.from_table = std::move(parameters_ref);
		} else {
			auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
			join->left = std::move(parameters_ref);
			join->right = std::move(node.from_table);
			node.from_table = std::move(join);
		}
	}
	if (first_row || ReturnsAtMostOneRow(node, from_parameters_only)) {
		return;
	}
	if (has_limit(node)) {
		auto inner = make_uniq<SelectStatement>();
		inner->node = std::move(body);
		auto first = make_uniq<SelectNode>();
		first->select_list.push_back(make_uniq<StarExpression>());
		first->from_table = make_uniq<SubqueryRef>(std::move(inner));
		body = std::move(first);
	}
	add_limit(*body);
}

ScalarMacroFunction &ExpressionBinder::UnfoldMacroExpression(FunctionExpression &function,
                                                             ScalarMacroCatalogEntry &macro_func,
                                                             unique_ptr<ParsedExpression> &expr, idx_t depth,
                                                             vector<unique_ptr<Expression>> &shared_arguments,
                                                             bool &probed) {
	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positional_arguments;
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;
	const bool binds_values =
	    binder.GetBindingMode() == BindingMode::STANDARD_BINDING || binder.GetBindingMode() == BindingMode::PREPARE;
	const bool can_bind_arguments = expr->GetExpressionClass() != ExpressionClass::WINDOW && binds_values;
	MacroBoundArguments bound_arguments;
	if (can_bind_arguments) {
		bound_arguments.bind_in_place = MacroArgumentsBindInPlace(function, macro_func);
	}
	auto bind_result = MacroFunction::BindMacroFunction(
	    binder, macro_func.macros, macro_func.name, macro_func.ParentCatalog().Compatibility(), function,
	    positional_arguments, named_arguments, depth, lambda_bindings, can_bind_arguments ? this : nullptr,
	    can_bind_arguments ? &bound_arguments : nullptr);
	if (!bind_result.error.empty()) {
		throw BinderException(*expr, bind_result.error);
	}
	auto &macro_def = macro_func.macros[bind_result.function_idx.GetIndex()]->Cast<ScalarMacroFunction>();
	if (binder.global_binder_state->macro_type_probing && expr->GetExpressionClass() != ExpressionClass::WINDOW &&
	    HasExactResultType(macro_def)) {
		probed = true;
		return macro_def;
	}

	vector<unique_ptr<Expression>> prebound = std::move(bound_arguments.positional);
	prebound.resize(positional_arguments.size());
	for (auto &named : named_arguments) {
		auto bound = bound_arguments.named.find(named.first);
		prebound.push_back(bound == bound_arguments.named.end() ? nullptr : std::move(bound->second));
	}
	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);

	const auto query_location = function.GetQueryLocation();
	// replace current expression with stored macro expression
	// special case: If this is a window function, then we need to return a window expression
	const bool is_window = expr->GetExpressionClass() == ExpressionClass::WINDOW;
	const bool distinct = !is_window && function.Distinct();
	optional_ptr<const MacroBodyUse> use;
	if (!is_window && binds_values) {
		use = GetMacroBodyUse(macro_def, macro_func.name);
	}
	const bool inline_body = use && use->select_item &&
	                         absl::c_all_of(positional_arguments, [&](const unique_ptr<ParsedExpression> &argument) {
		                         return IsLevelIndependentMacroArgument(*argument);
	                         });
	unique_ptr<ParsedExpression> filter;
	unique_ptr<OrderModifier> order_by;
	Identifier function_name;
	if (is_window) {
		UnfoldWindowMacroExpression(expr, macro_def);
	} else {
		filter = std::move(function.FilterMutable());
		order_by = std::move(function.OrderByMutable());
		function_name = function.FunctionName();
		if (inline_body) {
			expr = use->select_item->Copy();
			expr->ClearAlias();
		} else {
			expr = macro_def.expression->Copy();
		}
	}
	const bool ordered = order_by && !order_by->orders.empty();
	const bool call_modifiers = distinct || filter || ordered;
	auto modifiers_error = [&]() {
		return InvalidInputException("Function %s is a %s. \"DISTINCT\", \"FILTER\", and \"ORDER BY\" are only "
		                             "applicable to window and aggregate functions.",
		                             function_name, CatalogTypeToString(CatalogType::MACRO_ENTRY));
	};

	// qualify only the macro parameters with a new empty binder that only knows the macro binding
	auto dummy_binder = Binder::CreateBinder(context);
	dummy_binder->macro_binding = new_macro_binding.get();
	ExpressionBinder::QualifyColumnNames(*dummy_binder, expr);

	vector<string> shared_names;
	vector<unique_ptr<ParsedExpression>> shared_parsed;
	if (use) {
		const bool can_share = (!use->select_item || inline_body) && use->lambda_body && CanShareMacroArguments();
		const bool function_arguments = !macro_def.return_types.empty();
		auto &names = new_macro_binding->GetColumnNames();
		vector<idx_t> shared_positions;
		for (idx_t i = 0; i < positional_arguments.size(); i++) {
			auto &argument = positional_arguments[i];
			auto parameter = use->parameters.find(names[i]);
			D_ASSERT(parameter != use->parameters.end());
			const bool prebound_argument = i < prebound.size() && prebound[i];
			if (parameter->second.references == 0 || (parameter->second.references == 1 && !prebound_argument) ||
			    IsTrivialMacroArgument(*argument, binder.global_binder_state->bound_macro_arguments)) {
				continue;
			}
			MacroArgumentScan scan;
			ScanMacroArgument(*argument, scan);
			if (scan.blocked || (scan.is_volatile && !function_arguments)) {
				continue;
			}
			const bool uncaptured = absl::c_none_of(
			    scan.names, [&](const Identifier &name) { return parameter->second.lambda_names.count(name) > 0; });
			const bool share = can_share && parameter->second.shareable && uncaptured;
			if (!share && !scan.constant && !prebound_argument) {
				continue;
			}
			auto parsed = argument->Copy();
			ErrorData error;
			unique_ptr<Expression> bound;
			if (prebound_argument) {
				bound = std::move(prebound[i]);
			} else {
				bound = BindChild(parsed, depth, error);
			}
			if (!bound || (scan.has_parameter && bound->GetReturnType().id() == LogicalTypeId::UNKNOWN)) {
				continue;
			}
			if (bound->IsFoldable() && !TypeVisitor::Contains(bound->GetReturnType(), [](const LogicalType &type) {
				    return type.HasAlias();
			    })) {
				Value value;
				if (ExpressionExecutor::TryEvaluateFoldable(context, *bound, value, MAX_ARGUMENT_LITERAL_SIZE)) {
					argument = ConstantExpression::FromValue(value);
					continue;
				}
			}
			if (!share) {
				if (prebound_argument && (!use->select_item || inline_body) && parameter->second.shareable &&
				    uncaptured && scan.lambda_body && CanShareMacroArguments()) {
					auto &reused = binder.global_binder_state->bound_macro_arguments;
					Identifier name("0_macro_bound_" + to_string(reused.size()));
					reused.emplace(name, std::move(bound));
					argument = make_uniq<ColumnRefExpression>(std::move(name));
				}
				continue;
			}
			shared_positions.push_back(i);
			shared_parsed.push_back(std::move(parsed));
			shared_arguments.push_back(std::move(bound));
		}
		for (idx_t i = 0; i < positional_arguments.size() && !shared_positions.empty(); i++) {
			auto parameter = use->parameters.find(names[i]);
			D_ASSERT(parameter != use->parameters.end());
			if (parameter->second.references == 0 ||
			    IsTrivialMacroArgument(*positional_arguments[i], binder.global_binder_state->bound_macro_arguments) ||
			    absl::c_linear_search(shared_positions, i)) {
				continue;
			}
			if (!IsLambdaSafeMacroArgument(*positional_arguments[i])) {
				shared_positions.clear();
				shared_parsed.clear();
				shared_arguments.clear();
			}
		}
		const auto lambda_depth = lambda_bindings ? lambda_bindings->size() : 0;
		for (auto i : shared_positions) {
			auto name = "0_macro_argument_" + to_string(lambda_depth) + "_" + to_string(shared_names.size());
			positional_arguments[i] = make_uniq<ColumnRefExpression>(Identifier(name));
			shared_names.push_back(std::move(name));
		}
	}

	if (macro_def.query_body && !is_window) {
		if (call_modifiers) {
			throw modifiers_error();
		}
		D_ASSERT(shared_names.empty());
		ExpandSqlFunctionBody(*new_macro_binding, positional_arguments, expr);
		return macro_def;
	}

	// now replace the parameters
	macro_binding = new_macro_binding.get();
	vector<identifier_set_t> lambda_params;
	ReplaceMacroParameters(expr, lambda_params);
	if (call_modifiers) {
		vector<reference<unique_ptr<ParsedExpression>>> aggregates;
		FindAggregateExprs(expr, aggregates);
		if (aggregates.empty() || (distinct && aggregates.size() != 1)) {
			throw modifiers_error();
		}
		for (auto &aggregate : aggregates) {
			auto &target = aggregate.get()->Cast<FunctionExpression>();
			target.DistinctMutable() = target.Distinct() || distinct;
			if (filter) {
				auto combined = filter->Copy();
				if (target.Filter()) {
					combined = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
					                                            std::move(target.FilterMutable()), std::move(combined));
				}
				target.FilterMutable() = std::move(combined);
			}
			if (ordered) {
				auto prefix = order_by->Copy();
				auto &call_orders = prefix->Cast<OrderModifier>().orders;
				auto &orders = target.OrderByMutable()->orders;
				orders.insert(orders.begin(), std::make_move_iterator(call_orders.begin()),
				              std::make_move_iterator(call_orders.end()));
			}
		}
	}
	if (shared_names.empty()) {
		return macro_def;
	}
	binder.global_binder_state->shared_macro_arguments += shared_names.size();
	auto alias = expr->GetAlias();
	vector<unique_ptr<ParsedExpression>> invoke_arguments;
	invoke_arguments.reserve(shared_parsed.size() + 1);
	invoke_arguments.push_back(make_uniq<LambdaExpression>(std::move(shared_names), std::move(expr)));
	for (auto &argument : shared_parsed) {
		invoke_arguments.push_back(std::move(argument));
	}
	expr = make_uniq<FunctionExpression>(
	    QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), Identifier("invoke")),
	    std::move(invoke_arguments));
	expr->SetAlias(std::move(alias));
	expr->SetQueryLocation(query_location);
	return macro_def;
}

BindResult ExpressionBinder::BindMacro(FunctionExpression &function, ScalarMacroCatalogEntry &macro_func, idx_t depth,
                                       unique_ptr<ParsedExpression> &expr) {
	auto stack_checker = StackCheck(*expr, 3);

	const bool constant_candidate =
	    function.GetArguments().empty() && expr->GetExpressionClass() == ExpressionClass::FUNCTION;
	auto &constant_macros = binder.global_binder_state->constant_macros;
	if (constant_candidate) {
		auto constant = constant_macros.find(&macro_func);
		if (constant != constant_macros.end()) {
			return BindResult(make_uniq<BoundConstantExpression>(constant->second));
		}
	}

	// unfold the macro expression
	vector<unique_ptr<Expression>> shared_arguments;
	bool probed = false;
	auto &macro_def = UnfoldMacroExpression(function, macro_func, expr, depth, shared_arguments, probed);
	if (probed) {
		return BindResult(make_uniq<BoundReferenceExpression>(macro_def.ResultType(), 0));
	}
	BindResult result;
	if (shared_arguments.empty()) {
		// bind the unfolded macro
		auto outer_lambda_bindings = lambda_bindings;
		absl::Cleanup restore_lambda_bindings = [&] {
			lambda_bindings = outer_lambda_bindings;
		};
		if (constant_candidate) {
			lambda_bindings = nullptr;
		}
		result = BindExpression(expr, depth);
	} else {
		auto &invoke = expr->Cast<FunctionExpression>();
		auto &invoke_entry =
		    GetCatalogEntry(EntryLookupInfo(CatalogType::SCALAR_FUNCTION_ENTRY, invoke.GetQualifiedName()),
		                    OnEntryNotFound::THROW_EXCEPTION)
		        ->Cast<ScalarFunctionCatalogEntry>();
		vector<unique_ptr<Expression>> children;
		vector<LogicalType> child_types;
		children.reserve(shared_arguments.size() + 1);
		child_types.reserve(shared_arguments.size() + 1);
		children.push_back(nullptr);
		child_types.push_back(LogicalType::LAMBDA);
		for (auto &argument : shared_arguments) {
			child_types.push_back(argument->GetReturnType());
			children.push_back(std::move(argument));
		}
		auto &lambda = invoke.GetArgumentsMutable()[0].GetExpressionMutable()->Cast<LambdaExpression>();
		result =
		    BindLambdaFunction(invoke, invoke_entry, lambda, 0, std::move(children), std::move(child_types), depth);
	}
	if (result.HasError()) {
		return result;
	}
	if (!macro_def.return_types.empty()) {
		auto declared = macro_def.ResultType();
		if (MacroFunction::ResultNeedsCast(result.expression->GetReturnType(), declared)) {
			result.expression = BoundCastExpression::AddCastToType(context, std::move(result.expression), declared);
		}
	}
	if (constant_candidate && result.expression->IsFoldable() && !result.expression->IsVolatile()) {
		Value value;
		if (ExpressionExecutor::TryEvaluateFoldable(context, *result.expression, value)) {
			constant_macros.emplace(&macro_func, value);
			return BindResult(make_uniq<BoundConstantExpression>(std::move(value)));
		}
	}
	return result;
}

} // namespace duckdb
