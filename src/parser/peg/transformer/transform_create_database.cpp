#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/attach_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

// CREATE DATABASE foo [WITH (BLOCK_SIZE = n, ROW_GROUP_SIZE = n)] -> ATTACH '' AS foo (TYPE serenedb, ...)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateDatabaseStatement(
    PEGTransformer &transformer, const optional<bool> &if_not_exists, const Identifier &catalog_name,
    optional<case_insensitive_map_t<Value>> create_database_options,
    optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list) {
	auto result = make_uniq<AttachStatement>();
	auto info = make_uniq<AttachInfo>();
	info->name = catalog_name;
	info->path = "";
	info->options["type"] = Value("serenedb");
	if (create_database_options) {
		for (auto &option : *create_database_options) {
			info->options.emplace(option.first, std::move(option.second));
		}
	}
	if (with_list) {
		for (auto &option : *with_list) {
			auto name = StringUtil::Lower(option.first);
			if (name != "block_size" && name != "row_group_size") {
				throw ParserException("Unrecognized CREATE DATABASE option \"%s\": the options are BLOCK_SIZE and "
				                      "ROW_GROUP_SIZE",
				                      option.first);
			}
			if (option.second->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
				throw ParserException("CREATE DATABASE option \"%s\" must be a constant", option.first);
			}
			auto value = option.second->Cast<ConstantExpression>().GetLiteral().ToValue();
			if (value.IsNull()) {
				throw ParserException("CREATE DATABASE option \"%s\" needs a value", option.first);
			}
			info->options[name] = value;
		}
	}
	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropDatabaseStatement(
    PEGTransformer &transformer, const optional<bool> &if_exists, const Identifier &catalog_name,
    const optional<bool> &drop_database_force, const optional<bool> &drop_behavior) {
	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::DATABASE_ENTRY;
	result->info->SetQualifiedName(Identifier(), Identifier(), catalog_name);
	result->info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	result->info->cascade = drop_behavior && *drop_behavior;
	return std::move(result);
}

bool PEGTransformerFactory::TransformDropDatabaseForce(PEGTransformer &transformer) {
	return true;
}

case_insensitive_map_t<Value>
PEGTransformerFactory::TransformCreateDatabaseOptions(PEGTransformer &transformer, const bool &has_result,
                                                      const vector<pair<string, Value>> &create_database_option) {
	case_insensitive_map_t<Value> result;
	for (auto &option : create_database_option) {
		if (!result.emplace(option.first, option.second).second) {
			throw ParserException("conflicting or redundant options");
		}
	}
	return result;
}

pair<string, Value> PEGTransformerFactory::TransformCreateDatabaseOwner(PEGTransformer &transformer,
                                                                        const string &database_owner_name) {
	return {"owner", Value(database_owner_name)};
}

string PEGTransformerFactory::TransformDatabaseOwnerName(PEGTransformer &transformer, const bool &has_result,
                                                         const Identifier &col_id) {
	return col_id.GetIdentifierName();
}

} // namespace duckdb
