#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/function/scalar_macro_function.hpp"

namespace duckdb {

unique_ptr<QueryNode> Binder::BindTableMacro(FunctionExpression &function, MacroCatalogEntry &macro_func, idx_t depth,
                                             optional_ptr<MacroFunction> &definition) {
	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positional_arguments;
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;

	auto bind_result = MacroFunction::BindMacroFunction(*this, macro_func.macros, macro_func.name,
	                                                    macro_func.ParentCatalog().Compatibility(), function,
	                                                    positional_arguments, named_arguments, depth);
	if (!bind_result.error.empty()) {
		throw BinderException(function, bind_result.error);
	}
	auto &macro_def = *macro_func.macros[bind_result.function_idx.GetIndex()];
	definition = macro_def;
	const bool owned = !IsUnowned(macro_func);
	const auto owner =
	    owned && macro_def.security_definer ? optional_idx(macro_func.permissions.owner) : optional_idx();
	bool other_role = false;
	if (owned && principal) {
		const auto role = FunctionBodyRole(owner);
		other_role = role != principal || role != Caller();
	}

	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);
	new_macro_binding->arguments = &positional_arguments;

	// We need an ExpressionBinder so that we can call ExpressionBinder::ReplaceMacroParametersRecursive()
	auto eb = ExpressionBinder(*this, this->context);

	eb.macro_binding = new_macro_binding.get();
	vector<identifier_set_t> lambda_params;
	optional_ptr<const ExpressionBinder> caller;
	if (HasEnclosingScope()) {
		caller = GetInnermostScope();
	}
	MacroCall call {caller, {}};

	unique_ptr<QueryNode> node;
	unique_ptr<TableRef> parameters;
	if (macro_def.type == MacroType::SCALAR_MACRO) {
		auto &scalar_macro = macro_def.Cast<ScalarMacroFunction>();
		auto expr = scalar_macro.expression->Copy();
		if (other_role) {
			ExpressionBinder::MarkFunctionBody(*expr, owner);
		}
		if (macro_def.return_names.size() > 1 && scalar_macro.query_body) {
			auto &record = expr->Cast<SubqueryExpression>().SubqueryMutable()->node->Cast<SelectNode>();
			auto first_row = make_uniq<SelectNode>();
			first_row->select_list.emplace_back(make_uniq<StarExpression>());
			first_row->from_table = std::move(record.from_table);
			auto limit = make_uniq<LimitModifier>();
			limit->limit = ConstantExpression::Integer(1);
			first_row->modifiers.push_back(std::move(limit));
			auto first_row_statement = make_uniq<SelectStatement>();
			first_row_statement->node = std::move(first_row);
			auto join = make_uniq<JoinRef>();
			join->type = JoinType::LEFT;
			join->left = make_uniq<EmptyTableRef>();
			join->right = make_uniq<SubqueryRef>(std::move(first_row_statement));
			join->condition = ConstantExpression::Boolean(true);
			auto select_node = make_uniq<SelectNode>();
			select_node->select_list.emplace_back(make_uniq<StarExpression>());
			select_node->from_table = std::move(join);
			node = std::move(select_node);
		} else if (macro_def.return_names.size() > 1) {
			auto fields = make_uniq<StarExpression>();
			fields->ExpressionMutable() = make_uniq<CastExpression>(macro_def.ResultType(), std::move(expr));
			auto select_node = make_uniq<SelectNode>();
			select_node->select_list.emplace_back(std::move(fields));
			select_node->from_table = make_uniq<EmptyTableRef>();
			node = std::move(select_node);
		} else {
			auto select_node = make_uniq<SelectNode>();
			if (scalar_macro.query_body) {
				eb.ExpandSqlFunctionBody(*new_macro_binding, positional_arguments, expr, caller);
			}
			expr->SetAlias(macro_def.return_names.empty() ? macro_func.name : Identifier(macro_def.return_names[0]));
			select_node->select_list.push_back(std::move(expr));
			select_node->from_table = make_uniq<EmptyTableRef>();
			if (scalar_macro.query_body) {
				return std::move(select_node);
			}
			node = std::move(select_node);
		}
	} else {
		node = macro_def.Cast<TableMacroFunction>().query_node->Copy();
	}
	if (macro_def.type == MacroType::TABLE_MACRO || other_role) {
		parameters = eb.BindTableMacroArguments(*node, *new_macro_binding, positional_arguments, other_role);
	}
	ParsedExpressionIterator::EnumerateQueryNodeChildren(*node, [&](unique_ptr<ParsedExpression> &child) {
		eb.ReplaceMacroParameters(child, lambda_params, call, true);
	});
	if (parameters || other_role) {
		const Identifier body_alias("0_macro_body");
		auto body = make_uniq<SelectStatement>();
		body->node = std::move(node);
		body->function_body = other_role;
		body->function_owner = owner;
		auto body_ref = make_uniq<SubqueryRef>(std::move(body), body_alias);
		auto select_node = make_uniq<SelectNode>();
		select_node->select_list.push_back(make_uniq<StarExpression>(body_alias));
		if (parameters) {
			auto join = make_uniq<JoinRef>(JoinRefType::CROSS);
			join->left = std::move(parameters);
			join->right = std::move(body_ref);
			select_node->from_table = std::move(join);
		} else {
			select_node->from_table = std::move(body_ref);
		}
		node = std::move(select_node);
	}

	if (macro_def.type == MacroType::TABLE_MACRO && !macro_def.return_types.empty()) {
		auto body = make_uniq<SelectStatement>();
		body->node = std::move(node);
		auto body_ref = make_uniq<SubqueryRef>(std::move(body), macro_func.name);
		if (macro_def.return_names.empty()) {
			body_ref->column_name_alias.emplace_back(macro_func.name);
		}
		for (auto &name : macro_def.return_names) {
			body_ref->column_name_alias.emplace_back(name);
		}
		auto select_node = make_uniq<SelectNode>();
		select_node->select_list.push_back(make_uniq<StarExpression>());
		select_node->from_table = std::move(body_ref);
		node = std::move(select_node);
	}
	return node;
}

} // namespace duckdb
