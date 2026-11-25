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

#include <absl/algorithm/container.h>

namespace duckdb {

static idx_t GranteeId(Binder &binder, const RoleSpec &grantee) {
	return grantee.IsPublic() ? ACL_ID_PUBLIC : binder.BindRole(grantee)->oid;
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
	if (role.MemberOf(owner)) {
		return;
	}
	const auto options = BestGrantor(info.grantors, owner, info.privileges, acl).options;
	if (options == AclMode::NoRights && role.HeldModes(acl) == AclMode::NoRights) {
		ThrowPermissionDenied(type, name);
	}
	WarnUngranted(context, info, info.privileges, options, info.privileges == ObjectKindOf(type).privileges,
	              StringUtil::Format("\"%s\"", name));
}

static const Permissions &TargetPermissions(idx_t overload_oid, CatalogEntry &target) {
	if (overload_oid) {
		auto &macro = target.Cast<MacroCatalogEntry>();
		return macro.OverloadPermissions(*macro.FindOverload(overload_oid));
	}
	return target.permissions;
}

struct RelationColumn {
	reference<const Identifier> name;
	std::span<const AclItem> acl;
};

static void RequireGrantable(Binder &binder, const AlterPermissionsInfo &info, const ObjectReference &object,
                             const RoleAccess &role, CatalogEntry &entry) {
	auto &context = binder.context;
	auto &permissions = TargetPermissions(object.overload_oid, entry);
	const auto owner = permissions.owner;
	const bool checked = !role.MemberOf(owner);
	vector<AclItem> defaults;
	std::span<const AclItem> stored = permissions.acl;
	if (checked && permissions.UsesDefaultAcl()) {
		defaults = Permissions::AclDefault(ObjectKindOf(object.type).acl_class, owner);
		stored = defaults;
	}
	auto &name = entry.name.GetIdentifierName();
	if (checked && info.privileges != AclMode::NoRights) {
		const auto options = BestGrantor(info.grantors, owner, info.privileges, stored).options;
		if (options == AclMode::NoRights &&
		    !role.CanAny(object.type, permissions, ObjectKindOf(object.type).privileges)) {
			ThrowPermissionDenied(object.type, name);
		}
		WarnUngranted(context, info, info.privileges, options, info.privileges == ObjectKindOf(object.type).privileges,
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
		if (absl::c_none_of(columns,
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
		if (options == AclMode::NoRights && !role.CanAny(object.type, permissions, ACL_COLUMN_PRIVILEGES) &&
		    (role.HeldModes(column.acl) & ACL_COLUMN_PRIVILEGES) == AclMode::NoRights) {
			throw PermissionException("permission denied for column \"%s\" of relation \"%s\"", column_name, name);
		}
		WarnUngranted(context, info, requested, options, requested == ACL_COLUMN_PRIVILEGES,
		              StringUtil::Format("column \"%s\" of relation \"%s\"", column_name, name));
	}
}

static CatalogEntry &ResolvePermissionsTarget(Binder &binder, const AlterPermissionsInfo &info,
                                              ObjectReference &object) {
	auto &context = binder.context;
	auto &name = object.name;
	object.type = info.entry_catalog_type;
	if (object.type == CatalogType::SCHEMA_ENTRY) {
		auto &path = name.Path();
		auto &schema =
		    *binder.EntryRetriever().GetSchema(path.front(), vector<Identifier>(path.begin() + 1, path.end()),
		                                       AclMode::NoRights, OnEntryNotFound::THROW_EXCEPTION);
		object.name = QualifiedName::FromCatalogSchema(schema.ParentCatalog().GetName(), schema.GetParentSchemaPath(),
		                                               schema.name);
		return schema;
	}
	if (IsCatalogLevel(object.type)) {
		auto &entry = *binder.EntryRetriever().GetEntry(EntryLookupInfo(object.type, name));
		object.name = QualifiedName(entry.ParentCatalog().GetName(), Identifier(), entry.name);
		return entry;
	}
	optional_ptr<CatalogEntry> entry;
	if (object.type == CatalogType::MACRO_ENTRY) {
		auto routine = binder.BindRoutine(
		    {name, object.func_parameters, object.has_func_args, info.include_functions, info.include_procedures},
		    AclMode::NoRights, OnEntryNotFound::THROW_EXCEPTION);
		entry = routine.entry;
		if (routine.overload && !IsUnowned(*entry)) {
			object.overload_oid = entry->Cast<MacroCatalogEntry>().OverloadOid(*routine.overload);
		}
	} else {
		entry = binder.GetCatalogEntry(EntryLookupInfo(object.type, name), OnEntryNotFound::RETURN_NULL);
	}
	if (!entry) {
		throw CatalogException::MissingEntry(EntryLookupInfo(object.type, name), string());
	}
	if (object.type == CatalogType::VIEW_ENTRY && entry->type != CatalogType::VIEW_ENTRY) {
		throw CatalogException(Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx()),
		                       StringUtil::Format("\"%s\" is not a view", name.Name().GetIdentifierName()));
	}
	object.type = entry->type == CatalogType::VIEW_ENTRY ? CatalogType::TABLE_ENTRY : entry->type;
	object.name = QualifiedName::FromCatalogSchema(
	    entry->ParentCatalog().GetName(),
	    entry->ParentSchemaPath(entry->ParentCatalog().GetCatalogTransaction(context)), entry->name);
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
	auto granted_by = binder.BindRole(info.granted_by)->oid;
	if (!role.MemberOf(granted_by)) {
		throw PermissionException("must be member of role \"%s\"", binder.RoleName(granted_by));
	}
	return binder.AccessOf(granted_by);
}

static const RoleAccess &ResolveGrantors(Binder &binder, AlterPermissionsInfo &info) {
	auto &context = binder.context;
	for (auto &grantee : info.grantees) {
		info.grantee_ids.push_back(GranteeId(binder, grantee));
	}
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
	return role;
}

static void ResolveGrant(Binder &binder, const AlterPermissionsInfo &info, const RoleAccess &role,
                         ObjectReference &object) {
	auto &context = binder.context;
	if (info.entry_catalog_type == CatalogType::PARAMETER_ENTRY) {
		const auto setting = object.name.Name();
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
		object.name = QualifiedName(Identifier(StringUtil::Lower(name)));
		object.type = CatalogType::PARAMETER_ENTRY;
		return;
	}
	auto &target = ResolvePermissionsTarget(binder, info, object);
	RefuseSystemTablespace(target);
	if (target.internal) {
		throw PermissionException("Cannot alter entry \"%s\" because it is an internal system entry",
		                          target.name.GetIdentifierName());
	}
	RequireGrantable(binder, info, object, role, target);
}

static void ResolveOwner(Binder &binder, AlterPermissionsInfo &info, ObjectReference &object) {
	auto &context = binder.context;
	auto &entry = ResolvePermissionsTarget(binder, info, object);
	RefuseSystemTablespace(entry);
	auto &permissions = TargetPermissions(object.overload_oid, entry);
	info.new_owner_id = binder.BindRole(info.new_owner)->oid;
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
		        object.overload_oid ? entry.Cast<MacroCatalogEntry>().FindOverload(object.overload_oid).get()
		                            : nullptr);
	}
	if (!role.CanSet(info.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", binder.RoleName(info.new_owner_id));
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
	for (auto &grantee : info.grantees) {
		info.grantee_ids.push_back(GranteeId(binder, grantee));
	}
	const auto add_role = [&](idx_t role) {
		if (!binder.CheckAs().MemberOf(role)) {
			throw PermissionException("permission denied to change default privileges");
		}
		info.target_roles.push_back(role);
	};
	if (info.for_roles.empty()) {
		add_role(context.current_role_id);
	}
	for (auto &for_role : info.for_roles) {
		add_role(binder.BindRole(for_role)->oid);
	}
	const auto database = DatabaseManager::GetDefaultDatabase(context);
	if (info.default_schemas.empty()) {
		info.default_scopes.push_back(0);
	}
	for (auto &schema : info.default_schemas) {
		info.default_scopes.push_back(
		    binder.EntryRetriever()
		        .GetSchema(database, {Identifier(schema)}, AclMode::NoRights, OnEntryNotFound::THROW_EXCEPTION)
		        ->oid);
	}
	binder.EntryRetriever().GetEntry(EntryLookupInfo(CatalogType::DATABASE_ENTRY, QualifiedName(database)));
	info.SetQualifiedName(Identifier(), Identifier(), database);
}

static vector<idx_t> ResolveOwnedRoles(Binder &binder, const vector<RoleSpec> &names, const string &action) {
	vector<idx_t> result;
	for (auto &name : names) {
		auto role = binder.BindRole(name)->oid;
		if (!binder.CheckAs().MemberOf(role)) {
			throw PermissionException::WithDetail(
			    StringUtil::Format("Only roles with privileges of role \"%s\" may %s objects owned by it.",
			                       binder.RoleName(role), action),
			    "permission denied to %s objects", action);
		}
		result.push_back(role);
	}
	return result;
}

static void RefuseBootstrapRole(Binder &binder, const vector<idx_t> &roles, const string &action) {
	const auto bootstrap = DBConfig::GetConfig(binder.context).bootstrap_role;
	if (bootstrap != 0 && absl::c_linear_search(roles, bootstrap)) {
		throw DependencyException("cannot %s owned by role %s because they are required by the database system", action,
		                          binder.RoleName(bootstrap));
	}
}

void Binder::ResolveOwned(AlterOwnedInfo &info) {
	const bool drop = info.new_owner.empty();
	info.role_ids = ResolveOwnedRoles(*this, info.roles, drop ? "drop" : "reassign");
	if (drop) {
		info.grantors = GrantorRoles(context, CheckAs());
		RefuseBootstrapRole(*this, info.role_ids, "drop objects");
		return;
	}
	info.new_owner_id = BindRole(info.new_owner)->oid;
	if (!CheckAs().CanSet(info.new_owner_id)) {
		throw PermissionException("must be able to SET ROLE \"%s\"", RoleName(info.new_owner_id));
	}
	RefuseBootstrapRole(*this, info.role_ids, "reassign ownership of objects");
}

static void ExpandAllInSchema(Binder &binder, AlterPermissionsInfo &grant) {
	const auto kind = grant.entry_catalog_type;
	vector<ObjectReference> objects;
	for (auto &listed : grant.objects) {
		const auto name = Binder::ResolveCatalog(binder.EntryRetriever(), listed.name);
		auto &path = name.Path();
		auto &schema =
		    *binder.EntryRetriever().GetSchema(path.front(), vector<Identifier>(path.begin() + 1, path.end()),
		                                       AclMode::NoRights, OnEntryNotFound::THROW_EXCEPTION);
		const auto scan = [&](CatalogType type) {
			schema.Scan(binder.context, type, [&](CatalogEntry &entry) {
				if (entry.internal || !EntryVisible(binder.context, binder.CheckAs(), entry)) {
					return;
				}
				if (kind != CatalogType::MACRO_ENTRY) {
					if (entry.type == kind ||
					    (kind == CatalogType::TABLE_ENTRY && entry.type == CatalogType::VIEW_ENTRY)) {
						objects.push_back(ObjectReference {schema.GetQualifiedName(entry.name)});
					}
					return;
				}
				for (auto &overload : entry.Cast<MacroCatalogEntry>().macros) {
					if (overload->is_procedure ? grant.include_procedures : grant.include_functions) {
						objects.push_back(
						    ObjectReference {schema.GetQualifiedName(entry.name), overload->ParameterTypes(), true});
					}
				}
			});
		};
		scan(kind);
		if (kind == CatalogType::MACRO_ENTRY) {
			scan(CatalogType::TABLE_MACRO_ENTRY);
		}
	}
	grant.objects = std::move(objects);
	grant.all_in_schema = false;
}

void Binder::BindAlterPermissions(AlterPermissionsInfo &info, StatementProperties &properties) {
	if (info.default_objtype != CatalogType::INVALID) {
		ResolveDefaultPrivileges(*this, info);
		return;
	}
	if (info.all_in_schema) {
		ExpandAllInSchema(*this, info);
	}
	const auto catalog_type = info.entry_catalog_type;
	optional_ptr<const RoleAccess> granting;
	if (info.new_owner.empty()) {
		granting = ResolveGrantors(*this, info);
	}
	for (auto &object : info.objects) {
		if (catalog_type == CatalogType::SCHEMA_ENTRY) {
			object.name = ResolveCatalog(entry_retriever, object.name);
			properties.RegisterDBModify(Catalog::GetCatalog(context, object.name.Path().front()), context,
			                            DatabaseModificationType::ALTER_TABLE);
		} else if (catalog_type != CatalogType::DATABASE_ENTRY && catalog_type != CatalogType::ROLE_ENTRY &&
		           catalog_type != CatalogType::TABLESPACE_ENTRY &&
		           catalog_type != CatalogType::FOREIGN_DATA_WRAPPER_ENTRY &&
		           catalog_type != CatalogType::PARAMETER_ENTRY) {
			object.name = BindTableName(object.name);
			properties.RegisterDBModify(Catalog::GetCatalog(context, object.name.Catalog()), context,
			                            DatabaseModificationType::ALTER_TABLE);
		}
		if (granting) {
			ResolveGrant(*this, info, *granting, object);
		} else {
			ResolveOwner(*this, info, object);
		}
	}
}

} // namespace duckdb
