#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_foreign_server_info.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

static idx_t GranteeId(Binder &binder, const string &name) {
	return name == "public" ? ACL_ID_PUBLIC : RoleSpecId(binder.context, name);
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
	if (role.Owns(owner)) {
		return;
	}
	if ((role.HeldModes(acl) | role.GrantableModes(acl)) == AclMode::NoRights) {
		ThrowPermissionDenied(type, name);
	}
	WarnUngranted(context, info, info.privileges, role.GrantableModes(acl),
	              info.privileges == ObjectKindOf(type).privileges, StringUtil::Format("\"%s\"", name));
}

static const Permissions &TargetPermissions(const AlterPermissionsInfo &info, CatalogEntry &target) {
	if (info.overload_oid) {
		auto &macro = target.Cast<MacroCatalogEntry>();
		return macro.OverloadPermissions(*macro.FindOverload(info.overload_oid));
	}
	return target.permissions;
}

static void RequireGrantable(Binder &binder, const AlterPermissionsInfo &info, CatalogEntry &entry,
                             const Permissions &permissions) {
	auto &role = binder.CheckAs();
	if (role.Owns(permissions.owner)) {
		return;
	}
	vector<AclItem> defaults;
	std::span<const AclItem> stored = permissions.acl;
	if (permissions.UsesDefaultAcl()) {
		defaults = Permissions::AclDefault(ObjectKindOf(entry.type).acl_class, permissions.owner);
		stored = defaults;
	}
	auto held = role.HeldModes(stored) | role.GrantableModes(stored);
	if (entry.type == CatalogType::VIEW_ENTRY) {
		for (auto &column : entry.Cast<ViewCatalogEntry>().column_acls) {
			held |= role.HeldModes(column.acl) | role.GrantableModes(column.acl);
		}
	} else if (entry.type == CatalogType::TABLE_ENTRY) {
		for (auto &column : entry.Cast<TableCatalogEntry>().GetColumns().Logical()) {
			held |= role.HeldModes(column.Acl()) | role.GrantableModes(column.Acl());
		}
	}
	if (held == AclMode::NoRights) {
		ThrowPermissionDenied(entry);
	}
	auto &context = binder.context;
	const auto grantable = role.GrantableModes(stored);
	const auto all = ObjectKindOf(info.entry_catalog_type).privileges;
	WarnUngranted(context, info, info.privileges, grantable, info.privileges == all,
	              StringUtil::Format("\"%s\"", entry.name.GetIdentifierName()));
	for (auto &privilege : info.column_privileges) {
		auto column_grantable = grantable;
		if (entry.type == CatalogType::TABLE_ENTRY) {
			auto &table = entry.Cast<TableCatalogEntry>();
			if (table.ColumnExists(privilege.column)) {
				column_grantable |= role.GrantableModes(table.GetColumn(privilege.column).Acl());
			}
		} else if (entry.type == CatalogType::VIEW_ENTRY) {
			for (auto &column : entry.Cast<ViewCatalogEntry>().column_acls) {
				if (column.column == privilege.column) {
					column_grantable |= role.GrantableModes(column.acl);
				}
			}
		}
		WarnUngranted(context, info, privilege.privileges & ACL_COLUMN_PRIVILEGES, column_grantable,
		              privilege.privileges == all,
		              StringUtil::Format("column \"%s\" of relation \"%s\"", privilege.column.GetIdentifierName(),
		                                 entry.name.GetIdentifierName()));
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
	const auto container = ObjectKindOf(info.entry_catalog_type).container;
	if (container == ObjectContainer::DATABASE) {
		return binder.ResolveDatabaseEntry(info.entry_catalog_type, info, AclMode::NoRights);
	}
	if (container == ObjectContainer::CLUSTER) {
		return *ClusterEntry(context, RoleAccess::System(), info.entry_catalog_type, name.Name(), AclMode::NoRights,
		                     OnEntryNotFound::THROW_EXCEPTION);
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

static vector<idx_t> CallerGrantors(Binder &binder) {
	vector<idx_t> grantors;
	auto &role = binder.CheckAs();
	if (!role.is_superuser) {
		grantors.push_back(role.role);
		grantors.insert(grantors.end(), role.closure.begin(), role.closure.end());
	}
	return grantors;
}

static void ResolveGrant(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &role = binder.CheckAs();
	info.grantee_id = GranteeId(binder, info.grantee);
	if (!info.granted_by.empty()) {
		auto granted_by = RoleSpecId(context, info.granted_by);
		if (!role.MemberOf(granted_by)) {
			throw PermissionException("must be member of role \"%s\"", RoleName(context, granted_by));
		}
		info.grantors = {granted_by};
	} else {
		info.grantors = CallerGrantors(binder);
	}
	if (info.entry_catalog_type == CatalogType::FOREIGN_DATA_WRAPPER_ENTRY) {
		const auto wrapper = info.GetQualifiedName().Name();
		auto &config = DBConfig::GetConfig(context);
		config.GetForeignDataWrapper(wrapper.GetIdentifierName());
		const auto database_name = DatabaseManager::GetDefaultDatabase(context);
		ClusterDatabase(context, database_name);
		auto &catalog = Catalog::GetCatalog(context, database_name);
		std::span<const AclItem> acl;
		if (auto entry = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context),
		                                             CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, wrapper)) {
			acl = entry->permissions.acl;
		}
		RequireGrantableAcl(context, info, role, config.bootstrap_role, acl, CatalogType::FOREIGN_DATA_WRAPPER_ENTRY,
		                    wrapper.GetIdentifierName());
		info.object_owner_id = config.bootstrap_role;
		info.SetQualifiedName(database_name, Identifier(), wrapper);
		return;
	}
	if (info.entry_catalog_type == CatalogType::LANGUAGE_ENTRY) {
		auto &name = info.GetQualifiedName().Name().GetIdentifierName();
		DBConfig::GetConfig(context).GetLanguage(name);
		throw PermissionException("Cannot alter entry \"%s\" because it is an internal system entry", name);
	}
	if (info.entry_catalog_type == CatalogType::PARAMETER_ENTRY) {
		const auto setting = info.GetQualifiedName().Name();
		if (!Settings::GetAccess(context, setting)) {
			Catalog::AutoloadExtensionByConfigName(context, setting);
		}
		const auto &name = setting.GetIdentifierName();
		auto &system = Catalog::GetSystemCatalog(context);
		if (system.IsDuckCatalog()) {
			throw NotImplementedException("parameter privileges are not supported by this catalog");
		}
		const auto owner = DBConfig::GetConfig(context).bootstrap_role;
		std::span<const AclItem> acl;
		if (auto parameter = system.LookupTopLevelEntry(system.GetCatalogTransaction(context),
		                                                CatalogType::PARAMETER_ENTRY, setting)) {
			acl = parameter->permissions.acl;
		}
		RequireGrantableAcl(context, info, role, owner, acl, CatalogType::PARAMETER_ENTRY, name);
		info.object_owner_id = owner;
		info.SetQualifiedName(Identifier(), Identifier(), Identifier(StringUtil::Lower(name)));
		return;
	}
	auto &target = ResolvePermissionsTarget(binder, info);
	RefuseSystemTablespace(target);
	if (target.internal) {
		throw PermissionException("Cannot alter entry \"%s\" because it is an internal system entry",
		                          target.name.GetIdentifierName());
	}
	RequireGrantable(binder, info, target, TargetPermissions(info, target));
	const bool relation = target.type == CatalogType::TABLE_ENTRY || target.type == CatalogType::VIEW_ENTRY;
	if (relation && !info.column_privileges.empty()) {
		vector<Identifier> view_columns;
		if (target.type == CatalogType::VIEW_ENTRY) {
			auto &view = target.Cast<ViewCatalogEntry>();
			view.BindView(context);
			auto names = view.GetColumnInfo()->names;
			for (idx_t i = 0; i < names.size(); i++) {
				view_columns.push_back(i < view.aliases.size() ? view.aliases[i] : names[i]);
			}
		}
		for (auto &privilege : info.column_privileges) {
			const bool exists =
			    target.type == CatalogType::TABLE_ENTRY
			        ? target.Cast<TableCatalogEntry>().ColumnExists(privilege.column)
			        : std::find(view_columns.begin(), view_columns.end(), privilege.column) != view_columns.end();
			if (!exists) {
				throw CatalogException("column \"%s\" of relation \"%s\" does not exist",
				                       privilege.column.GetIdentifierName(), target.name.GetIdentifierName());
			}
		}
	}
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
	if (!relation && !info.overload_oid) {
		Require(context, role, entry, AclMode::Own);
	} else if (!relation && !Allowed(role, entry, AclMode::Own,
	                                 entry.Cast<MacroCatalogEntry>().FindOverload(info.overload_oid).get())) {
		throw PermissionException("must be owner of function %s", entry.name.GetIdentifierName());
	}
	if (!role.CanSet(info.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", RoleName(context, info.new_owner_id));
	}
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		if (!role.Has(RoleOption::CreateDb)) {
			throw PermissionException("permission denied to change owner of database");
		}
	} else if (entry.type == CatalogType::FOREIGN_SERVER_ENTRY) {
		RequireForeignDataWrapperUsage(context, context.AccessOf(info.new_owner_id), entry.ParentCatalog(),
		                               entry.GetInfo()->Cast<CreateForeignServerInfo>().fdw_name);
	} else if (ObjectKindOf(entry.type).container == ObjectContainer::DATABASE) {
		RequireDatabasePrivilege(context, role, entry.ParentCatalog().GetName(), AclMode::Create);
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
	ClusterDatabase(context, database);
	info.SetQualifiedName(Identifier(), Identifier(), database);
}

static vector<idx_t> ResolveOwnedRoles(Binder &binder, const vector<string> &names, const string &action) {
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
		drop.grantors = CallerGrantors(*this);
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
	if (role.is_superuser || role.Has(RoleOption::CreateDb)) {
		return;
	}
	bool database = false;
	auto &cluster = Catalog::GetSystemCatalog(context);
	cluster.ScanTopLevelEntries(
	    cluster.GetCatalogTransaction(context), CatalogType::DATABASE_ENTRY, [&](CatalogEntry &entry) {
		    const auto owner = entry.permissions.owner;
		    database = database ||
		               (owner != reassign.new_owner_id && std::find(reassign.role_ids.begin(), reassign.role_ids.end(),
		                                                            owner) != reassign.role_ids.end());
	    });
	if (database) {
		throw PermissionException("permission denied to change owner of database");
	}
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
	const auto target_type = info.GetCatalogType();
	if (target_type == CatalogType::DATABASE_ENTRY || target_type == CatalogType::ROLE_ENTRY) {
		info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
	}
}

} // namespace duckdb
