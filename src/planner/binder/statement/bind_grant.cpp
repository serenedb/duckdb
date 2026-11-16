#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
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

static void RequireGrantableAcl(const RoleAccess &role, idx_t owner, std::span<const AclItem> acl, CatalogType type,
                                std::string_view name) {
	if (!role.Owns(owner) && (role.HeldModes(acl) | role.GrantableModes(acl)) == AclMode::NoRights) {
		ThrowPermissionDenied(type, name);
	}
}

static void TargetDatabaseRow(AlterPermissionsInfo &info, idx_t owner, const Identifier &database) {
	info.object_owner_id = owner;
	info.entry_catalog_type = CatalogType::DATABASE_ENTRY;
	info.SetQualifiedName(Identifier(), Identifier(), database);
}

static void ResolveBuiltinFunctionGrant(Binder &binder, CatalogEntry &function, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &config = DBConfig::GetConfig(context);
	auto &name = function.name.GetIdentifierName();
	const auto overloads = FunctionOverloadCount(function);
	string key;
	if (info.has_func_args) {
		for (auto &type : info.func_parameters) {
			binder.BindLogicalType(type);
		}
		const auto wanted = config.builtin_signature_key(function, info.func_parameters);
		for (idx_t overload = 0; overload < overloads && key.empty(); overload++) {
			if (config.builtin_function_key(function, overload) == wanted) {
				key = wanted;
			}
		}
		if (key.empty()) {
			auto extra_info = Exception::InitializeExtraInfo("MISSING_ENTRY", optional_idx());
			extra_info["type"] = CatalogTypeToString(CatalogType::MACRO_ENTRY);
			throw CatalogException(extra_info,
			                       StringUtil::Format("function %s(%s) does not exist", name,
			                                          MacroFunction::ParameterTypesToString(info.func_parameters)));
		}
	} else {
		for (idx_t overload = 0; overload < overloads; overload++) {
			auto candidate = config.builtin_function_key(function, overload);
			if (key.empty()) {
				key = std::move(candidate);
			} else if (candidate != key) {
				auto extra_info = Exception::InitializeExtraInfo("AMBIGUOUS_FUNCTION", optional_idx());
				extra_info["hint"] = "Specify the argument list to select the function unambiguously.";
				throw CatalogException(extra_info, StringUtil::Format("function name \"%s\" is not unique", name));
			}
		}
	}
	const auto database_name = DatabaseManager::GetDefaultDatabase(context);
	auto &database = ClusterDatabase(context, database_name);
	const auto owner = config.bootstrap_role;
	auto builtin = BuiltinFunctionAcl(function, owner);
	auto row = database.permissions.FindRow(CatalogType::MACRO_ENTRY, 0, key);
	RequireGrantableAcl(binder.CheckAs(), owner, row ? row->acl : builtin, function.type, name);
	info.database_rows = {GrantRow {CatalogType::MACRO_ENTRY, 0, std::move(key), std::move(builtin), {}}};
	TargetDatabaseRow(info, owner, database_name);
}

static void ResolveBuiltinTypeGrant(Binder &binder, idx_t type, const string &name, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	const auto database_name = DatabaseManager::GetDefaultDatabase(context);
	auto &database = ClusterDatabase(context, database_name);
	const auto owner = DBConfig::GetConfig(context).bootstrap_role;
	auto builtin = Permissions::AclDefault(CatalogType::TYPE_ENTRY, owner);
	auto row = database.permissions.FindRow(CatalogType::TYPE_ENTRY, type);
	RequireGrantableAcl(binder.CheckAs(), owner, row ? row->acl : builtin, CatalogType::TYPE_ENTRY, name);
	info.database_rows = {GrantRow {CatalogType::TYPE_ENTRY, type, string(), std::move(builtin), {}}};
	TargetDatabaseRow(info, owner, database_name);
}

static const Permissions &TargetPermissions(Binder &binder, const AlterPermissionsInfo &info, CatalogEntry &target) {
	if (info.overload_oid) {
		auto &macro = target.Cast<MacroCatalogEntry>();
		return macro.OverloadPermissions(*macro.FindOverload(info.overload_oid));
	}
	return EffectivePermissions(binder.context, target);
}

static void RequireGrantable(Binder &binder, CatalogEntry &entry, const Permissions &permissions) {
	auto &role = binder.CheckAs();
	if (role.Owns(permissions.owner)) {
		return;
	}
	vector<AclItem> defaults;
	std::span<const AclItem> stored = permissions.acl;
	if (stored.empty()) {
		defaults = Permissions::AclDefault(ObjectKindOf(entry.type).acl_class, permissions.owner);
		stored = defaults;
	}
	auto held = role.HeldModes(stored) | role.GrantableModes(stored);
	auto columns = EffectiveColumnAcls(binder.context, entry);
	if (!columns && entry.type == CatalogType::VIEW_ENTRY) {
		columns = &entry.Cast<ViewCatalogEntry>().column_acls;
	}
	if (columns) {
		for (auto &column : *columns) {
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
}

static CatalogEntry &ResolvePermissionsTarget(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &name = info.GetQualifiedName();
	if (info.entry_catalog_type == CatalogType::SCHEMA_ENTRY) {
		auto &schema = *binder.EntryRetriever().GetSchema(name.Catalog(), {name.Name()}, EntryUse::REFERENCE,
		                                                  OnEntryNotFound::THROW_EXCEPTION);
		info.SetQualifiedName(schema.ParentCatalog().GetName(), Identifier(), schema.name);
		return schema;
	}
	const auto container = ObjectKindOf(info.entry_catalog_type).container;
	if (container == ObjectContainer::DATABASE) {
		return binder.ResolveDatabaseEntry(info.entry_catalog_type, info, EntryUse::REFERENCE);
	}
	if (container == ObjectContainer::CLUSTER) {
		return *ClusterEntry(context, RoleAccess::System(), info.entry_catalog_type, name.Name(), EntryUse::REFERENCE,
		                     OnEntryNotFound::THROW_EXCEPTION);
	}
	optional_ptr<CatalogEntry> entry;
	if (info.entry_catalog_type == CatalogType::MACRO_ENTRY) {
		auto routine = binder.BindRoutine(
		    {name, info.func_parameters, info.has_func_args, info.include_functions, info.include_procedures},
		    EntryUse::REFERENCE, OnEntryNotFound::THROW_EXCEPTION);
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
		auto &name = info.GetQualifiedName().Name().GetIdentifierName();
		auto &config = DBConfig::GetConfig(context);
		auto &wrapper = config.GetForeignDataWrapper(name);
		const auto database_name = DatabaseManager::GetDefaultDatabase(context);
		auto &database = ClusterDatabase(context, database_name);
		RequireGrantableAcl(role, config.bootstrap_role,
		                    database.permissions.RowAcl(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, wrapper.oid),
		                    CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, name);
		info.database_rows = {GrantRow {CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, wrapper.oid, string(), {}, {}}};
		TargetDatabaseRow(info, config.bootstrap_role, database_name);
		return;
	}
	if (info.entry_catalog_type == CatalogType::LANGUAGE_ENTRY) {
		auto &name = info.GetQualifiedName().Name().GetIdentifierName();
		auto &config = DBConfig::GetConfig(context);
		auto &language = config.GetLanguage(name);
		if (!language.trusted) {
			auto extra_info = Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx());
			extra_info["detail"] = "GRANT and REVOKE are not allowed on untrusted languages, because only superusers "
			                       "can use untrusted languages.";
			throw CatalogException(extra_info, StringUtil::Format("language \"%s\" is not trusted", name));
		}
		const auto database_name = DatabaseManager::GetDefaultDatabase(context);
		auto &database = ClusterDatabase(context, database_name);
		auto stored = database.permissions.RowAcl(CatalogType::LANGUAGE_ENTRY, language.oid);
		auto acl = stored.empty() ? Permissions::AclDefault(CatalogType::LANGUAGE_ENTRY, config.bootstrap_role)
		                          : vector<AclItem>(stored.begin(), stored.end());
		RequireGrantableAcl(role, config.bootstrap_role, acl, CatalogType::LANGUAGE_ENTRY, name);
		info.database_rows = {GrantRow {CatalogType::LANGUAGE_ENTRY, language.oid, string(), {}, {}}};
		TargetDatabaseRow(info, config.bootstrap_role, database_name);
		return;
	}
	if (info.entry_catalog_type == CatalogType::PARAMETER_ENTRY) {
		const auto setting = info.GetQualifiedName().Name();
		if (!Settings::GetAccess(context, setting)) {
			Catalog::AutoloadExtensionByConfigName(context, setting);
		}
		const auto &name = setting.GetIdentifierName();
		const auto owner = DBConfig::GetConfig(context).bootstrap_role;
		const Identifier holder(RoleName(context, owner));
		auto entry = ClusterEntry(context, CatalogType::ROLE_ENTRY, holder);
		if (!entry) {
			throw NotImplementedException("parameter privileges are not supported by this catalog");
		}
		RequireGrantableAcl(role, owner, entry->permissions.RowAcl(CatalogType::PARAMETER_ENTRY, 0, name),
		                    CatalogType::PARAMETER_ENTRY, name);
		info.database_rows = {GrantRow {CatalogType::PARAMETER_ENTRY, 0, StringUtil::Lower(name), {}, {}}};
		info.object_owner_id = owner;
		info.entry_catalog_type = CatalogType::ROLE_ENTRY;
		info.SetQualifiedName(Identifier(), Identifier(), holder);
		return;
	}
	if (info.all_in_schema) {
		return;
	}
	auto &target = ResolvePermissionsTarget(binder, info);
	auto &config = DBConfig::GetConfig(context);
	if (target.internal && FunctionOverloadCount(target) && config.builtin_function_key &&
	    !config.builtin_function_key(target, 0).empty()) {
		ResolveBuiltinFunctionGrant(binder, target, info);
		return;
	}
	if (target.internal && target.type == CatalogType::TYPE_ENTRY && config.builtin_type_oid) {
		string name;
		if (auto type = config.builtin_type_oid(target.Cast<TypeCatalogEntry>().user_type, name)) {
			ResolveBuiltinTypeGrant(binder, type, name, info);
			return;
		}
	}
	RequireGrantable(binder, target, TargetPermissions(binder, info, target));
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
	const bool schema = target.type == CatalogType::SCHEMA_ENTRY;
	if (!target.internal || IsUnowned(target) || (!schema && !relation) ||
	    !DefaultSchemaGenerator::IsDefaultSchema(schema ? target.name : target.ParentSchema(context).name)) {
		return;
	}
	info.database_rows = {GrantRow {schema ? CatalogType::SCHEMA_ENTRY : CatalogType::TABLE_ENTRY,
	                                target.oid,
	                                string(),
	                                target.permissions.acl,
	                                {}}};
	TargetDatabaseRow(info, target.permissions.owner, target.ParentCatalog().GetName());
}

static void ResolveOwner(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	auto &entry = ResolvePermissionsTarget(binder, info);
	auto &permissions = TargetPermissions(binder, info, entry);
	info.new_owner_id = RoleSpecId(context, info.new_owner);
	auto &role = binder.CheckAs();
	const bool enforce = !role.is_superuser;
	const bool relation = Catalog::InRelationNamespace(entry.type);
	if (enforce && relation) {
		RequireOwner(context, role, entry);
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
	if (entry.type == CatalogType::SUBSCRIPTION_ENTRY) {
		RequireUse(context, role, entry, EntryUse::OWN);
	} else if (!relation) {
		RequireOwner(role, entry, permissions.owner);
	}
	if (!role.CanSet(info.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", RoleName(context, info.new_owner_id));
	}
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		if (!role.Has(RoleOption::CreateDb)) {
			throw PermissionException("permission denied to change owner of database");
		}
	} else if (entry.type == CatalogType::FOREIGN_SERVER_ENTRY) {
		RequireForeignDataWrapperUsage(context, context.AccessOf(info.new_owner_id), entry.ParentCatalog().GetName(),
		                               entry.GetInfo()->Cast<CreateForeignServerInfo>().fdw_name);
	} else if (ObjectKindOf(entry.type).container == ObjectContainer::DATABASE) {
		RequireDatabasePrivilege(context, role, entry.ParentCatalog().GetName(), AclMode::Create);
	} else {
		auto &schema = entry.ParentSchema(context);
		if (!context.AccessOf(info.new_owner_id).Can(CatalogType::SCHEMA_ENTRY, schema.permissions, AclMode::Create)) {
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
	const auto database = DatabaseManager::GetDefaultDatabase(context);
	if (!info.default_schema.empty()) {
		info.default_scope = binder.EntryRetriever()
		                         .GetSchema(database, {Identifier(info.default_schema)}, EntryUse::REFERENCE,
		                                    OnEntryNotFound::THROW_EXCEPTION)
		                         ->oid;
	}
	ClusterDatabase(context, database);
	info.SetQualifiedName(Identifier(), Identifier(), database);
}

static idx_t ResolveOwnedRole(Binder &binder, const string &name, const string &action, const string &detail) {
	auto &context = binder.context;
	auto role = RoleSpecId(context, name);
	if (!binder.CheckAs().MemberOf(role)) {
		throw PermissionException(
		    unordered_map<string, string> {{"detail", StringUtil::Format(detail, RoleName(context, role), action)}},
		    StringUtil::Format("permission denied to %s objects", action));
	}
	return role;
}

static vector<idx_t> ResolveOwnedRoles(Binder &binder, const vector<string> &names, const string &action) {
	vector<idx_t> result;
	for (auto &name : names) {
		result.push_back(ResolveOwnedRole(binder, name, action,
		                                  "Only roles with privileges of role \"%s\" may %s objects owned by it."));
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
	reassign.new_owner_id = ResolveOwnedRole(*this, reassign.new_owner, "reassign",
	                                         "Only roles with privileges of role \"%s\" may %s objects to it.");
	RefuseBootstrapRole(*this, reassign.role_ids, "reassign ownership of objects");
}

static void ResolvePermissions(Binder &binder, AlterPermissionsInfo &info) {
	if (!info.new_owner.empty()) {
		ResolveOwner(binder, info);
	} else if (info.default_objtype != CatalogType::INVALID) {
		ResolveDefaultPrivileges(binder, info);
	} else {
		ResolveGrant(binder, info);
	}
}

void Binder::BindPrivilegeAlter(AlterInfo &info, StatementProperties &properties) {
	const auto catalog_type = info.GetCatalogType();
	if (catalog_type != CatalogType::DATABASE_ENTRY && catalog_type != CatalogType::ROLE_ENTRY &&
	    catalog_type != CatalogType::TABLESPACE_ENTRY && catalog_type != CatalogType::FOREIGN_DATA_WRAPPER_ENTRY &&
	    catalog_type != CatalogType::PARAMETER_ENTRY && catalog_type != CatalogType::LANGUAGE_ENTRY) {
		info.SetQualifiedName(BindTableName(info.GetQualifiedName()));
		auto &catalog = Catalog::GetCatalog(context, info.GetQualifiedName().Catalog());
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
	}
	if (info.type == AlterType::ALTER_PERMISSIONS) {
		ResolvePermissions(*this, info.Cast<AlterPermissionsInfo>());
	} else if (info.type == AlterType::ALTER_DATABASE_SETTINGS) {
		auto &settings = info.Cast<AlterDatabaseSettingsInfo>();
		if (!settings.role.empty()) {
			settings.role_id = RoleSpecId(context, settings.role);
		}
		CheckAlterDatabaseSettings(settings);
		Settings::ResolveCurrentValues(context, settings.set_config);
	} else {
		if (const auto role = RoleSpecKeyword(context, info.GetQualifiedName().Name().GetIdentifierName());
		    role.IsValid()) {
			info.SetQualifiedName(Identifier(), Identifier(), Identifier(RoleName(context, role.GetIndex())));
		}
		auto &alter = info.Cast<AlterRoleInfo>();
		CheckAlterRole(alter);
		Settings::ResolveCurrentValues(context, alter.set_config);
	}
	const auto target_type = info.GetCatalogType();
	if (target_type == CatalogType::DATABASE_ENTRY || target_type == CatalogType::ROLE_ENTRY) {
		info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
	}
}

} // namespace duckdb
