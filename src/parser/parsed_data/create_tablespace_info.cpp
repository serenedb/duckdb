#include "duckdb/parser/parsed_data/create_tablespace_info.hpp"

#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

CreateTablespaceInfo::CreateTablespaceInfo() : CreateInfo(CatalogType::TABLESPACE_ENTRY, Identifier::InvalidSchema()) {
}

unique_ptr<CreateInfo> CreateTablespaceInfo::Copy() const {
	auto result = make_uniq<CreateTablespaceInfo>();
	CopyProperties(*result);
	result->location = location;
	result->owner = owner;
	return std::move(result);
}

string CreateTablespaceInfo::ToString() const {
	string result = "CREATE TABLESPACE " + SQLIdentifier(GetQualifiedName().Name());
	if (!owner.empty()) {
		result += " OWNER " + SQLIdentifier(owner);
	}
	return result + " LOCATION " + SQLString(location) + ";";
}

} // namespace duckdb
