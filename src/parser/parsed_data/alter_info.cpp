#include "duckdb/parser/parsed_data/alter_info.hpp"

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
                                       QualifiedName name_p, idx_t owner_p)
    : AlterInfo(AlterType::SET_PERMISSIONS, std::move(name_p), OnEntryNotFound::THROW_EXCEPTION),
      permissions_alter_type(permissions_alter_type_p), entry_catalog_type(entry_catalog_type_p), owner(owner_p) {
}

SetPermissionsInfo::SetPermissionsInfo()
    : AlterInfo(AlterType::SET_PERMISSIONS), permissions_alter_type(PermissionsAlterType::INVALID),
      entry_catalog_type(CatalogType::INVALID), owner(0) {
}

CatalogType SetPermissionsInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> SetPermissionsInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetPermissionsInfo>(permissions_alter_type, entry_catalog_type, GetQualifiedName(),
	                                                     owner);
}

string SetPermissionsInfo::ToString() const {
	switch (permissions_alter_type) {
	case PermissionsAlterType::GRANT_PRIVILEGES:
		return "GRANT ON " + qualified_name.ToString();
	case PermissionsAlterType::REVOKE_PRIVILEGES:
		return "REVOKE ON " + qualified_name.ToString();
	case PermissionsAlterType::CHANGE_ROLE_OWNER:
		return "ALTER " + qualified_name.ToString() + " OWNER TO " + to_string(owner);
	case PermissionsAlterType::REPLACE_DEFINITION:
		return "ALTER " + qualified_name.ToString();
	default:
		throw InternalException("Unsupported permissions alter type");
	}
}

} // namespace duckdb
