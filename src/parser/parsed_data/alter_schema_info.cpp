#include "duckdb/parser/parsed_data/alter_schema_info.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

RenameSchemaInfo::RenameSchemaInfo()
    : AlterInfo(AlterType::ALTER_SCHEMA, QualifiedName(), OnEntryNotFound::THROW_EXCEPTION) {
}

RenameSchemaInfo::RenameSchemaInfo(Identifier schema_p, Identifier new_name_p, OnEntryNotFound if_not_found)
    : AlterInfo(AlterType::ALTER_SCHEMA, QualifiedName(Identifier(), std::move(schema_p), Identifier()), if_not_found),
      new_name(std::move(new_name_p)) {
}

CatalogType RenameSchemaInfo::GetCatalogType() const {
	return CatalogType::SCHEMA_ENTRY;
}

unique_ptr<AlterInfo> RenameSchemaInfo::Copy() const {
	auto result = make_uniq<RenameSchemaInfo>(GetQualifiedName().Schema(), new_name, if_not_found);
	result->oid = oid;
	return result;
}

string RenameSchemaInfo::ToString() const {
	string result = "ALTER SCHEMA ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += StringUtil::Format("%s RENAME TO %s", GetQualifiedName().Schema(), new_name);
	return result;
}

} // namespace duckdb
