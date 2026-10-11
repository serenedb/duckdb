#include "duckdb/parser/parsed_data/create_replication_origin_info.hpp"

#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

CreateReplicationOriginInfo::CreateReplicationOriginInfo()
    : CreateInfo(CatalogType::REPLICATION_ORIGIN_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateReplicationOriginInfo::Copy() const {
	auto result = make_uniq<CreateReplicationOriginInfo>();
	CopyProperties(*result);
	result->remote_lsn = remote_lsn;
	return std::move(result);
}

string CreateReplicationOriginInfo::ToString() const {
	return "SELECT pg_replication_origin_create(" +
	       KeywordHelper::WriteQuoted(GetQualifiedName().Name().GetIdentifierName(), '\'') + ");";
}

} // namespace duckdb
