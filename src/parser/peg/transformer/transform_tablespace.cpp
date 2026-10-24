#include "duckdb/parser/parsed_data/alter_tablespace_info.hpp"
#include "duckdb/parser/parsed_data/create_tablespace_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateTablespaceStatement(PEGTransformer &transformer,
                                                                                   ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto info = make_uniq<CreateTablespaceInfo>();
	info->SetName(transformer.Transform<Identifier>(list_pr.GetChild(2)));
	auto &owner = list_pr.Child<OptionalParseResult>(3);
	if (owner.HasResult()) {
		info->owner = TransformOwnerRoleSpec(transformer, owner.GetResult().Cast<ListParseResult>().GetChild(1));
	}
	info->location = transformer.Transform<string>(list_pr.GetChild(5));
	auto result = make_uniq<CreateStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropTablespaceStatement(PEGTransformer &transformer,
                                                                                 ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::TABLESPACE_ENTRY;
	result->info->SetQualifiedName(Identifier(), Identifier(), transformer.Transform<Identifier>(list_pr.GetChild(3)));
	result->info->if_not_found = list_pr.Child<OptionalParseResult>(2).HasResult() ? OnEntryNotFound::RETURN_NULL
	                                                                               : OnEntryNotFound::THROW_EXCEPTION;
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterTablespaceStatement(PEGTransformer &transformer,
                                                                                  ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	AlterEntryData data(
	    QualifiedName(Identifier(), Identifier(), transformer.Transform<Identifier>(list_pr.GetChild(2))),
	    OnEntryNotFound::THROW_EXCEPTION);
	auto result = make_uniq<AlterStatement>();
	result->info =
	    make_uniq<AlterTablespaceInfo>(std::move(data), transformer.Transform<Identifier>(list_pr.GetChild(5)));
	return std::move(result);
}

Identifier PEGTransformerFactory::TransformTablespaceClause(PEGTransformer &transformer, const Identifier &col_id) {
	return col_id;
}

Identifier PEGTransformerFactory::TransformDatabaseTablespace(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.Transform<Identifier>(parse_result.Cast<ListParseResult>().GetChild(3));
}

} // namespace duckdb
