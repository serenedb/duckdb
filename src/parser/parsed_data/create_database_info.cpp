#include "duckdb/parser/parsed_data/create_database_info.hpp"

#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreateDatabaseInfo::CreateDatabaseInfo() : CreateInfo(CatalogType::DATABASE_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateDatabaseInfo::Copy() const {
	auto result = make_uniq<CreateDatabaseInfo>();
	CopyProperties(*result);
	return std::move(result);
}

string CreateDatabaseInfo::ToString() const {
	return "CREATE DATABASE " + KeywordHelper::WriteOptionallyQuoted(GetQualifiedName().Name().GetIdentifierName()) +
	       ";";
}

} // namespace duckdb
