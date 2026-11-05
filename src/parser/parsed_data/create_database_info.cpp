#include "duckdb/parser/parsed_data/create_database_info.hpp"

#include <algorithm>

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreateDatabaseInfo::CreateDatabaseInfo() : CreateInfo(CatalogType::DATABASE_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateDatabaseInfo::Copy() const {
	auto result = make_uniq<CreateDatabaseInfo>();
	CopyProperties(*result);
	result->options = options;
	result->settings = settings;
	result->connection_limit = connection_limit;
	result->allow_connections = allow_connections;
	return std::move(result);
}

string CreateDatabaseInfo::ToString() const {
	string result =
	    "CREATE DATABASE " + KeywordHelper::WriteOptionallyQuoted(GetQualifiedName().Name().GetIdentifierName());
	if (!allow_connections) {
		result += " ALLOW_CONNECTIONS FALSE";
	}
	if (connection_limit != -1) {
		result += " CONNECTION LIMIT " + std::to_string(connection_limit);
	}
	if (!options.empty()) {
		vector<string> options_sql;
		for (auto &option : options) {
			options_sql.push_back(StringUtil::Upper(option.first) + " = " + option.second.ToSQLString());
		}
		std::sort(options_sql.begin(), options_sql.end());
		result += " WITH (" + StringUtil::Join(options_sql, ", ") + ")";
	}
	return result + ";";
}

} // namespace duckdb
