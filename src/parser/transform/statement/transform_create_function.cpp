#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/transformer.hpp"
#include "parser/parser.hpp"

namespace duckdb {

// Replace $N positional parameter references with named column references.
// PG uses $1, $2 in function bodies; DuckDB macros use param names directly.
static void ReplacePositionalParams(unique_ptr<ParsedExpression> &expr, const vector<string> &param_names) {
	if (!expr) {
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::PARAMETER) {
		auto &param = expr->Cast<ParameterExpression>();
		// Positional params have numeric identifiers: "1", "2", etc.
		idx_t idx = 0;
		try { idx = std::stoull(param.identifier); } catch (...) { return; }
		if (idx >= 1 && idx <= param_names.size()) {
			auto replacement = make_uniq<ColumnRefExpression>(param_names[idx - 1]);
			replacement->alias = expr->alias;
			expr = std::move(replacement);
			return;
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		ReplacePositionalParams(child, param_names);
	});
}

static void ReplacePositionalParamsInQuery(QueryNode &node, const vector<string> &param_names) {
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
		node,
		[&](unique_ptr<ParsedExpression> &child) { ReplacePositionalParams(child, param_names); },
		[&](TableRef &ref) {
			ParsedExpressionIterator::EnumerateTableRefChildren(
				ref,
				[&](unique_ptr<ParsedExpression> &child) { ReplacePositionalParams(child, param_names); });
		});
}

unique_ptr<MacroFunction> Transformer::TransformMacroFunction(duckdb_libpgquery::PGFunctionDefinition &def) {
	unique_ptr<MacroFunction> macro_func;
	if (def.function) {
		// Check if this is a PG-style string body (AS 'body' or AS $$body$$)
		// matched through the "param_list AS a_expr" path (Sconst as a_expr).
		// Parse the body string as SQL using raw_parser (which is re-entrant).
		if (def.function->type == duckdb_libpgquery::T_PGAConst) {
			auto &constant = PGCast<duckdb_libpgquery::PGAConst>(*def.function);
			if (constant.val.type == duckdb_libpgquery::T_PGString) {
				auto body_stmts = duckdb_libpgquery::raw_parser(constant.val.val.str);
				if (!body_stmts || body_stmts->length != 1) {
					throw ParserException("Function body must contain exactly one statement");
				}
				auto &raw = PGCast<duckdb_libpgquery::PGRawStmt>(
				    *static_cast<duckdb_libpgquery::PGNode *>(body_stmts->head->data.ptr_value));
				if (raw.stmt->type == duckdb_libpgquery::T_PGSelectStmt) {
					auto query_node = TransformSelectNode(*raw.stmt);
					macro_func = make_uniq<TableMacroFunction>(std::move(query_node));
				} else {
					throw ParserException("Only SELECT statements are supported in SQL function/procedure bodies");
				}
			}
		}
		if (!macro_func) {
			auto expression = TransformExpression(def.function);
			macro_func = make_uniq<ScalarMacroFunction>(std::move(expression));
		}
	} else if (def.query) {
		auto query_node = TransformSelectNode(*def.query);
		macro_func = make_uniq<TableMacroFunction>(std::move(query_node));
	}

	if (!def.params) {
		return macro_func;
	}

	case_insensitive_set_t parameter_names;
	for (auto node = def.params->head; node != nullptr; node = node->next) {
		auto target = PGPointerCast<duckdb_libpgquery::PGNode>(node->data.ptr_value);
		if (target->type != duckdb_libpgquery::T_PGFunctionParameter) {
			throw InternalException("TODO");
		}
		auto &param = PGCast<duckdb_libpgquery::PGFunctionParameter>(*target);

		// Transform parameter name/type
		if (!parameter_names.insert(param.name).second) {
			throw ParserException("Duplicate parameter '%s' in macro definition", param.name);
		}
		macro_func->parameters.emplace_back(make_uniq<ColumnRefExpression>(param.name));
		macro_func->types.emplace_back(param.typeName ? TransformTypeName(*param.typeName) : LogicalType::UNKNOWN);

		// Transform parameter default value
		if (param.defaultValue) {
			auto default_expr = TransformExpression(PGPointerCast<duckdb_libpgquery::PGNode>(param.defaultValue));
			default_expr->SetAlias(param.name);
			macro_func->default_parameters[param.name] = std::move(default_expr);
		} else if (!macro_func->default_parameters.empty()) {
			throw ParserException("Parameter without a default follows parameter with a default");
		}
	}

	// Replace $N positional params with named params in the function body
	if (!macro_func->parameters.empty()) {
		vector<string> param_name_list;
		for (auto &p : macro_func->parameters) {
			param_name_list.push_back(p->Cast<ColumnRefExpression>().GetColumnName());
		}
		if (macro_func->type == MacroType::SCALAR_MACRO) {
			auto &scalar = macro_func->Cast<ScalarMacroFunction>();
			ReplacePositionalParams(scalar.expression, param_name_list);
		} else {
			auto &table = macro_func->Cast<TableMacroFunction>();
			if (table.query_node) {
				ReplacePositionalParamsInQuery(*table.query_node, param_name_list);
			}
		}
	}

	return macro_func;
}

unique_ptr<CreateStatement> Transformer::TransformCreateFunction(duckdb_libpgquery::PGCreateFunctionStmt &stmt) {
	D_ASSERT(stmt.type == duckdb_libpgquery::T_PGCreateFunctionStmt);
	D_ASSERT(stmt.functions);

	auto result = make_uniq<CreateStatement>();
	auto qname = TransformQualifiedName(*stmt.name);

	vector<unique_ptr<MacroFunction>> macros;
	for (auto c = stmt.functions->head; c != nullptr; c = lnext(c)) {
		auto &function_def = *PGPointerCast<duckdb_libpgquery::PGFunctionDefinition>(c->data.ptr_value);
		macros.push_back(TransformMacroFunction(function_def));
	}
	PivotEntryCheck("macro");

	auto catalog_type =
	    macros[0]->type == MacroType::SCALAR_MACRO ? CatalogType::MACRO_ENTRY : CatalogType::TABLE_MACRO_ENTRY;
	auto info = make_uniq<CreateMacroInfo>(catalog_type);
	info->catalog = qname.catalog;
	info->schema = qname.schema;
	info->name = qname.name;

	// temporary macro
	switch (stmt.name->relpersistence) {
	case duckdb_libpgquery::PG_RELPERSISTENCE_TEMP:
		info->temporary = true;
		break;
	case duckdb_libpgquery::PG_RELPERSISTENCE_UNLOGGED:
		throw ParserException("Unlogged flag not supported for macros: '%s'", qname.name);
	case duckdb_libpgquery::RELPERSISTENCE_PERMANENT:
		info->temporary = false;
		break;
	default:
		throw ParserException("Unsupported persistence flag for table '%s'", qname.name);
	}

	// what to do on conflict
	info->on_conflict = TransformOnConflict(stmt.onconflict);
	info->is_procedure = stmt.is_procedure;
	info->macros = std::move(macros);

	result->info = std::move(info);

	return result;
}

} // namespace duckdb
