#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/catalog/role_graph.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_foreign_server_info.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

static idx_t GranteeId(Binder &binder, const RoleSpec &grantee) {
	return grantee.IsPublic() ? ACL_ID_PUBLIC : RoleSpecId(binder.context, grantee);
}

static void WarnUngranted(ClientContext &context, const AlterPermissionsInfo &info, AclMode requested,
                          AclMode grantable, bool all, const string &object) {
	const auto applied = requested & grantable;
	if (requested == AclMode::NoRights || applied == requested || (all && applied != AclMode::NoRights)) {
		return;
	}
	const auto amount = applied == AclMode::NoRights ? "no" : "not all";
	if (info.revoke) {
		context.WarnUser(StringUtil::Format("%s privileges could be revoked for %s", amount, object),
		                 "PRIVILEGE_NOT_REVOKED");
	} else {
		context.WarnUser(StringUtil::Format("%s privileges were granted for %s", amount, object),
		                 "PRIVILEGE_NOT_GRANTED");
	}
}

static void RequireGrantableAcl(ClientContext &context, const AlterPermissionsInfo &info, const RoleAccess &role,
                                idx_t owner, std::span<const AclItem> acl, CatalogType type, std::string_view name) {
	if (info.repeats_object || role.MemberOf(owner)) {
		return;
	}
	const auto options = BestGrantor(info.grantors, owner, info.privileges, acl).options;
	if (options == AclMode::NoRights && role.HeldModes(acl) == AclMode::NoRights) {
		ThrowPermissionDenied(type, name);
	}
	WarnUngranted(context, info, info.privileges, options, info.privileges == ObjectKindOf(type).privileges,
	              StringUtil::Format("\"%s\"", name));
}

static const Permissions &TargetPermissions(const AlterPermissionsInfo &info, CatalogEntry &target) {
	if (info.overload_oid) {
		auto &macro = target.Cast<MacroCatalogEntry>();
		return macro.OverloadPermissions(*macro.FindOverload(info.overload_oid));
	}
	return target.permissions;
}

struct RelationColumn {
	reference<const Identifier> name;
	std::span<const AclItem> acl;
};

static void RequireGrantable(Binder &binder, const AlterPermissionsInfo &info, const RoleAccess &role,
                             CatalogEntry &entry) {
	if (info.repeats_object) {
		return;
	}
	auto &context = binder.context;
	auto &permissions = TargetPermissions(info, entry);
	const auto owner = permissions.owner;
	const bool checked = !role.MemberOf(owner);
	vector<AclItem> defaults;
	std::span<const AclItem> stored = permissions.acl;
	if (checked && permissions.UsesDefaultAcl()) {
		defaults = Permissions::AclDefault(ObjectKindOf(info.entry_catalog_type).acl_class, owner);
		stored = defaults;
	}
	auto &name = entry.name.GetIdentifierName();
	if (checked && info.privileges != AclMode::NoRights) {
		const auto options = BestGrantor(info.grantors, owner, info.privileges, stored).options;
		if (options == AclMode::NoRights &&
		    !role.CanAny(info.entry_catalog_type, permissions, ObjectKindOf(info.entry_catalog_type).privileges)) {
			ThrowPermissionDenied(info.entry_catalog_type, name);
		}
		WarnUngranted(context, info, info.privileges, options,
		              info.privileges == ObjectKindOf(info.entry_catalog_type).privileges,
		              StringUtil::Format("\"%s\"", name));
	}
	const auto implied = info.revoke ? info.privileges & ACL_COLUMN_PRIVILEGES : AclMode::NoRights;
	if ((entry.type != CatalogType::TABLE_ENTRY && entry.type != CatalogType::VIEW_ENTRY) ||
	    (info.column_privileges.empty() && (!checked || implied == AclMode::NoRights))) {
		return;
	}
	vector<RelationColumn> columns;
	if (entry.type == CatalogType::TABLE_ENTRY) {
		for (auto &column : entry.Cast<TableCatalogEntry>().GetColumns().Logical()) {
			columns.push_back({column.Name(), column.Acl()});
		}
	} else {
		auto &view = entry.Cast<ViewCatalogEntry>();
		view.BindView(context);
		auto column_info = view.GetColumnInfo();
		for (idx_t i = 0; i < column_info->names.size(); i++) {
			auto &column = i < view.aliases.size() ? view.aliases[i] : column_info->names[i];
			columns.push_back({column, FindColumnAcl(view.column_acls, column)});
		}
	}
	for (auto &privilege : info.column_privileges) {
		if (std::none_of(columns.begin(), columns.end(),
		                 [&](const RelationColumn &column) { return column.name.get() == privilege.column; })) {
			throw CatalogException("column \"%s\" of relation \"%s\" does not exist",
			                       privilege.column.GetIdentifierName(), name);
		}
	}
	if (!checked) {
		return;
	}
	for (auto &column : columns) {
		auto requested = implied;
		for (auto &privilege : info.column_privileges) {
			if (privilege.column == column.name.get()) {
				requested |= privilege.privileges & ACL_COLUMN_PRIVILEGES;
			}
		}
		if (requested == AclMode::NoRights) {
			continue;
		}
		auto &column_name = column.name.get().GetIdentifierName();
		const auto options = BestGrantor(info.grantors, owner, requested, stored, column.acl).options;
		if (options == AclMode::NoRights && !role.CanAny(info.entry_catalog_type, permissions, ACL_COLUMN_PRIVILEGES) &&
		    (role.HeldModes(column.acl) & ACL_COLUMN_PRIVILEGES) == AclMode::NoRights) {
			throw PermissionException("permission denied for column \"%s\" of relation \"%s\"", column_name, name);
		}
		WarnUngranted(context, info, requested, options, requested == ACL_COLUMN_PRIVILEGES,
		              StringUtil::Format("column \"%s\" of relation \"%s\"", column_name, name));
	}
}

static CatalogEntry &ResolvePermissionsTarget(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &name = info.GetQualifiedName();
	if (info.entry_catalog_type == CatalogType::SCHEMA_ENTRY) {
		auto &path = name.Path();
		auto &schema =
		    *binder.EntryRetriever().GetSchema(path.front(), vector<Identifier>(path.begin() + 1, path.end()),
		                                       AclMode::NoRights, OnEntryNotFound::THROW_EXCEPTION);
		info.SetQualifiedName(QualifiedName::FromCatalogSchema(schema.ParentCatalog().GetName(),
		                                                       schema.GetParentSchemaPath(), schema.name));
		return schema;
	}
	if (IsCatalogLevel(info.entry_catalog_type)) {
		auto &entry = *binder.EntryRetriever().GetEntry(EntryLookupInfo(info.entry_catalog_type, name));
		info.SetQualifiedName(entry.ParentCatalog().GetName(), Identifier(), entry.name);
		return entry;
	}
	optional_ptr<CatalogEntry> entry;
	if (info.entry_catalog_type == CatalogType::MACRO_ENTRY) {
		auto routine = binder.BindRoutine(
		    {name, info.func_parameters, info.has_func_args, info.include_functions, info.include_procedures},
		    AclMode::NoRights, OnEntryNotFound::THROW_EXCEPTION);
		entry = routine.entry;
		if (routine.overload && !IsUnowned(*entry)) {
			info.overload_oid = entry->Cast<MacroCatalogEntry>().OverloadOid(*routine.overload);
		}
	} else {
		entry = binder.GetCatalogEntry(EntryLookupInfo(info.entry_catalog_type, name), OnEntryNotFound::RETURN_NULL);
	}
	if (!entry) {
		throw CatalogException::MissingEntry(EntryLookupInfo(info.entry_catalog_type, name), string());
	}
	if (info.entry_catalog_type == CatalogType::VIEW_ENTRY && entry->type != CatalogType::VIEW_ENTRY) {
		throw CatalogException(Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx()),
		                       StringUtil::Format("\"%s\" is not a view", name.Name().GetIdentifierName()));
	}
	info.entry_catalog_type = entry->type == CatalogType::VIEW_ENTRY ? CatalogType::TABLE_ENTRY : entry->type;
	info.SetQualifiedName(QualifiedName::FromCatalogSchema(
	    entry->ParentCatalog().GetName(),
	    entry->ParentSchemaPath(entry->ParentCatalog().GetCatalogTransaction(context)), entry->name));
	return *entry;
}

static void RefuseSystemTablespace(const CatalogEntry &entry) {
	if (entry.type == CatalogType::TABLESPACE_ENTRY && entry.internal) {
		throw NotImplementedException(
		    unordered_map<string, string> {
		        {"detail", "A system tablespace cannot be renamed, given to another owner or granted on."}},
		    StringUtil::Format("tablespace \"%s\" is a system tablespace", entry.name.GetIdentifierName()));
	}
}

static void RefuseOwnedSequence(ClientContext &context, CatalogEntry &sequence) {
	auto &catalog = sequence.ParentCatalog();
	auto table = catalog.OwnerOf(catalog.GetCatalogTransaction(context), sequence);
	if (!table) {
		return;
	}
	auto &name = sequence.name.GetIdentifierName();
	throw NotImplementedException(
	    unordered_map<string, string> {{"detail", StringUtil::Format("Sequence \"%s\" is linked to table \"%s\".", name,
	                                                                 table->name.GetIdentifierName())}},
	    StringUtil::Format("cannot change owner of sequence \"%s\"", name));
}

static vector<idx_t> GrantorRoles(ClientContext &context, const RoleAccess &role) {
	if (role.is_superuser) {
		return {};
	}
	return context.security ? context.security->InheritedRoles(role.role) : vector<idx_t> {role.role};
}

static const RoleAccess &GrantingRole(Binder &binder, const AlterPermissionsInfo &info) {
	auto &role = binder.CheckAs();
	if (info.granted_by.empty()) {
		return role;
	}
	auto granted_by = RoleSpecId(binder.context, info.granted_by);
	if (!role.MemberOf(granted_by)) {
		throw PermissionException("must be member of role \"%s\"", RoleName(binder.context, granted_by));
	}
	return binder.AccessOf(granted_by);
}

static void ResolveGrant(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	info.grantee_id = GranteeId(binder, info.grantee);
	auto &role = GrantingRole(binder, info);
	info.grantors = GrantorRoles(context, role);
	info.best_grantor = true;
	SDB_IF_FAILURE("grant_first_grantor_with_an_option") {
		info.best_grantor = false;
		if (!role.is_superuser) {
			info.grantors.assign(1, role.role);
			info.grantors.insert(info.grantors.end(), role.closure.begin(), role.closure.end());
		}
	}
	if (info.entry_catalog_type == CatalogType::PARAMETER_ENTRY) {
		const auto setting = info.GetQualifiedName().Name();
		if (!Settings::GetAccess(context, setting)) {
			Catalog::AutoloadExtensionByConfigName(context, setting);
		}
		const auto &name = setting.GetIdentifierName();
		if (Catalog::GetSystemCatalog(context).IsDuckCatalog()) {
			throw NotImplementedException("parameter privileges are not supported by this catalog");
		}
		const auto owner = DBConfig::GetConfig(context).bootstrap_role;
		std::span<const AclItem> acl;
		if (auto parameter = binder.EntryRetriever().GetEntry(
		        EntryLookupInfo(CatalogType::PARAMETER_ENTRY, QualifiedName(setting)), OnEntryNotFound::RETURN_NULL)) {
			acl = parameter->permissions.acl;
		}
		RequireGrantableAcl(context, info, role, owner, acl, CatalogType::PARAMETER_ENTRY, name);
		info.SetQualifiedName(Identifier(), Identifier(), Identifier(StringUtil::Lower(name)));
		return;
	}
	auto &target = ResolvePermissionsTarget(binder, info);
	RefuseSystemTablespace(target);
	if (target.internal) {
		throw PermissionException("Cannot alter entry \"%s\" because it is an internal system entry",
		                          target.name.GetIdentifierName());
	}
	RequireGrantable(binder, info, role, target);
}

static void ResolveOwner(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &entry = ResolvePermissionsTarget(binder, info);
	RefuseSystemTablespace(entry);
	auto &permissions = TargetPermissions(info, entry);
	info.new_owner_id = RoleSpecId(context, info.new_owner);
	auto &role = binder.CheckAs();
	const bool enforce = !role.is_superuser;
	const bool relation = Catalog::InRelationNamespace(entry.type);
	if (enforce && relation) {
		Require(context, role, entry, AclMode::Own);
	}
	if (info.new_owner_id == permissions.owner) {
		return;
	}
	if (entry.type == CatalogType::SEQUENCE_ENTRY) {
		RefuseOwnedSequence(context, entry);
	}
	if (!enforce) {
		return;
	}
	if (!relation) {
		Require(context, role, entry, AclMode::Own,
		        info.overload_oid ? entry.Cast<MacroCatalogEntry>().FindOverload(info.overload_oid).get() : nullptr);
	}
	if (!role.CanSet(info.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", RoleName(context, info.new_owner_id));
	}
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		if (!role.Has(RoleOption::CreateDb)) {
			throw PermissionException("permission denied to change owner of database");
		}
	} else if (entry.type == CatalogType::FOREIGN_SERVER_ENTRY) {
		auto &wrapper =
		    Catalog::GetEntry(context, CatalogType::FOREIGN_DATA_WRAPPER_ENTRY,
		                      QualifiedName(entry.ParentCatalog().GetName(), Identifier(),
		                                    Identifier(entry.GetInfo()->Cast<CreateForeignServerInfo>().fdw_name)));
		Require(context, context.AccessOf(info.new_owner_id), wrapper, AclMode::Usage);
	} else if (ObjectKindOf(entry.type).container == ObjectContainer::DATABASE) {
		if (auto database = Catalog::GetEntry(
		        context, EntryLookupInfo(CatalogType::DATABASE_ENTRY, QualifiedName(entry.ParentCatalog().GetName())),
		        OnEntryNotFound::RETURN_NULL)) {
			Require(context, role, *database, AclMode::Create);
		}
	} else if (entry.type != CatalogType::TABLESPACE_ENTRY) {
		auto &schema = entry.ParentSchema(context);
		if (!Allowed(context.AccessOf(info.new_owner_id), schema, AclMode::Create)) {
			ThrowPermissionDenied(schema);
		}
	}
}

static void ResolveDefaultPrivileges(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	info.target_role = info.for_role.empty() ? context.current_role_id : RoleSpecId(context, info.for_role);
	if (!binder.CheckAs().MemberOf(info.target_role)) {
		throw PermissionException("permission denied to change default privileges");
	}
	info.grantee_id = GranteeId(binder, info.grantee);
	info.default_acl_oid = DatabaseManager::Get(context).NextOid();
	const auto database = DatabaseManager::GetDefaultDatabase(context);
	if (!info.default_schema.empty()) {
		info.default_scope = binder.EntryRetriever()
		                         .GetSchema(database, {Identifier(info.default_schema)}, AclMode::NoRights,
		                                    OnEntryNotFound::THROW_EXCEPTION)
		                         ->oid;
	}
	binder.EntryRetriever().GetEntry(EntryLookupInfo(CatalogType::DATABASE_ENTRY, QualifiedName(database)));
	info.SetQualifiedName(Identifier(), Identifier(), database);
}

static vector<idx_t> ResolveOwnedRoles(Binder &binder, const vector<RoleSpec> &names, const string &action) {
	auto &context = binder.context;
	vector<idx_t> result;
	for (auto &name : names) {
		auto role = RoleSpecId(context, name);
		if (!binder.CheckAs().MemberOf(role)) {
			throw PermissionException(
			    unordered_map<string, string> {
			        {"detail",
			         StringUtil::Format("Only roles with privileges of role \"%s\" may %s objects owned by it.",
			                            RoleName(context, role), action)}},
			    StringUtil::Format("permission denied to %s objects", action));
		}
		result.push_back(role);
	}
	return result;
}

static void RefuseBootstrapRole(Binder &binder, const vector<idx_t> &roles, const string &action) {
	const auto bootstrap = DBConfig::GetConfig(binder.context).bootstrap_role;
	if (bootstrap != 0 && std::find(roles.begin(), roles.end(), bootstrap) != roles.end()) {
		throw DependencyException("cannot %s owned by role %s because they are required by the database system", action,
		                          RoleName(binder.context, bootstrap));
	}
}

void Binder::ResolveOwned(AlterInfo &info) {
	if (info.type == AlterType::DROP_OWNED) {
		auto &drop = info.Cast<DropOwnedInfo>();
		drop.role_ids = ResolveOwnedRoles(*this, drop.roles, "drop");
		drop.grantors = GrantorRoles(context, CheckAs());
		RefuseBootstrapRole(*this, drop.role_ids, "drop objects");
		return;
	}
	auto &reassign = info.Cast<ReassignOwnedInfo>();
	reassign.role_ids = ResolveOwnedRoles(*this, reassign.roles, "reassign");
	reassign.new_owner_id = RoleSpecId(context, reassign.new_owner);
	auto &role = CheckAs();
	if (!role.CanSet(reassign.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", RoleName(context, reassign.new_owner_id));
	}
	RefuseBootstrapRole(*this, reassign.role_ids, "reassign ownership of objects");
}

void Binder::BindAlterPermissions(AlterPermissionsInfo &info, StatementProperties &properties) {
	const auto catalog_type = info.GetCatalogType();
	if (catalog_type == CatalogType::SCHEMA_ENTRY) {
		info.SetQualifiedName(ResolveCatalog(entry_retriever, info.GetQualifiedName()));
		properties.RegisterDBModify(Catalog::GetCatalog(context, info.GetQualifiedName().Path().front()), context,
		                            DatabaseModificationType::ALTER_TABLE);
	} else if (catalog_type != CatalogType::DATABASE_ENTRY && catalog_type != CatalogType::ROLE_ENTRY &&
	           catalog_type != CatalogType::TABLESPACE_ENTRY &&
	           catalog_type != CatalogType::FOREIGN_DATA_WRAPPER_ENTRY &&
	           catalog_type != CatalogType::PARAMETER_ENTRY && catalog_type != CatalogType::LANGUAGE_ENTRY) {
		info.SetQualifiedName(BindTableName(info.GetQualifiedName()));
		auto &catalog = Catalog::GetCatalog(context, info.GetQualifiedName().Catalog());
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
	}
	if (!info.new_owner.empty()) {
		ResolveOwner(*this, info);
	} else if (info.default_objtype != CatalogType::INVALID) {
		ResolveDefaultPrivileges(*this, info);
	} else {
		ResolveGrant(*this, info);
	}
}

} // namespace duckdb
