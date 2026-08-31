#include "duckdb/parser/parsed_data/alter_info.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"

#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

AlterInfo::AlterInfo(AlterType type, QualifiedName name_p, OnEntryNotFound if_not_found)
    : ParseInfo(TYPE), type(type), if_not_found(if_not_found), allow_internal(false),
      qualified_name(std::move(name_p)) {
}

AlterInfo::AlterInfo(AlterType type) : ParseInfo(TYPE), type(type) {
}

AlterInfo::~AlterInfo() {
}

bool AlterInfo::TargetsSharedRelationGrammar() const {
	switch (type) {
	case AlterType::ALTER_TABLE: {
		auto alter_table_type = Cast<AlterTableInfo>().alter_table_type;
		return alter_table_type == AlterTableType::RENAME_TABLE ||
		       alter_table_type == AlterTableType::SET_TABLE_OPTIONS ||
		       alter_table_type == AlterTableType::RESET_TABLE_OPTIONS;
	}
	case AlterType::ALTER_VIEW:
		return Cast<AlterViewInfo>().alter_view_type == AlterViewType::RENAME_VIEW;
	default:
		return false;
	}
}

AlterEntryData AlterInfo::GetAlterEntryData() const {
	return AlterEntryData(GetQualifiedName(), if_not_found);
}

bool AlterInfo::IsAddIndexedConstraint() const {
	if (type != AlterType::ALTER_TABLE) {
		return false;
	}

	auto &table_info = Cast<AlterTableInfo>();
	if (table_info.alter_table_type != AlterTableType::ADD_CONSTRAINT) {
		return false;
	}

	auto &constraint_info = table_info.Cast<AddConstraintInfo>();
	return constraint_info.constraint->type == ConstraintType::UNIQUE;
}

SetPermissionsInfo::SetPermissionsInfo(PermissionsAlterType permissions_alter_type_p, CatalogType entry_catalog_type_p,
                                       QualifiedName name_p, CatalogPermissions permissions_p)
    : AlterInfo(AlterType::SET_PERMISSIONS, std::move(name_p), OnEntryNotFound::THROW_EXCEPTION),
      permissions_alter_type(permissions_alter_type_p), entry_catalog_type(entry_catalog_type_p),
      permissions(std::move(permissions_p)) {
}

SetPermissionsInfo::SetPermissionsInfo()
    : AlterInfo(AlterType::SET_PERMISSIONS), permissions_alter_type(PermissionsAlterType::INVALID),
      entry_catalog_type(CatalogType::INVALID) {
}

void AclItem::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<idx_t>(100, "grantee", grantee);
	serializer.WritePropertyWithDefault<idx_t>(101, "grantor", grantor);
	serializer.WritePropertyWithDefault<uint64_t>(102, "privs", static_cast<uint64_t>(privs));
	serializer.WritePropertyWithDefault<uint64_t>(103, "grant_option", static_cast<uint64_t>(grant_option));
}

AclItem AclItem::Deserialize(Deserializer &deserializer) {
	AclItem result;
	deserializer.ReadPropertyWithDefault<idx_t>(100, "grantee", result.grantee);
	deserializer.ReadPropertyWithDefault<idx_t>(101, "grantor", result.grantor);
	result.privs = static_cast<AclMode>(deserializer.ReadPropertyWithDefault<uint64_t>(102, "privs"));
	result.grant_option = static_cast<AclMode>(deserializer.ReadPropertyWithDefault<uint64_t>(103, "grant_option"));
	return result;
}

void ColumnAclItem::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<idx_t>(100, "catalog_oid", catalog_oid);
	serializer.WritePropertyWithDefault<vector<AclItem>>(101, "acl", acl);
}

ColumnAclItem ColumnAclItem::Deserialize(Deserializer &deserializer) {
	ColumnAclItem result;
	deserializer.ReadPropertyWithDefault<idx_t>(100, "catalog_oid", result.catalog_oid);
	deserializer.ReadPropertyWithDefault<vector<AclItem>>(101, "acl", result.acl);
	return result;
}

void CatalogPermissions::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<idx_t>(100, "owner", owner);
	serializer.WritePropertyWithDefault<vector<AclItem>>(101, "acl", acl);
	serializer.WritePropertyWithDefault<vector<ColumnAclItem>>(102, "column_acl", column_acl);
}

CatalogPermissions CatalogPermissions::Deserialize(Deserializer &deserializer) {
	CatalogPermissions result;
	deserializer.ReadPropertyWithDefault<idx_t>(100, "owner", result.owner);
	deserializer.ReadPropertyWithDefault<vector<AclItem>>(101, "acl", result.acl);
	deserializer.ReadPropertyWithDefault<vector<ColumnAclItem>>(102, "column_acl", result.column_acl);
	return result;
}

CatalogType SetPermissionsInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> SetPermissionsInfo::Copy() const {
	auto result = make_uniq_base<AlterInfo, SetPermissionsInfo>(permissions_alter_type, entry_catalog_type,
	                                                            GetQualifiedName(), permissions);
	result->oid = oid;
	return result;
}

string SetPermissionsInfo::ToString() const {
	switch (permissions_alter_type) {
	case PermissionsAlterType::GRANT_PRIVILEGES:
		return "GRANT ON " + qualified_name.ToString();
	case PermissionsAlterType::REVOKE_PRIVILEGES:
		return "REVOKE ON " + qualified_name.ToString();
	case PermissionsAlterType::CHANGE_ROLE_OWNER:
		return "ALTER " + qualified_name.ToString() + " OWNER TO " + to_string(permissions.owner);
	case PermissionsAlterType::REPLACE_DEFINITION:
		return "ALTER " + qualified_name.ToString();
	default:
		throw InternalException("Unsupported permissions alter type");
	}
}

} // namespace duckdb
