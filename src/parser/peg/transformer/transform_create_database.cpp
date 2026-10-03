#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/attach_statement.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

// CREATE DATABASE foo [WITH (BLOCK_SIZE = n, ROW_GROUP_SIZE = n)] -> ATTACH '' AS foo (TYPE serenedb, ...)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateDatabaseStatement(
    PEGTransformer &transformer, const optional<bool> &if_not_exists, const Identifier &catalog_name,
    optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list) {
	auto result = make_uniq<AttachStatement>();
	auto info = make_uniq<AttachInfo>();
	info->name = catalog_name;
	info->path = "";
	info->options["type"] = Value("serenedb");
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
			auto &value = option.second->Cast<ConstantExpression>().GetValue();
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

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformDropDatabaseStatement(PEGTransformer &transformer, const optional<bool> &if_exists,
                                                      const Identifier &catalog_name,
                                                      const optional<bool> &drop_database_force) {
	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_drop_database";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(catalog_name.GetIdentifierName())));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(static_cast<bool>(if_exists))));
	return std::move(result);
}

bool PEGTransformerFactory::TransformDropDatabaseForce(PEGTransformer &transformer) {
	return true;
}

} // namespace duckdb
