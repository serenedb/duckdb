#include "duckdb/catalog/permissions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/column_acls.hpp"
#include "duckdb/catalog/schema_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <absl/algorithm/container.h>

namespace duckdb {

const Permissions &PermissionsOf(const PrivilegeTarget &target) {
	static const Permissions UNOWNED {.acl = {{.grantee = ACL_ID_PUBLIC, .privs = ACL_GRANTABLE & ~AclMode::Create}}};
	auto &entry = target.entry;
	const bool unowned =
	    entry.permissions.owner == 0 || (!entry.internal && entry.ParentCatalog().GetAttached().IsReadOnly());
	const bool system_view = entry.type == CatalogType::VIEW_ENTRY && entry.ParentCatalog().IsSystemCatalog();
	if (unowned && !system_view) {
		return UNOWNED;
	}
	return target.permissions;
}

static optional_ptr<CatalogEntry> SystemEntry(ClientContext &context, CatalogType type, const Identifier &name) {
	auto &system = Catalog::GetSystemCatalog(context);
	return system.LookupEntry(system.GetCatalogTransaction(context), type, name);
}

static optional_ptr<CatalogEntry> ServerOf(ClientContext &context, AttachedDatabase &database) {
	const auto server_oid = database.ForeignServer();
	if (!server_oid.IsValid()) {
		return nullptr;
	}
	for (auto &db : DatabaseManager::Get(*context.db).GetDatabases()) {
		if (db->IsSystem() || db->IsTemporary() || db->IsReadOnly()) {
			continue;
		}
		auto &catalog = db->GetCatalog();
		auto server = catalog.LookupEntry(catalog.GetCatalogTransaction(context), CatalogType::FOREIGN_SERVER_ENTRY,
		                                  database.GetName());
		if (server && server->oid == server_oid.GetIndex()) {
			return server;
		}
	}
	return nullptr;
}

static bool CanConnect(ClientContext &context, const RoleAccess &role, const Identifier &name,
                       optional_ptr<CatalogEntry> database) {
	return !database || name.GetIdentifierName() == ClientData::Get(context).default_database.GetIdentifierName() ||
	       role.Can(CatalogType::DATABASE_ENTRY, database->permissions, AclMode::Connect);
}

bool DatabaseVisible(ClientContext &context, const RoleAccess &role, AttachedDatabase &database) {
	if (role.is_superuser || database.IsSystem() || database.IsTemporary()) {
		return true;
	}
	if (!database.IsReadOnly()) {
		if (auto entry = SystemEntry(context, CatalogType::DATABASE_ENTRY, database.GetName())) {
			return CanConnect(context, role, database.GetName(), entry);
		}
	}
	auto server = ServerOf(context, database);
	return server && role.Can(CatalogType::FOREIGN_SERVER_ENTRY, server->permissions, AclMode::Usage);
}

static bool HoldsAny(const RoleAccess &role, const ObjectKind &kind, const Permissions &permissions) {
	if (role.MemberOf(permissions.owner)) {
		return true;
	}
	if (permissions.UsesDefaultAcl() && role.capabilities == RoleCapability::None) {
		return (kind.public_privileges & kind.privileges) != AclMode::NoRights;
	}
	return role.CanAny(kind.acl_class, permissions, kind.privileges);
}

static optional_ptr<CatalogEntry> ContainingSchema(ClientContext &context, CatalogEntry &entry) {
	if (!entry.ParentSchemaOid()) {
		return nullptr;
	}
	if (entry.type != CatalogType::SCHEMA_ENTRY) {
		return entry.ParentSchema(context);
	}
	auto &parent = *entry.Cast<SchemaCatalogEntry>().GetSchemaInfo()->parent;
	return parent.versions->GetVisible(entry.ParentCatalog().GetCatalogTransaction(context).view);
}

bool EntryVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry) {
	if (role.is_superuser) {
		return true;
	}
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		return CanConnect(context, role, entry.name, SystemEntry(context, CatalogType::DATABASE_ENTRY, entry.name));
	}
	if (entry.internal || PermissionsOf(entry).owner == 0) {
		return true;
	}
	if (auto schema = ContainingSchema(context, entry)) {
		if (!EntryVisible(context, role, *schema)) {
			return false;
		}
		if (role.MemberOf(schema->permissions.owner)) {
			return true;
		}
	}
	const auto kind = ObjectKindOf(entry.type);
	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::JOB_ENTRY:
	case CatalogType::FOREIGN_SERVER_ENTRY:
		return HoldsAny(role, kind, entry.permissions);
	case CatalogType::SUBSCRIPTION_ENTRY:
		return role.MemberOf(entry.permissions.owner);
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return HoldsAny(role, kind, entry.permissions) ||
		       AnyColumnAcl(entry.Cast<StandardEntry>(), [&](std::span<const AclItem> acl) {
			       return !acl.empty() && (role.HeldModes(acl) & ACL_COLUMN_PRIVILEGES) != AclMode::NoRights;
		       });
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		return absl::c_any_of(macro.macros, [&](const unique_ptr<MacroFunction> &overload) {
			return HoldsAny(role, kind, macro.OverloadPermissions(*overload));
		});
	}
	case CatalogType::INDEX_ENTRY: {
		auto &index = entry.Cast<IndexCatalogEntry>();
		auto relation = index.GetRelation(index.ParentCatalog().GetCatalogTransaction(context));
		return !relation || EntryVisible(context, role, *relation);
	}
	case CatalogType::TRIGGER_ENTRY: {
		auto &trigger = entry.Cast<TriggerCatalogEntry>();
		auto transaction = trigger.ParentCatalog().GetCatalogTransaction(context);
		auto relation = trigger.ParentSchema(transaction)
		                    .GetEntry(transaction, CatalogType::TABLE_ENTRY, trigger.base_table->Table());
		return relation && EntryVisible(context, role, *relation);
	}
	default:
		return true;
	}
}

static string ObjectNounOf(CatalogType type) {
	const auto noun = ObjectKindOf(type).noun;
	return noun.empty() ? StringUtil::Lower(CatalogTypeToString(type)) : string(noun);
}

static bool IsFunction(const CatalogEntry &entry) {
	switch (entry.type) {
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
	case CatalogType::WINDOW_FUNCTION_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		return true;
	default:
		return false;
	}
}

static bool Uses(AclMode use, AclMode modes) {
	return (use & modes) != AclMode::NoRights;
}

bool PrivilegeCheck::Holds(const RoleAccess &role, const PrivilegeTarget &target, AclMode use) {
	auto &entry = target.entry;
	const auto modes = use & ACL_GRANTABLE;
	const bool any = Uses(use, AclMode::Any);
	if (!target.of_columns) {
		auto &permissions = PermissionsOf(target);
		if (Uses(use, AclMode::Execute) && IsFunction(entry) && permissions.owner == 0) {
			const auto capability = entry.Cast<FunctionEntry>().required_capability;
			return capability == RoleCapability::None || role.Has(capability) || role.owns_builtins;
		}
		if (modes == AclMode::NoRights) {
			return true;
		}
		const auto type = ObjectKindOf(entry.type).acl_class;
		return any ? role.CanAny(type, permissions, modes) : role.Can(type, permissions, modes);
	}
	auto &relation = entry.Cast<StandardEntry>();
	const auto granted = [&](std::span<const AclItem> acl) {
		return any ? (role.HeldModes(acl) & modes) != AclMode::NoRights
		           : role.Can(CatalogType::TABLE_ENTRY, acl, modes);
	};
	if (target.columns.empty()) {
		return AnyColumnAcl(relation, granted);
	}
	return absl::c_all_of(target.columns, [&](LogicalIndex column) { return granted(ColumnAclOf(relation, column)); });
}

const RoleAccess &Role(ExpressionState &state) {
	return Role(state.GetContext(), state.expr.Cast<BoundFunctionExpression>().BoundRole());
}

PrivilegeCheck PrivilegeCheck::Ownership(const RoleAccess &role, const PrivilegeTarget &target) {
	auto &entry = target.entry;
	if (entry.type != CatalogType::ROLE_ENTRY) {
		return role.MemberOf(target.permissions.owner) ? PrivilegeCheck() : PrivilegeCheck(Denial::NotOwner, entry);
	}
	if (entry.Cast<RoleEntry>().IsSuperuser()) {
		return {Denial::SuperuserRole, entry};
	}
	if (!role.Has(RoleOption::CreateRole)) {
		return {Denial::NoCreateRole, entry};
	}
	return role.AdminOf(entry.oid) ? PrivilegeCheck() : PrivilegeCheck(Denial::NoAdminOption, entry);
}

PrivilegeCheck PrivilegeCheck::CreateIn(ClientContext &context, const RoleAccess &role, const CatalogEntry &container,
                                        Denial denied) {
	if (!container.ParentCatalog().GetAttached().IsTemporary()) {
		return Holds(role, container, AclMode::Create) ? PrivilegeCheck() : PrivilegeCheck(denied, container);
	}
	auto database = SystemEntry(context, CatalogType::DATABASE_ENTRY, ClientData::Get(context).default_database);
	if (!database || Holds(role, *database, AclMode::CreateTemp)) {
		return {};
	}
	return {Denial::NoTemporaryTables, *database};
}

PrivilegeCheck PrivilegeCheck::Rules(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target,
                                     AclMode use) {
	auto &entry = target.entry;
	const auto container = ObjectKindOf(entry.type).container;
	if (Uses(use, AclMode::SetRole) && !role.CanSet(entry.oid)) {
		return {Denial::NoSetRole, entry};
	}
	if (Uses(use, AclMode::Own | AclMode::Rename)) {
		if (auto check = Ownership(role, target); !check) {
			return check;
		}
	}
	if (Uses(use, AclMode::Rename) && container != ObjectContainer::CLUSTER) {
		auto parent = container == ObjectContainer::DATABASE
		                  ? SystemEntry(context, CatalogType::DATABASE_ENTRY, entry.ParentCatalog().GetName())
		                  : optional_ptr<CatalogEntry>(entry.ParentSchema(context));
		if (parent) {
			if (auto check = CreateIn(context, role, *parent, Denial::ParentPrivilege); !check) {
				return check;
			}
		}
	}
	if (Uses(use, AclMode::Drop) && (container == ObjectContainer::DATABASE || container == ObjectContainer::CLUSTER ||
	                                 !role.MemberOf(entry.ParentSchema(context).permissions.owner))) {
		if (auto check = Ownership(role, target); !check) {
			return check;
		}
	}
	if (entry.type == CatalogType::SCHEMA_ENTRY && Uses(use, AclMode::Create)) {
		if (auto check = CreateIn(context, role, entry, Denial::Privilege); !check) {
			return check;
		}
		use = use & ~AclMode::Create;
	}
	if (Holds(role, target, use & ~(AclMode::Own | AclMode::Drop | AclMode::Rename | AclMode::SetRole))) {
		return {};
	}
	return {Denial::Privilege, entry};
}

void PrivilegeCheck::Throw(optional_ptr<const StatementNaming> naming) const {
	auto &entry = *object;
	const auto named = [&]() {
		if (!naming) {
			return StringUtil::Format("%s %s", ObjectNounOf(entry.type), entry.name.GetIdentifierName());
		}
		return StringUtil::Format(
		    "%s %s", naming->noun,
		    StringUtil::Join(naming->name, ".", [](const Identifier &part) { return part.GetIdentifierName(); }));
	};
	switch (denial) {
	case Denial::Privilege:
		throw PermissionException("permission denied for %s", named());
	case Denial::NotOwner:
		throw PermissionException("must be owner of %s", named());
	case Denial::NoSetRole:
		throw PermissionException("must be able to SET ROLE \"%s\"", entry.name.GetIdentifierName());
	case Denial::SuperuserRole:
		throw PermissionException::WithDetail("The current user must have the SUPERUSER attribute.",
		                                      "permission denied");
	case Denial::NoCreateRole:
		throw PermissionException::WithDetail("The current user must have the CREATEROLE attribute.",
		                                      "permission denied");
	case Denial::NoAdminOption:
		throw PermissionException::WithDetail(
		    StringUtil::Format("The current user must have the ADMIN option on role \"%s\".",
		                       entry.name.GetIdentifierName()),
		    "permission denied");
	case Denial::NoTemporaryTables:
		throw PermissionException("permission denied to create temporary tables in database \"%s\"",
		                          entry.name.GetIdentifierName());
	case Denial::ParentPrivilege:
		throw PermissionException("permission denied for %s %s", ObjectNounOf(entry.type),
		                          entry.name.GetIdentifierName());
	case Denial::None:
		break;
	}
	throw InternalException("a privilege check that allows was asked to throw");
}

} // namespace duckdb
