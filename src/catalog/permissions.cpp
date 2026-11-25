#include "duckdb/catalog/permissions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/database_entry.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/column_acls.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/catalog/schema_info.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parsed_data/create_subscription_info.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include <algorithm>

namespace duckdb {

bool IsUnowned(const CatalogEntry &entry) {
	auto &catalog = entry.ParentCatalog();
	if (!entry.internal && catalog.GetAttached().IsReadOnly()) {
		return true;
	}
	return entry.permissions.owner == 0 && (entry.internal || catalog.Compatibility() != SqlCompatibility::POSTGRES);
}

bool CanUseSchema(const RoleAccess &role, const CatalogEntry &schema) {
	return role.is_superuser || schema.internal || IsUnowned(schema) ||
	       schema.ParentCatalog().Compatibility() != SqlCompatibility::POSTGRES ||
	       role.Can(CatalogType::SCHEMA_ENTRY, schema.permissions, AclMode::Usage);
}

static bool MayCall(const RoleAccess &role, RoleCapability capability) {
	return capability == RoleCapability::None || role.Has(capability) || role.owns_builtins;
}

static optional_ptr<Catalog> RoleCatalog(ClientContext &context) {
	auto &system = Catalog::GetSystemCatalog(context);
	if (system.IsDuckCatalog()) {
		return nullptr;
	}
	return &system;
}

static optional_ptr<CatalogEntry> ClusterDatabaseOf(ClientContext &context, AttachedDatabase &database) {
	return database.IsReadOnly() ? nullptr : ClusterEntry(context, CatalogType::DATABASE_ENTRY, database.GetName());
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
		auto server = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context),
		                                          CatalogType::FOREIGN_SERVER_ENTRY, database.GetName());
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
	if (database.IsSystem() || database.IsTemporary()) {
		return true;
	}
	if (auto entry = ClusterDatabaseOf(context, database)) {
		return CanConnect(context, role, database.GetName(), entry);
	}
	auto server = ServerOf(context, database);
	return server && role.Can(CatalogType::FOREIGN_SERVER_ENTRY, server->permissions, AclMode::Usage);
}

static bool HoldsAny(const RoleAccess &role, const ObjectKind &kind, const Permissions &permissions) {
	if (role.Owns(permissions.owner)) {
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
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		return CanConnect(context, role, entry.name, ClusterEntry(context, CatalogType::DATABASE_ENTRY, entry.name));
	}
	if ((entry.type == CatalogType::TABLE_FUNCTION_ENTRY || entry.type == CatalogType::SCALAR_FUNCTION_ENTRY) &&
	    !Allowed(role, entry, AclMode::Execute)) {
		return false;
	}
	if (entry.internal || IsUnowned(entry)) {
		return true;
	}
	if (auto schema = ContainingSchema(context, entry)) {
		if (!EntryVisible(context, role, *schema)) {
			return false;
		}
		if (role.Owns(schema->permissions.owner)) {
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
		return role.Owns(entry.permissions.owner);
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return HoldsAny(role, kind, entry.permissions) ||
		       AnyColumnAcl(entry.Cast<StandardEntry>(), [&](std::span<const AclItem> acl) {
			       return !acl.empty() && (role.HeldModes(acl) & ACL_COLUMN_PRIVILEGES) != AclMode::NoRights;
		       });
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		return std::any_of(macro.macros.begin(), macro.macros.end(), [&](const unique_ptr<MacroFunction> &overload) {
			auto &permissions = macro.OverloadPermissions(*overload);
			return HoldsAny(role, kind, permissions);
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

bool LookupVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry) {
	return role.is_superuser || entry.type == CatalogType::TABLE_FUNCTION_ENTRY ||
	       entry.type == CatalogType::SCALAR_FUNCTION_ENTRY || EntryVisible(context, role, entry);
}

static constexpr uint64_t PUBLIC_BIT = uint64_t(1) << 63;

static uint64_t RoleBit(idx_t role) {
	return role == ACL_ID_PUBLIC ? PUBLIC_BIT : uint64_t(1) << (Hash(uint64_t(role)) % 63);
}

static uint64_t GranteeBits(std::span<const AclItem> acl, AclMode privileges) {
	uint64_t bits = 0;
	for (auto &item : acl) {
		if ((item.privs & privileges) != AclMode::NoRights) {
			bits |= RoleBit(item.grantee);
		}
	}
	return bits;
}

static uint64_t HolderBits(const ObjectKind &kind, const Permissions &permissions) {
	if (permissions.owner == 0) {
		return VISIBLE_TO_ALL;
	}
	auto bits = RoleBit(permissions.owner);
	if (!permissions.UsesDefaultAcl()) {
		return bits | GranteeBits(permissions.acl, kind.privileges);
	}
	return (kind.public_privileges & kind.privileges) != AclMode::NoRights ? bits | PUBLIC_BIT : bits;
}

static uint64_t OwnBits(const CatalogEntry &entry) {
	if (entry.internal) {
		return VISIBLE_TO_ALL;
	}
	const auto kind = ObjectKindOf(entry.type);
	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::JOB_ENTRY:
		return HolderBits(kind, entry.permissions);
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY: {
		auto bits = HolderBits(kind, entry.permissions);
		AnyColumnAcl(entry.Cast<StandardEntry>(), [&](std::span<const AclItem> acl) {
			bits |= GranteeBits(acl, ACL_COLUMN_PRIVILEGES);
			return false;
		});
		return bits;
	}
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		uint64_t bits = 0;
		for (auto &overload : macro.macros) {
			bits |= HolderBits(kind, macro.OverloadPermissions(*overload));
		}
		return bits;
	}
	default:
		return VISIBLE_TO_ALL;
	}
}

uint64_t VisibilityBits(const CatalogEntry &entry) {
	const auto bits = OwnBits(entry);
	if (bits == VISIBLE_TO_ALL || !entry.ParentSchemaOid()) {
		return bits;
	}
	auto &schema_info = entry.type == CatalogType::SCHEMA_ENTRY
	                        ? *entry.Cast<SchemaCatalogEntry>().GetSchemaInfo()->parent
	                        : *entry.Cast<StandardEntry>().schema_info;
	auto schema = schema_info.versions->GetCommitted();
	return schema ? bits | RoleBit(schema->permissions.owner) : bits;
}

uint64_t ViewerBits(const RoleAccess &role) {
	if (role.is_superuser || role.capabilities != RoleCapability::None) {
		return VISIBLE_TO_ALL;
	}
	auto bits = PUBLIC_BIT | RoleBit(role.role);
	for (auto member : role.closure) {
		bits |= RoleBit(member);
	}
	return bits;
}

void CheckReference(ClientContext &context, const RoleAccess &role, CatalogEntry &entry) {
	auto &database = entry.type == CatalogType::DATABASE_ENTRY ? entry.Cast<AttachedDatabase>()
	                                                           : entry.ParentCatalog().GetAttached();
	if (!DatabaseVisible(context, role, database)) {
		auto &name = database.GetName().GetIdentifierName();
		if (ClusterDatabaseOf(context, database)) {
			throw PermissionException({{"detail", "User does not have CONNECT privilege."}},
			                          StringUtil::Format("permission denied for database \"%s\"", name));
		}
		if (auto server = ServerOf(context, database)) {
			ThrowPermissionDenied(*server);
		}
		throw PermissionException(
		    {{"detail",
		      "Only roles with the SUPERUSER attribute may use a database attached without a foreign server."}},
		    StringUtil::Format("permission denied for database \"%s\"", name));
	}
	if (entry.type != CatalogType::DATABASE_ENTRY && !EntryVisible(context, role, entry)) {
		ThrowPermissionDenied(entry);
	}
}

template <class F>
static bool AnyRoleReference(const CatalogEntry &entry, F &&named) {
	auto &permissions = entry.permissions;
	const auto any_acl = [&](std::span<const AclItem> acl) {
		return std::any_of(acl.begin(), acl.end(),
		                   [&](const AclItem &item) { return named(item.grantee) || named(item.grantor); });
	};
	if (entry.type == CatalogType::MACRO_ENTRY || entry.type == CatalogType::TABLE_MACRO_ENTRY) {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		return std::any_of(macro.macros.begin(), macro.macros.end(), [&](const unique_ptr<MacroFunction> &overload) {
			auto &overload_permissions = macro.OverloadPermissions(*overload);
			return named(overload_permissions.owner) || any_acl(overload_permissions.acl);
		});
	}
	if (named(permissions.owner) || any_acl(permissions.acl)) {
		return true;
	}
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		auto &defaults = entry.Cast<DatabaseEntry>().DefaultAcls();
		return std::any_of(defaults.begin(), defaults.end(),
		                   [&](const DefaultAcl &row) { return named(row.role) || any_acl(row.acl); });
	}
	if (entry.type == CatalogType::ROLE_ENTRY) {
		auto &member_of = entry.Cast<RoleEntry>().MemberOf();
		return std::any_of(member_of.begin(), member_of.end(),
		                   [&](const Membership &edge) { return named(edge.grantor); });
	}
	return (entry.type == CatalogType::VIEW_ENTRY || entry.type == CatalogType::TABLE_ENTRY) &&
	       AnyColumnAcl(entry.Cast<StandardEntry>(), any_acl);
}

bool ReferencesRole(const CatalogEntry &entry, idx_t role) {
	return AnyRoleReference(entry, [&](idx_t oid) { return oid == role; });
}

void ForEachRoleReference(const CatalogEntry &entry, const std::function<void(idx_t role)> &callback) {
	AnyRoleReference(entry, [&](idx_t oid) {
		if (oid != ACL_ID_PUBLIC) {
			callback(oid);
		}
		return false;
	});
}

string RoleOptionsToSQL(RoleOption set_options, RoleOption clear_options) {
	string result;
	for (auto &named : ROLE_OPTIONS) {
		if (HasOption(set_options, named.option)) {
			result += " ";
			result += named.keyword;
		} else if (HasOption(clear_options, named.option)) {
			result += " NO";
			result += named.keyword;
		}
	}
	return result;
}

string ValidUntilToSQL(int64_t valid_until) {
	return SQLString::ToString(Value::TIMESTAMPTZ(timestamp_tz_t(valid_until)).ToString());
}

static string ObjectNounOf(CatalogType type, optional_ptr<const MacroFunction> overload) {
	if (overload && overload->is_procedure) {
		return string(PROCEDURE_NOUN);
	}
	const auto noun = ObjectNoun(type);
	return noun.empty() ? StringUtil::Lower(CatalogTypeToString(type)) : string(noun);
}

void ThrowPermissionDenied(CatalogType type, std::string_view name, optional_ptr<const MacroFunction> overload) {
	throw PermissionException("permission denied for %s %s", ObjectNounOf(type, overload), name);
}

void ThrowPermissionDenied(const CatalogEntry &entry, optional_ptr<const MacroFunction> overload) {
	ThrowPermissionDenied(entry.type, entry.name.GetIdentifierName(), overload);
}

void RequireSuperuser(const RoleAccess &role) {
	if (!role.is_superuser) {
		throw PermissionException("permission denied: superuser required");
	}
}

static bool IsMacro(const CatalogEntry &entry) {
	return entry.type == CatalogType::MACRO_ENTRY || entry.type == CatalogType::TABLE_MACRO_ENTRY;
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

static bool OwnsEntry(const RoleAccess &role, const CatalogEntry &entry, optional_ptr<const MacroFunction> overload) {
	if (entry.type == CatalogType::ROLE_ENTRY) {
		return !entry.Cast<RoleEntry>().IsSuperuser() && role.Has(RoleOption::CreateRole) && role.AdminOf(entry.oid);
	}
	if (!IsMacro(entry)) {
		return role.Owns(entry.permissions.owner);
	}
	auto &macro = entry.Cast<MacroCatalogEntry>();
	if (overload) {
		return role.Owns(macro.OverloadPermissions(*overload).owner);
	}
	return std::all_of(macro.macros.begin(), macro.macros.end(), [&](const unique_ptr<MacroFunction> &function) {
		return role.Owns(macro.OverloadPermissions(*function).owner);
	});
}

bool Allowed(const RoleAccess &role, const CatalogEntry &entry, AclMode use,
             optional_ptr<const MacroFunction> overload) {
	D_ASSERT(!Uses(use, AclMode::Drop));
	if (role.is_superuser) {
		return true;
	}
	if (Uses(use, AclMode::Execute) && IsFunction(entry) && (!overload || IsUnowned(entry))) {
		return MayCall(role, entry.Cast<FunctionEntry>().required_capability);
	}
	if (Uses(use, AclMode::Own | AclMode::Rename) && !OwnsEntry(role, entry, overload)) {
		return false;
	}
	if (Uses(use, AclMode::Admin) && !role.AdminOf(entry.oid)) {
		return false;
	}
	if (Uses(use, AclMode::SetRole) && !role.CanSet(entry.oid)) {
		return false;
	}
	const auto modes = use & ACL_GRANTABLE;
	if (modes == AclMode::NoRights) {
		return true;
	}
	auto &permissions = overload ? entry.Cast<MacroCatalogEntry>().OverloadPermissions(*overload) : entry.permissions;
	return role.Can(ObjectKindOf(entry.type).acl_class, permissions, modes);
}

[[noreturn]] static void ThrowNotOwner(const CatalogEntry &entry, optional_ptr<const StatementNaming> naming) {
	if (naming) {
		throw PermissionException("must be owner of %s %s", naming->noun,
		                          StringUtil::Join(naming->name.Path(), ".",
		                                           [](const Identifier &part) { return part.GetIdentifierName(); }));
	}
	throw PermissionException("must be owner of %s %s", ObjectNounOf(entry.type, nullptr),
	                          entry.name.GetIdentifierName());
}

static void RequireOwnership(const RoleAccess &role, CatalogEntry &entry, optional_ptr<const MacroFunction> overload,
                             optional_ptr<const StatementNaming> naming) {
	if (entry.type == CatalogType::ROLE_ENTRY) {
		auto &target = entry.Cast<RoleEntry>();
		string detail;
		if (target.IsSuperuser()) {
			detail = "The current user must have the SUPERUSER attribute.";
		} else if (!role.Has(RoleOption::CreateRole)) {
			detail = "The current user must have the CREATEROLE attribute.";
		} else if (!role.AdminOf(target.oid)) {
			detail = StringUtil::Format("The current user must have the ADMIN option on role \"%s\".",
			                            target.name.GetIdentifierName());
		} else {
			return;
		}
		throw PermissionException({{"detail", detail}}, "permission denied");
	}
	if (!OwnsEntry(role, entry, overload)) {
		ThrowNotOwner(entry, naming);
	}
}

static void RequireCreateIn(ClientContext &context, const RoleAccess &role, CatalogEntry &container) {
	if (container.ParentCatalog().GetAttached().IsTemporary()) {
		auto &database = ClientData::Get(context).default_database;
		auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database);
		if (entry && !Allowed(role, *entry, AclMode::CreateTemp)) {
			throw PermissionException("permission denied to create temporary tables in database \"%s\"",
			                          database.GetIdentifierName());
		}
		return;
	}
	if (!Allowed(role, container, AclMode::Create)) {
		ThrowPermissionDenied(container);
	}
}

void Require(ClientContext &context, const RoleAccess &role, CatalogEntry &entry, AclMode use,
             optional_ptr<const MacroFunction> overload, optional_ptr<const StatementNaming> naming) {
	D_ASSERT(!Uses(use, AclMode::Admin | AclMode::SetRole));
	if (role.is_superuser) {
		return;
	}
	if (Uses(use, AclMode::Own | AclMode::Rename)) {
		RequireOwnership(role, entry, overload, naming);
	}
	if (Uses(use, AclMode::Own) && entry.type == CatalogType::SUBSCRIPTION_ENTRY &&
	    !entry.GetInfo()->Cast<CreateSubscriptionInfo>().password_required) {
		throw PermissionException(
		    unordered_map<string, string> {{"hint", "Subscriptions with the password_required option set to false "
		                                            "may only be created or modified by the superuser."}},
		    "password_required=false is superuser-only");
	}
	if (Uses(use, AclMode::Rename)) {
		const auto container = ObjectKindOf(entry.type).container;
		if (container == ObjectContainer::DATABASE) {
			RequireDatabasePrivilege(context, role, entry.ParentCatalog().GetName(), AclMode::Create);
		} else if (container != ObjectContainer::CLUSTER) {
			RequireCreateIn(context, role, entry.ParentSchema(context));
		}
	}
	if (Uses(use, AclMode::Drop)) {
		const auto container = ObjectKindOf(entry.type).container;
		const bool in_schema = container != ObjectContainer::DATABASE && container != ObjectContainer::CLUSTER;
		if (!in_schema || !role.Owns(entry.ParentSchema(context).permissions.owner)) {
			RequireOwnership(role, entry, overload, naming);
		}
	}
	if (entry.type == CatalogType::SCHEMA_ENTRY && Uses(use, AclMode::Create)) {
		RequireCreateIn(context, role, entry);
		use = use & ~AclMode::Create;
	}
	if (!Allowed(role, entry, use & ACL_GRANTABLE, overload)) {
		ThrowPermissionDenied(entry, overload);
	}
}

optional_ptr<CatalogEntry> ClusterEntry(ClientContext &context, CatalogType type, const Identifier &name) {
	auto cluster = RoleCatalog(context);
	if (!cluster) {
		return nullptr;
	}
	return cluster->LookupTopLevelEntry(cluster->GetCatalogTransaction(context), type, name);
}

optional_ptr<CatalogEntry> TopLevelEntry(ClientContext &context, const RoleAccess &role, Catalog &catalog,
                                         CatalogType type, const Identifier &name, AclMode use,
                                         OnEntryNotFound if_not_found) {
	auto entry = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context), type, name);
	if (!entry || !LookupVisible(context, role, *entry)) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingObject(type, name);
		}
		return nullptr;
	}
	Require(context, role, *entry, use);
	return entry;
}

optional_ptr<CatalogEntry> ClusterEntry(ClientContext &context, const RoleAccess &role, CatalogType type,
                                        const Identifier &name, AclMode use, OnEntryNotFound if_not_found) {
	auto cluster = RoleCatalog(context);
	if (cluster) {
		return TopLevelEntry(context, role, *cluster, type, name, use, if_not_found);
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException::MissingObject(type, name);
	}
	return nullptr;
}

CatalogEntry &ClusterDatabase(ClientContext &context, const Identifier &name) {
	return *ClusterEntry(context, RoleAccess::System(), CatalogType::DATABASE_ENTRY, name, AclMode::NoRights,
	                     OnEntryNotFound::THROW_EXCEPTION);
}

QualifiedName ClusterName(ClientContext &context, const QualifiedName &name) {
	auto cluster = RoleCatalog(context);
	if (!cluster) {
		return name;
	}
	return QualifiedName(cluster->GetName(), Identifier(), name.Name());
}

idx_t RoleId(ClientContext &context, const Identifier &name) {
	return ClusterEntry(context, RoleAccess::System(), CatalogType::ROLE_ENTRY, name, AclMode::NoRights,
	                    OnEntryNotFound::THROW_EXCEPTION)
	    ->oid;
}

static idx_t KeywordRole(ClientContext &context, RoleSpecType type) {
	return type == RoleSpecType::SESSION_USER ? context.session_role : context.current_role_id;
}

idx_t RoleSpecId(ClientContext &context, const RoleSpec &role) {
	return role.type == RoleSpecType::NAME ? RoleId(context, role.name) : KeywordRole(context, role.type);
}

optional_ptr<RoleEntry> RoleSpecEntry(ClientContext &context, const RoleSpec &role, OnEntryNotFound if_not_found) {
	if (role.type == RoleSpecType::NAME) {
		auto entry = ClusterEntry(context, RoleAccess::System(), CatalogType::ROLE_ENTRY, role.name, AclMode::NoRights,
		                          if_not_found);
		return entry ? &entry->Cast<RoleEntry>() : nullptr;
	}
	optional_ptr<CatalogEntry> entry;
	if (auto cluster = RoleCatalog(context)) {
		entry = cluster->LookupOwner(cluster->GetCatalogTransaction(context), KeywordRole(context, role.type));
	}
	if (entry && entry->type == CatalogType::ROLE_ENTRY) {
		return &entry->Cast<RoleEntry>();
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException::MissingObject(CatalogType::ROLE_ENTRY, Identifier(role.ToString()));
	}
	return nullptr;
}

string RoleName(ClientContext &context, idx_t role) {
	auto cluster = RoleCatalog(context);
	if (!cluster) {
		return string();
	}
	auto entry = cluster->LookupOwner(cluster->GetCatalogTransaction(context), role);
	return entry && entry->type == CatalogType::ROLE_ENTRY ? entry->name.GetIdentifierName() : string();
}

static bool StoresPermissions(Catalog &catalog) {
	auto &database = catalog.GetAttached();
	if (database.ForeignServer().IsValid()) {
		return false;
	}
	return !catalog.IsDuckCatalog() || database.IsTemporary() ||
	       IsSereneDBStorageVersion(database.GetStorageManager().GetStorageVersion());
}

void AssignOwner(ClientContext &context, Permissions &permissions, Catalog &catalog, CatalogType objtype,
                 optional_ptr<SchemaCatalogEntry> schema, idx_t owner) {
	if (!StoresPermissions(catalog)) {
		return;
	}
	auto database = ClusterEntry(context, CatalogType::DATABASE_ENTRY, catalog.GetName());
	permissions.AssignOwner(owner, objtype,
	                        database ? database->Cast<DatabaseEntry>().DefaultAcls() : std::span<const DefaultAcl>(),
	                        schema ? optional_idx(schema->oid) : optional_idx());
}

void RequireDatabasePrivilege(ClientContext &context, const RoleAccess &role, const Identifier &database,
                              AclMode need) {
	if (role.is_superuser) {
		return;
	}
	if (auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database)) {
		Require(context, role, *entry, need);
	}
}

void RequireForeignDataWrapperUsage(ClientContext &context, const RoleAccess &role, Catalog &catalog,
                                    std::string_view wrapper) {
	auto entry = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context),
	                                         CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, Identifier(wrapper));
	if (!entry) {
		throw CatalogException::MissingObject(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, Identifier(wrapper));
	}
	Require(context, role, *entry, AclMode::Usage);
}

void RequireUserSchema(const SchemaCatalogEntry &schema, const Identifier &name) {
	if (schema.internal && (!schema.catalog.IsDuckCatalog() || DefaultSchemaGenerator::IsDefaultSchema(schema.name))) {
		throw PermissionException(
		    unordered_map<string, string> {{"detail", "System catalog modifications are currently disallowed."}},
		    StringUtil::Format("permission denied to create \"%s.%s\"", schema.name.GetIdentifierName(),
		                       name.GetIdentifierName()));
	}
}

void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type, Catalog &catalog) {
	if (role.is_superuser) {
		return;
	}
	if (auto extension = type.GetExtensionInfo()) {
		auto oid = extension->properties.find(ExtensionTypeInfo::CATALOG_OID_PROPERTY);
		if (oid != extension->properties.end()) {
			auto entry = catalog.LookupOwner(catalog.GetCatalogTransaction(context), oid->second.GetValue<idx_t>());
			if (entry && entry->type == CatalogType::TYPE_ENTRY && !IsUnowned(*entry)) {
				Require(context, role, *entry, AclMode::Usage);
			}
			return;
		}
	}
	if (!type.HasParameters()) {
		return;
	}
	switch (type.id()) {
	case LogicalTypeId::LIST:
		return RequireTypeUsage(context, role, ListType::GetChildType(type), catalog);
	case LogicalTypeId::ARRAY:
		return RequireTypeUsage(context, role, ArrayType::GetChildType(type), catalog);
	case LogicalTypeId::MAP:
		RequireTypeUsage(context, role, MapType::KeyType(type), catalog);
		return RequireTypeUsage(context, role, MapType::ValueType(type), catalog);
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			RequireTypeUsage(context, role, child.second, catalog);
		}
		return;
	case LogicalTypeId::UNION:
		for (idx_t i = 0; i < UnionType::GetMemberCount(type); i++) {
			RequireTypeUsage(context, role, UnionType::GetMemberType(type, i), catalog);
		}
		return;
	default:
		return;
	}
}

} // namespace duckdb
