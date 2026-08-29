#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {
unique_ptr<CreateStatement>
PEGTransformerFactory::TransformCreateSchemaStmt(PEGTransformer &transformer, const optional<bool> &if_not_exists,
                                                 const optional<QualifiedName> &name,
                                                 const optional<Identifier> &authorization) {
	if (!name && !authorization) {
		throw ParserException("CREATE SCHEMA requires a schema name or AUTHORIZATION");
	}
	// PG: "CREATE SCHEMA AUTHORIZATION role" names the schema after the role that is to own it.
	auto qualified_name = name ? *name : QualifiedName(*authorization);
	if (!qualified_name.Catalog().empty()) {
		throw ParserException("CREATE SCHEMA too many dots: expected \"catalog.schema\" or \"schema\"");
	}
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateSchemaInfo>();
	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	info->SetQualifiedName(QualifiedName(qualified_name.Schema(), qualified_name.Name(), Identifier()));
	if (authorization) {
		info->authorization = *authorization;
	}

	result->info = std::move(info);
	return result;
}

} // namespace duckdb
