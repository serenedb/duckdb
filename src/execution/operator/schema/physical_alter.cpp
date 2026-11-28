#include "duckdb/execution/operator/schema/physical_alter.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/alter_database_info.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

static void Alter(ClientContext &context, AlterInfo &info) {
	Catalog::GetDDLCatalog(context, info.GetCatalogType(), info.GetQualifiedName().Catalog()).Alter(context, info);
}

static void AlterPermissions(ClientContext &context, const AlterPermissionsInfo &info) {
	auto copy = info.Copy();
	auto &alter = copy->Cast<AlterPermissionsInfo>();
	alter.objects.clear();
	alter.grantees.clear();
	alter.grantee_ids.clear();
	alter.for_roles.clear();
	alter.target_roles.clear();
	alter.default_schemas.clear();
	alter.default_scopes.clear();
	const auto apply = [&]() {
		if (info.grantee_ids.empty()) {
			Alter(context, alter);
			return;
		}
		for (idx_t i = 0; i < info.grantee_ids.size(); i++) {
			alter.grantee = info.grantees[i];
			alter.grantee_id = info.grantee_ids[i];
			if (info.default_objtype != CatalogType::INVALID) {
				alter.default_acl_oid = DatabaseManager::Get(context).NextOid();
			}
			Alter(context, alter);
		}
	};
	for (auto &object : info.objects) {
		alter.SetQualifiedName(object.name);
		alter.entry_catalog_type = object.type;
		alter.overload_oid = object.overload_oid;
		alter.func_parameters = object.func_parameters;
		alter.has_func_args = object.has_func_args;
		apply();
	}
	for (auto role : info.target_roles) {
		alter.target_role = role;
		for (auto scope : info.default_scopes) {
			alter.default_scope = scope;
			apply();
		}
	}
}

static void AlterRole(ClientContext &context, AlterRoleInfo &info) {
	if (info.granted_roles.empty()) {
		Alter(context, info);
		return;
	}
	auto copy = info.Copy();
	auto &alter = copy->Cast<AlterRoleInfo>();
	alter.granted_roles.clear();
	alter.grantor_ids.clear();
	alter.members.clear();
	for (idx_t i = 0; i < info.granted_roles.size(); i++) {
		alter.grant_role = info.granted_roles[i];
		alter.grant_role_id = 0;
		alter.grantor_id = info.grantor_ids[i];
		for (auto &member : info.members) {
			alter.SetName(member.name);
			alter.role_type = member.type;
			Alter(context, alter);
		}
	}
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalAlter::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	if (!info) {
		return SourceResultType::FINISHED;
	}
	switch (info->type) {
	case AlterType::ALTER_DATABASE:
		DatabaseManager::Get(context.client).Alter(context.client, info->Cast<AlterDatabaseInfo>());
		break;
	case AlterType::ALTER_PERMISSIONS:
		AlterPermissions(context.client, info->Cast<AlterPermissionsInfo>());
		break;
	case AlterType::ALTER_ROLE:
		AlterRole(context.client, info->Cast<AlterRoleInfo>());
		break;
	default:
		Alter(context.client, *info);
		break;
	}
	return SourceResultType::FINISHED;
}

} // namespace duckdb
