#include "duckdb/catalog/permissions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
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

#include <absl/algorithm/container.h>

namespace duckdb {

const Permissions &PermissionsOf(PrivilegeTarget target) {
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
	auto bits = RoleBit(permissions.owner);
	if (!permissions.UsesDefaultAcl()) {
		return bits | GranteeBits(permissions.acl, kind.privileges);
	}
	return (kind.public_privileges & kind.privileges) != AclMode::NoRights ? bits | PUBLIC_BIT : bits;
}

static uint64_t OwnBits(const CatalogEntry &entry) {
	if (entry.internal || PermissionsOf(entry).owner == 0) {
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

static string ObjectNounOf(CatalogType type) {
	const auto noun = ObjectKindOf(type).noun;
	return noun.empty() ? StringUtil::Lower(CatalogTypeToString(type)) : string(noun);
}

void ThrowPermissionDenied(CatalogType type, std::string_view name) {
	throw PermissionException("permission denied for %s %s", ObjectNounOf(type), name);
}

void ThrowPermissionDenied(const CatalogEntry &entry) {
	ThrowPermissionDenied(entry.type, entry.name.GetIdentifierName());
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

static bool OwnsEntry(const RoleAccess &role, PrivilegeTarget target) {
	auto &entry = target.entry;
	if (entry.type == CatalogType::ROLE_ENTRY) {
		return !entry.Cast<RoleEntry>().IsSuperuser() && role.Has(RoleOption::CreateRole) && role.AdminOf(entry.oid);
	}
	return role.MemberOf(target.permissions.owner);
}

static bool Holds(const RoleAccess &role, PrivilegeTarget target, AclMode use, bool any) {
	auto &entry = target.entry;
	auto &permissions = PermissionsOf(target);
	if (Uses(use, AclMode::Execute) && IsFunction(entry) && permissions.owner == 0) {
		const auto capability = entry.Cast<FunctionEntry>().required_capability;
		return capability == RoleCapability::None || role.Has(capability) || role.owns_builtins;
	}
	if (Uses(use, AclMode::Own | AclMode::Rename) && !OwnsEntry(role, target)) {
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
	const auto type = ObjectKindOf(entry.type).acl_class;
	return any ? role.CanAny(type, permissions, modes) : role.Can(type, permissions, modes);
}

bool Allowed(const RoleAccess &role, PrivilegeTarget target, AclMode use) {
	D_ASSERT(!Uses(use, AclMode::Drop));
	return role.is_superuser || Holds(role, target, use, false);
}

bool AllowedAny(const RoleAccess &role, PrivilegeTarget target, AclMode privileges) {
	return role.is_superuser || Holds(role, target, privileges, true);
}

[[noreturn]] static void ThrowNotOwner(const CatalogEntry &entry, optional_ptr<const StatementNaming> naming) {
	if (naming) {
		throw PermissionException("must be owner of %s %s", naming->noun,
		                          StringUtil::Join(naming->name.Path(), ".",
		                                           [](const Identifier &part) { return part.GetIdentifierName(); }));
	}
	throw PermissionException("must be owner of %s %s", ObjectNounOf(entry.type), entry.name.GetIdentifierName());
}

static void RequireOwnership(const RoleAccess &role, PrivilegeTarget target,
                             optional_ptr<const StatementNaming> naming) {
	auto &entry = target.entry;
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
		throw PermissionException::WithDetail(detail, "permission denied");
	}
	if (!OwnsEntry(role, target)) {
		ThrowNotOwner(entry, naming);
	}
}

static void RequireCreateIn(ClientContext &context, const RoleAccess &role, const CatalogEntry &container) {
	if (container.ParentCatalog().GetAttached().IsTemporary()) {
		auto &database = ClientData::Get(context).default_database;
		auto entry = SystemEntry(context, CatalogType::DATABASE_ENTRY, database);
		if (entry && !Holds(role, *entry, AclMode::CreateTemp, false)) {
			throw PermissionException("permission denied to create temporary tables in database \"%s\"",
			                          database.GetIdentifierName());
		}
		return;
	}
	if (!Holds(role, container, AclMode::Create, false)) {
		ThrowPermissionDenied(container);
	}
}

void Require(ClientContext &context, const RoleAccess &role, PrivilegeTarget target, AclMode use,
             optional_ptr<const StatementNaming> naming) {
	D_ASSERT(!Uses(use, AclMode::Admin | AclMode::SetRole));
	if (role.is_superuser) {
		return;
	}
	auto &entry = target.entry;
	if (Uses(use, AclMode::Own | AclMode::Rename)) {
		RequireOwnership(role, target, naming);
	}
	if (Uses(use, AclMode::Rename)) {
		const auto container = ObjectKindOf(entry.type).container;
		if (container == ObjectContainer::DATABASE) {
			auto database = SystemEntry(context, CatalogType::DATABASE_ENTRY, entry.ParentCatalog().GetName());
			if (database && !Holds(role, *database, AclMode::Create, false)) {
				ThrowPermissionDenied(*database);
			}
		} else if (container != ObjectContainer::CLUSTER) {
			RequireCreateIn(context, role, entry.ParentSchema(context));
		}
	}
	if (Uses(use, AclMode::Drop)) {
		const auto container = ObjectKindOf(entry.type).container;
		const bool in_schema = container != ObjectContainer::DATABASE && container != ObjectContainer::CLUSTER;
		if (!in_schema || !role.MemberOf(entry.ParentSchema(context).permissions.owner)) {
			RequireOwnership(role, target, naming);
		}
	}
	if (entry.type == CatalogType::SCHEMA_ENTRY && Uses(use, AclMode::Create)) {
		RequireCreateIn(context, role, entry);
		use = use & ~AclMode::Create;
	}
	if (!Holds(role, target, use & ACL_GRANTABLE, false)) {
		ThrowPermissionDenied(entry);
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
			if (entry && entry->type == CatalogType::TYPE_ENTRY && !Holds(role, *entry, AclMode::Usage, false)) {
				ThrowPermissionDenied(*entry);
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
