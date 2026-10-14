#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
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

	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);
	new_macro_binding->arguments = &positional_arguments;

	// We need an ExpressionBinder so that we can call ExpressionBinder::ReplaceMacroParametersRecursive()
	auto eb = ExpressionBinder(*this, this->context);

	eb.macro_binding = new_macro_binding.get();
	vector<identifier_set_t> lambda_params;

	unique_ptr<QueryNode> node;
	if (macro_def.type == MacroType::SCALAR_MACRO) {
		auto expr = macro_def.Cast<ScalarMacroFunction>().expression->Copy();
		if (macro_def.return_names.size() > 1 && expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
			auto &record = expr->Cast<SubqueryExpression>().SubqueryMutable()->node->Cast<SelectNode>();
			auto join = make_uniq<JoinRef>();
			join->type = JoinType::LEFT;
			join->left = make_uniq<EmptyTableRef>();
			join->right = std::move(record.from_table);
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
			expr->SetAlias(macro_def.return_names.empty() ? macro_func.name : Identifier(macro_def.return_names[0]));
			select_node->select_list.push_back(std::move(expr));
			select_node->from_table = make_uniq<EmptyTableRef>();
			node = std::move(select_node);
		}
	} else {
		node = macro_def.Cast<TableMacroFunction>().query_node->Copy();
	}
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    *node, [&](unique_ptr<ParsedExpression> &child) { eb.ReplaceMacroParameters(child, lambda_params); });

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
