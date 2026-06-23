#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

unique_ptr<SelectStatement> PEGTransformerFactory::TransformDescribeStatement(PEGTransformer &transformer,
                                                                              unique_ptr<QueryNode> child) {
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(child);
	return select_statement;
}

unique_ptr<QueryNode>
PEGTransformerFactory::TransformShowSelect(PEGTransformer &transformer, const ShowType &show_or_describe_or_summarize,
                                           unique_ptr<SelectStatement> select_statement_internal) {
	auto result = make_uniq<ShowRef>();
	result->show_type = show_or_describe_or_summarize;
	result->query = std::move(select_statement_internal->node);
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(result);
	return std::move(select_node);
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowTables(PEGTransformer &transformer,
                                                                 const ShowType &show_or_describe,
                                                                 const QualifiedName &qualified_name) {
	auto showref = make_uniq<ShowRef>();
	showref->show_type = ShowType::SHOW_FROM;
	if (!IsInvalidCatalog(qualified_name.Catalog())) {
		throw ParserException("Expected \"SHOW TABLES FROM database\", \"SHOW TABLES FROM schema\", or "
		                      "\"SHOW TABLES FROM database.schema\"");
	}
	if (IsInvalidSchema(qualified_name.Schema())) {
		showref->SetSchemaName(qualified_name.Name());
	} else {
		showref->SetCatalogName(qualified_name.Schema());
		showref->SetSchemaName(qualified_name.Name());
	}
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(showref);
	return std::move(select_node);
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowAllTables(PEGTransformer &transformer,
                                                                    const ShowType &show_or_describe) {
	auto result = make_uniq<ShowRef>();
	// Legacy reasons, see bind_showref.cpp
	result->SetTableName("__show_tables_expanded");
	result->show_type = ShowType::SHOW_UNQUALIFIED;
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(result);
	return std::move(select_node);
}

// SHOW ALL -> SELECT name, setting, short_desc AS description FROM pg_settings
unique_ptr<QueryNode> PEGTransformerFactory::TransformShowAllSettings(PEGTransformer &transformer,
                                                                      const ShowType &show_or_describe) {
	auto result = make_uniq<SelectNode>();
	result->select_list.emplace_back(make_uniq<ColumnRefExpression>("name"));
	result->select_list.emplace_back(make_uniq<ColumnRefExpression>("setting"));
	auto desc_col = make_uniq<ColumnRefExpression>("short_desc");
	desc_col->SetAlias("description");
	result->select_list.emplace_back(std::move(desc_col));
	auto tableref = make_uniq<BaseTableRef>();
	tableref->SetTable("pg_settings");
	result->from_table = std::move(tableref);
	return std::move(result);
}

// Walk into ShowOrDescribeOrSummarize to find the original keyword (`SHOW`,
// `DESCRIBE`, `DESC`, `SUMMARIZE`). We use this to distinguish PG `SHOW
// varname` (-> current_setting) from DuckDB `DESC <table>` (-> describe).
static string ExtractShowKeyword(ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// ShowOrDescribeOrSummarize <- ShowOrDescribe / Summarize
	auto &outer_choice = list_pr.Child<ChoiceParseResult>(0).GetResult();
	if (outer_choice.name == "ShowOrDescribe") {
		// ShowOrDescribe <- ShowRule / DescribeRule
		auto &inner_choice = outer_choice.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		return string(inner_choice.name);
	}
	return string(outer_choice.name);
}

unique_ptr<QueryNode> PEGTransformerFactory::TransformShowQualifiedName(PEGTransformer &transformer,
                                                                        ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto showref = make_uniq<ShowRef>();
	showref->show_type = transformer.Transform<ShowType>(list_pr.Child<ListParseResult>(0));
	auto keyword = ExtractShowKeyword(list_pr.Child<ListParseResult>(0));

	auto &opt_target = list_pr.Child<OptionalParseResult>(1);
	if (opt_target.HasResult()) {
		auto target = transformer.Transform<DescribeTarget>(opt_target.GetResult());
		if (target.is_table_name) {
			// Case: SHOW 'something' or DESCRIBE 'something'
			showref->SetTableName(target.table_name);
		} else {
			// Case: A relation/table reference
			auto &base_table = *target.table_ref;

			if (showref->show_type == ShowType::SHOW_FROM) {
				// Logic for SHOW TABLES FROM [database].[schema]
				if (IsInvalidSchema(base_table.GetQualifiedName().Schema())) {
					showref->SetSchemaName(base_table.Table());
				} else {
					showref->SetCatalogName(base_table.GetQualifiedName().Schema());
					showref->SetSchemaName(base_table.Table());
				}
			} else if (IsInvalidSchema(base_table.GetQualifiedName().Schema())) {
				// Logic for unqualified relations (databases, tables, variables)
				auto table_name = StringUtil::Lower(base_table.Table().GetIdentifierName());
				if (table_name == "databases" || table_name == "tables" || table_name == "schemas" ||
				    table_name == "variables") {
					showref->SetTableName(Identifier("\"" + table_name + "\""));
					showref->show_type = ShowType::SHOW_UNQUALIFIED;
				} else if (keyword == "ShowRule") {
					// PG-compat: SHOW <unqualified_name> -> SELECT current_setting('name') AS "name"
					// DESC/DESCRIBE still goes through the table-description path below.
					auto result = make_uniq<SelectNode>();
					vector<unique_ptr<ParsedExpression>> args;
					args.push_back(make_uniq<ConstantExpression>(Value(base_table.Table().GetIdentifierName())));
					auto func_expr = make_uniq<FunctionExpression>("current_setting", std::move(args));
					// PG-compat: PG returns these three GUCs with their CamelCase canonical names as the
					// column header regardless of how the caller cased the identifier; drivers
					// compare case-sensitively (pgjdbc parameter_status, Npgsql cache keys).
					Identifier alias = base_table.Table();
					if (StringUtil::CIEquals(alias.GetIdentifierName(), "timezone")) {
						alias = "TimeZone";
					} else if (StringUtil::CIEquals(alias.GetIdentifierName(), "datestyle")) {
						alias = "DateStyle";
					} else if (StringUtil::CIEquals(alias.GetIdentifierName(), "intervalstyle")) {
						alias = "IntervalStyle";
					}
					func_expr->SetAlias(alias);
					result->select_list.push_back(std::move(func_expr));
					result->from_table = make_uniq<EmptyTableRef>();
					return std::move(result);
				}
			}
		}
		if (showref->GetTableName().empty() && showref->show_type != ShowType::SHOW_FROM) {
			auto show_select_node = make_uniq<SelectNode>();
			show_select_node->select_list.push_back(make_uniq<StarExpression>());
			if (target.is_table_name) {
				// Case: SHOW 'something' or DESCRIBE 'something'
				auto table_ref = make_uniq<BaseTableRef>();
				table_ref->SetTable(target.table_name);
				show_select_node->from_table = std::move(table_ref);
			} else {
				// Case: A relation/table reference
				show_select_node->from_table = std::move(target.table_ref);
			}
			showref->query = std::move(show_select_node);
		}
	} else {
		// Case: No relation specified (e.g., just "SHOW TABLES")
		if (showref->show_type == ShowType::SUMMARY) {
			throw ParserException("Expected table name with SUMMARIZE");
		}
		showref->SetTableName("__show_tables_expanded");
		showref->show_type = ShowType::SHOW_UNQUALIFIED;
	}

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(showref);

	return std::move(select_node);
}

DescribeTarget PEGTransformerFactory::TransformDescribeBaseTableName(PEGTransformer &transformer,
                                                                     unique_ptr<BaseTableRef> base_table_name) {
	DescribeTarget result;
	result.table_ref = std::move(base_table_name);
	return result;
}

// ShowAliasedSetting <- ShowOrDescribe ShowSettingAlias
// ShowSettingAlias  <- ('TRANSACTION' 'ISOLATION' 'LEVEL') / ('SESSION' 'AUTHORIZATION') / ('TIME' 'ZONE')
// PG-compat: each alias collapses to SHOW <varname>. Routed through current_setting() so the
// shape matches the regular SHOW <name> branch in TransformShowQualifiedName.
unique_ptr<QueryNode> PEGTransformerFactory::TransformShowAliasedSetting(PEGTransformer &transformer,
                                                                         ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &alias_list = list_pr.Child<ListParseResult>(1);
	auto &choice_pr = alias_list.Child<ChoiceParseResult>(0);
	auto &alts = choice_pr.GetResult().Cast<ListParseResult>();
	auto &first_kw = alts.Child<KeywordParseResult>(0).keyword;

	// PG-compat: PG-canonical GUC names. transaction_isolation / session_authorization are lowercase
	// in PG; timezone is the rare CamelCase outlier (TimeZone). Drivers compare the
	// result column header case-sensitively, so emit the canonical case verbatim.
	string setting_name;
	if (StringUtil::CIEquals(first_kw, "TRANSACTION")) {
		setting_name = "transaction_isolation";
	} else if (StringUtil::CIEquals(first_kw, "SESSION")) {
		setting_name = "session_authorization";
	} else {
		setting_name = "TimeZone";
	}

	auto result = make_uniq<SelectNode>();
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(make_uniq<ConstantExpression>(Value(setting_name)));
	auto func_expr = make_uniq<FunctionExpression>("current_setting", std::move(args));
	func_expr->SetAlias(Identifier(setting_name));
	result->select_list.push_back(std::move(func_expr));
	result->from_table = make_uniq<EmptyTableRef>();
	return std::move(result);
}

DescribeTarget PEGTransformerFactory::TransformDescribeStringLiteral(PEGTransformer &transformer,
                                                                     const string &string_literal) {
	DescribeTarget result;
	result.is_table_name = true;
	result.table_name = Identifier(string_literal);
	return result;
}

ShowType PEGTransformerFactory::TransformSummarizeRule(PEGTransformer &transformer) {
	return ShowType::SUMMARY;
}

ShowType PEGTransformerFactory::TransformShowRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

ShowType PEGTransformerFactory::TransformDescribeLongRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

ShowType PEGTransformerFactory::TransformDescRule(PEGTransformer &transformer) {
	return ShowType::DESCRIBE;
}

} // namespace duckdb
