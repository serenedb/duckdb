#include "duckdb/catalog/permissions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
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
	if (!target.entry) {
		return target.permissions;
	}
	auto &entry = *target.entry;
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

static optional_ptr<CatalogEntry> ContainingSchema(ClientContext &context, const CatalogEntry &entry) {
	if (!entry.ParentSchemaOid()) {
		return nullptr;
	}
	if (entry.type != CatalogType::SCHEMA_ENTRY) {
		return entry.ParentSchema(context);
	}
	auto &parent = *entry.Cast<SchemaCatalogEntry>().GetSchemaInfo()->parent;
	auto &catalog = Catalog::GetCatalog(context, entry.ParentCatalog().GetName());
	return parent.versions->GetVisible(catalog.GetCatalogTransaction(context).view);
}

bool ForEachSeer(const CatalogEntry &entry, absl::FunctionRef<bool(idx_t)> visit) {
	if (entry.internal || PermissionsOf(entry).owner == 0) {
		return visit(ACL_ID_PUBLIC);
	}
	const auto &kind = ObjectKindOf(entry.type);
	const auto holders = [&](const Permissions &permissions) {
		if (visit(permissions.owner)) {
			return true;
		}
		if (permissions.UsesDefaultAcl()) {
			return (kind.public_privileges & kind.privileges) != AclMode::NoRights && visit(ACL_ID_PUBLIC);
		}
		return absl::c_any_of(permissions.acl, [&](const AclItem &item) {
			return (item.privs & kind.privileges) != AclMode::NoRights && visit(item.grantee);
		});
	};
	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::JOB_ENTRY:
	case CatalogType::FOREIGN_SERVER_ENTRY:
		return holders(entry.permissions);
	case CatalogType::SUBSCRIPTION_ENTRY:
		return visit(entry.permissions.owner);
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return holders(entry.permissions) ||
		       AnyColumnAcl(entry.Cast<StandardEntry>(), [&](std::span<const AclItem> acl) {
			       return absl::c_any_of(acl, [&](const AclItem &item) {
				       return (item.privs & ACL_COLUMN_PRIVILEGES) != AclMode::NoRights && visit(item.grantee);
			       });
		       });
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		return absl::c_any_of(macro.macros, [&](const unique_ptr<MacroFunction> &overload) {
			return holders(macro.OverloadPermissions(*overload));
		});
	}
	default:
		return visit(ACL_ID_PUBLIC);
	}
}

SchemaView ViewOf(ClientContext &context, const RoleAccess &role, const CatalogEntry &schema) {
	const bool shown = static_cast<bool>(Check<AclMode::See>(context, role, schema));
	return {shown, shown && role.MemberOf(schema.permissions.owner)};
}

bool PrivilegeCheck::Sees(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target) {
	auto &entry = *target.entry;
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		return entry.name.GetIdentifierName() == ClientData::Get(context).default_database.GetIdentifierName() ||
		       Holds(role, target, AclMode::Connect);
	}
	if (entry.internal || PermissionsOf(target).owner == 0) {
		return true;
	}
	optional_ptr<CatalogEntry> schema;
	if (!target.schema) {
		schema = ContainingSchema(context, entry);
	}
	if (target.schema || schema) {
		const auto view = target.schema ? *target.schema : ViewOf(context, role, *schema);
		if (!view.shown) {
			return false;
		}
		if (view.owned) {
			return true;
		}
	}
	const auto relation_seen = [&](CatalogEntry &relation) {
		return target.schema ? Sees(context, role, PrivilegeTarget::In(relation, *target.schema))
		                     : Sees(context, role, relation);
	};
	switch (entry.type) {
	case CatalogType::INDEX_ENTRY: {
		auto &schema = entry.ParentSchema(context);
		auto relation =
		    entry.Cast<IndexCatalogEntry>().GetRelation(schema.ParentCatalog().GetCatalogTransaction(context));
		return !relation || relation_seen(*relation);
	}
	case CatalogType::TRIGGER_ENTRY: {
		auto &schema = entry.ParentSchema(context);
		auto relation = schema.GetEntry(schema.ParentCatalog().GetCatalogTransaction(context), CatalogType::TABLE_ENTRY,
		                                entry.Cast<TriggerCatalogEntry>().base_table->Table());
		return relation && relation_seen(*relation);
	}
	default: {
		const auto &kind = ObjectKindOf(entry.type);
		return (role.PredefinedRoleModes(kind.acl_class) & kind.privileges) != AclMode::NoRights ||
		       ForEachSeer(entry, [&](idx_t seer) { return seer == ACL_ID_PUBLIC || role.Uses(seer); });
	}
	}
}

bool CatalogEntryRetriever::Shows(CatalogEntry &entry) const {
	return static_cast<bool>(Check<AclMode::See>(context, LookupRole(), entry));
}

bool CatalogEntryRetriever::Shows(CatalogEntry &entry, const SchemaView &schema) const {
	return static_cast<bool>(Check<AclMode::See>(context, LookupRole(), PrivilegeTarget::In(entry, schema)));
}

SchemaView CatalogEntryRetriever::View(const SchemaCatalogEntry &schema) const {
	return ViewOf(context, LookupRole(), schema);
}

bool CatalogEntryRetriever::Hides(const EntryLookupInfo &lookup) const {
	auto entry = Internal(context).GetEntry(lookup, OnEntryNotFound::RETURN_NULL);
	return entry && !Shows(*entry);
}

bool CatalogEntryRetriever::Hides(const Identifier &catalog, const vector<Identifier> &schema_path) const {
	auto schema = Internal(context).GetSchema(catalog, schema_path, AclMode::NoRights, OnEntryNotFound::RETURN_NULL);
	return schema && !Shows(*schema);
}

void CatalogEntryRetriever::VisitSearchSchemas(
    absl::FunctionRef<void(const Identifier &name, SchemaCatalogEntry &schema)> visit) {
	auto &role = LookupRole();
	const auto database = DatabaseManager::TryGetDefaultDatabase(context);
	for (auto &entry : GetSearchPath().Get()) {
		if (!entry.GetCatalog().empty() && entry.GetCatalog() != database) {
			continue;
		}
		auto schema = Catalog::GetSchema(context, entry.GetCatalog(), entry.GetSchema(), OnEntryNotFound::RETURN_NULL);
		if (schema && Check<AclMode::Usage>(context, role, *schema)) {
			visit(entry.GetSchema(), *schema);
		}
	}
}

bool CatalogEntryRetriever::Shows(AttachedDatabase &database) const {
	auto &role = LookupRole();
	if (role.is_superuser || database.IsSystem() || database.IsTemporary()) {
		return true;
	}
	if (!database.IsReadOnly()) {
		if (auto entry = SystemEntry(context, CatalogType::DATABASE_ENTRY, database.GetName())) {
			return static_cast<bool>(Check<AclMode::See>(context, role, *entry));
		}
	}
	auto server = ServerOf(context, database);
	return server && Check<AclMode::Usage>(context, role, *server);
}

static string ObjectNounOf(CatalogType type) {
	const auto noun = ObjectKindOf(type).noun;
	return noun.empty() ? StringUtil::Lower(CatalogTypeToString(type)) : string(noun);
}

static bool Uses(AclMode use, AclMode modes) {
	return (use & modes) != AclMode::NoRights;
}

static bool HoldsRequiredRole(const RoleAccess &role, RoleCapability required) {
	return role.owns_builtins || (required != RoleCapability::Superuser && role.Uses(required));
}

template <class F>
static bool OnColumns(const PrivilegeTarget &target, F &&granted) {
	if (!target.entry) {
		auto &list = *target.column_list;
		if (target.columns.empty()) {
			return absl::c_any_of(list.Logical(),
			                      [&](const ColumnDefinition &column) { return granted(column.Acl()); });
		}
		return absl::c_all_of(target.columns,
		                      [&](LogicalIndex column) { return granted(list.GetColumn(column).Acl()); });
	}
	auto &relation = target.entry->Cast<StandardEntry>();
	if (target.columns.empty()) {
		return AnyColumnAcl(relation, granted);
	}
	return absl::c_all_of(target.columns, [&](LogicalIndex column) { return granted(ColumnAclOf(relation, column)); });
}

static bool HoldsRole(const RoleAccess &role, idx_t other, AclMode use) {
	const auto wanted = use & (AclMode::Usage | AclMode::Member | AclMode::Admin);
	auto held = AclMode::NoRights;
	if (Uses(wanted, AclMode::Usage) && role.MemberOf(other)) {
		held |= AclMode::Usage;
	}
	if (Uses(wanted, AclMode::Member) && role.BelongsTo(other)) {
		held |= AclMode::Member;
	}
	if (Uses(wanted, AclMode::Admin) && role.AdminOf(other)) {
		held |= AclMode::Admin;
	}
	return Uses(use, AclMode::Any) && wanted != AclMode::NoRights ? held != AclMode::NoRights : held == wanted;
}

bool PrivilegeCheck::Holds(const RoleAccess &role, const PrivilegeTarget &target, AclMode use) {
	if (target.type == CatalogType::ROLE_ENTRY) {
		return HoldsRole(role, target.oid, use);
	}
	const auto modes = use & ACL_GRANTABLE;
	const bool any = Uses(use, AclMode::Any);
	if (Uses(use, AclMode::GrantOption)) {
		auto &permissions = PermissionsOf(target);
		if (role.MemberOf(permissions.owner)) {
			return true;
		}
		const auto grantable = [&](std::span<const AclItem> acl) {
			const auto held = role.GrantableModes(acl) & modes;
			return any ? held != AclMode::NoRights : held == modes;
		};
		return target.of_columns ? OnColumns(target, grantable) : grantable(permissions.acl);
	}
	if (target.of_columns) {
		return OnColumns(target, [&](std::span<const AclItem> acl) {
			return any ? (role.HeldModes(acl) & modes) != AclMode::NoRights
			           : role.Can(CatalogType::TABLE_ENTRY, acl, modes);
		});
	}
	auto &permissions = PermissionsOf(target);
	if (Uses(use, AclMode::Execute) && permissions.owner == 0 && target.entry && ObjectKindOf(target.type).routine) {
		const auto required = target.entry->Cast<FunctionEntry>().required_capability;
		return required == RoleCapability::None || HoldsRequiredRole(role, required);
	}
	if (modes == AclMode::NoRights ||
	    (target.capability != RoleCapability::None && HoldsRequiredRole(role, target.capability))) {
		return true;
	}
	const auto type = ObjectKindOf(target.type).acl_class;
	return any ? role.CanAny(type, permissions, modes) : role.Can(type, permissions, modes);
}

const RoleAccess &Role(ExpressionState &state) {
	return Role(state.GetContext(), state.expr.Cast<BoundFunctionExpression>().BoundRole());
}

PrivilegeCheck PrivilegeCheck::Ownership(const RoleAccess &role, const PrivilegeTarget &target) {
	if (target.type != CatalogType::ROLE_ENTRY) {
		return role.MemberOf(target.permissions.owner) ? PrivilegeCheck()
		                                               : PrivilegeCheck(Denial::NotOwner, target.entry);
	}
	auto &entry = *target.entry;
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
	if (Uses(use, AclMode::See)) {
		return Sees(context, role, target) ? PrivilegeCheck() : PrivilegeCheck(Denial::Hidden, target.entry);
	}
	const auto container = ObjectKindOf(target.type).container;
	if (Uses(use, AclMode::SetRole) && !role.CanSet(target.oid)) {
		return {Denial::NoSetRole, target.entry};
	}
	if (Uses(use, AclMode::Own | AclMode::Rename)) {
		if (auto check = Ownership(role, target); !check) {
			return check;
		}
	}
	if (Uses(use, AclMode::Rename) && container != ObjectContainer::CLUSTER) {
		auto &entry = *target.entry;
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
	                                 !role.MemberOf(target.entry->ParentSchema(context).permissions.owner))) {
		if (auto check = Ownership(role, target); !check) {
			return check;
		}
	}
	if (target.entry && target.type == CatalogType::SCHEMA_ENTRY && Uses(use, AclMode::Create) &&
	    !Uses(use, AclMode::GrantOption)) {
		auto check = CreateIn(context, role, *target.entry, Denial::Privilege);
		use = use & ~AclMode::Create;
		const bool others = Uses(use, ACL_GRANTABLE);
		if (Uses(use, AclMode::Any) ? check || !others : !check || !others) {
			return check;
		}
	}
	if (Holds(role, target, use & ~(AclMode::Own | AclMode::Drop | AclMode::Rename | AclMode::SetRole))) {
		return {};
	}
	return {Denial::Privilege, target.entry};
}

struct StatementRequirement {
	RoleOption option = RoleOption::None;
	RoleCapability capability = RoleCapability::None;
	const char *message = "permission denied: superuser required";
	bool named = false;
	const char *detail = nullptr;
	const char *hint = nullptr;
};

static constexpr StatementRequirement STATEMENT_RULES[] = {
    {},
    {},
    {},
    {},
    {},
    {},
    {},
    {},
    {},
    {},
    {.message = "permission denied to create tablespace \"%s\"",
     .named = true,
     .hint = "Must be superuser to create a tablespace."},
    {.option = RoleOption::CreateDb, .message = "permission denied to create database"},
    {.option = RoleOption::CreateDb, .message = "permission denied to change owner of database"},
    {.capability = RoleCapability::CreateSubscription,
     .message = "permission denied to create subscription",
     .detail = "Only roles with privileges of the \"pg_create_subscription\" role may create subscriptions."},
    {.option = RoleOption::CreateRole,
     .message = "permission denied to create role",
     .detail = "Only roles with the CREATEROLE attribute may create roles."},
    {.option = RoleOption::Superuser,
     .message = "permission denied to create role",
     .detail = "Only roles with the SUPERUSER attribute may create roles with the SUPERUSER attribute."},
    {.option = RoleOption::CreateDb,
     .message = "permission denied to create role",
     .detail = "Only roles with the CREATEDB attribute may create roles with the CREATEDB attribute."},
    {.option = RoleOption::Replication,
     .message = "permission denied to create role",
     .detail = "Only roles with the REPLICATION attribute may create roles with the REPLICATION attribute."},
    {.option = RoleOption::BypassRls,
     .message = "permission denied to create role",
     .detail = "Only roles with the BYPASSRLS attribute may create roles with the BYPASSRLS attribute."},
    {.option = RoleOption::CreateRole,
     .message = "permission denied to drop role",
     .detail = "Only roles with the CREATEROLE attribute and the ADMIN option on the target roles may drop roles."},
    {.message = "permission denied to set session authorization \"%s\"", .named = true},
};

static_assert(std::size(STATEMENT_RULES) == static_cast<idx_t>(StatementRule::SetSessionAuthorization) + 1);

PrivilegeCheck PrivilegeCheck::Runs(const RoleAccess &role, StatementRule rule) {
	auto &requirement = STATEMENT_RULES[static_cast<idx_t>(rule)];
	if ((requirement.option != RoleOption::None && role.Has(requirement.option)) ||
	    (requirement.capability != RoleCapability::None && role.Uses(requirement.capability))) {
		return {};
	}
	PrivilegeCheck check(Denial::Statement, nullptr);
	check.rule = rule;
	return check;
}

void PrivilegeCheck::Throw(optional_ptr<const StatementNaming> naming) const {
	if (denial == Denial::Statement) {
		auto &statement = STATEMENT_RULES[static_cast<idx_t>(rule)];
		const auto message = statement.named
		                         ? StringUtil::Format(statement.message, naming->name.back().GetIdentifierName())
		                         : string(statement.message);
		if (statement.detail) {
			throw PermissionException::WithDetail(statement.detail, message);
		}
		if (statement.hint) {
			throw PermissionException::WithHint(statement.hint, message);
		}
		throw PermissionException(message);
	}
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
	case Denial::Hidden:
		throw InternalException("a visibility check was asked to throw");
	case Denial::Statement:
	case Denial::None:
		break;
	}
	throw InternalException("a privilege check that allows was asked to throw");
}

} // namespace duckdb
