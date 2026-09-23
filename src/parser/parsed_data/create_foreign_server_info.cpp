#include "duckdb/parser/parsed_data/create_foreign_server_info.hpp"

#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreateForeignServerInfo::CreateForeignServerInfo()
    : CreateInfo(CatalogType::FOREIGN_SERVER_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateForeignServerInfo::Copy() const {
	auto result = make_uniq<CreateForeignServerInfo>();
	CopyProperties(*result);
	result->server_type = server_type;
	result->version = version;
	result->fdw_name = fdw_name;
	result->options = options;
	return std::move(result);
}

string CreateForeignServerInfo::ToString() const {
	return "CREATE SERVER " + KeywordHelper::WriteOptionallyQuoted(GetQualifiedName().Name().GetIdentifierName()) + ";";
}

} // namespace duckdb
