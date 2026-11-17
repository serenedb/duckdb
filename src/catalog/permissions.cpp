#include "duckdb/catalog/permissions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/role_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
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

void RelationNeed::Select(idx_t column) {
	modes |= AclMode::Select;
	if (std::find(selected.begin(), selected.end(), column) == selected.end()) {
		selected.push_back(column);
	}
}

void Require(const RelationNeed &need) {
	auto &role = need.role;
	auto &permissions = need.relation.permissions;
	if (role.Can(CatalogType::TABLE_ENTRY, permissions, need.modes)) {
		return;
	}
	const auto relation_only = need.modes & ~(AclMode::Select | AclMode::Insert | AclMode::Update);
	const auto columns_granted = [&](AclMode mode, const vector<idx_t> &columns) {
		return (need.modes & mode) == AclMode::NoRights || role.CanColumns(need.relation, mode, columns);
	};
	if ((relation_only != AclMode::NoRights && !role.Can(CatalogType::TABLE_ENTRY, permissions, relation_only)) ||
	    !columns_granted(AclMode::Select, need.selected) || !columns_granted(AclMode::Insert, need.inserted) ||
	    !columns_granted(AclMode::Update, need.updated)) {
		ThrowPermissionDenied(need.relation);
	}
}

bool IsUnowned(const CatalogEntry &entry) {
	auto &catalog = entry.ParentCatalog();
	if (!entry.internal && catalog.GetAttached().IsReadOnly()) {
		return true;
	}
	const bool stores_owners = catalog.Compatibility() == SqlCompatibility::POSTGRES;
	return entry.permissions.owner == 0 && (entry.internal || !stores_owners);
}

bool CanUseSchema(const RoleAccess &role, const CatalogEntry &schema) {
	return role.is_superuser || schema.internal || IsUnowned(schema) ||
	       schema.ParentCatalog().Compatibility() != SqlCompatibility::POSTGRES ||
	       role.Can(CatalogType::SCHEMA_ENTRY, schema.permissions, AclMode::Usage);
}

static bool MayCall(ClientContext &context, const RoleAccess &role, RoleCapability capability) {
	return capability == RoleCapability::None || role.Has(capability) ||
	       role.Owns(DBConfig::GetConfig(context).bootstrap_role);
}

bool MayCallFunction(ClientContext &context, const RoleAccess &role, const CatalogEntry &function) {
	return MayCall(context, role, function.Cast<FunctionEntry>().required_capability);
}

void CheckBuiltinCall(ClientContext &context, const RoleAccess &role, const CatalogEntry &function) {
	if (!MayCallFunction(context, role, function)) {
		ThrowPermissionDenied(function);
	}
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

static optional_ptr<CatalogEntry> FindForeignServer(ClientContext &context, const Identifier &name) {
	for (auto &db : DatabaseManager::Get(*context.db).GetDatabases()) {
		if (db->IsSystem() || db->IsTemporary() || db->IsReadOnly()) {
			continue;
		}
		auto &catalog = db->GetCatalog();
		if (auto server = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context),
		                                              CatalogType::FOREIGN_SERVER_ENTRY, name)) {
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
	auto server = FindForeignServer(context, database.GetName());
	return server && role.Can(CatalogType::FOREIGN_SERVER_ENTRY, server->permissions, AclMode::Usage);
}

bool EntryVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry) {
	if (entry.type == CatalogType::DATABASE_ENTRY) {
		return CanConnect(context, role, entry.name, ClusterEntry(context, CatalogType::DATABASE_ENTRY, entry.name));
	}
	if ((entry.type == CatalogType::TABLE_FUNCTION_ENTRY || entry.type == CatalogType::SCALAR_FUNCTION_ENTRY) &&
	    !MayCallFunction(context, role, entry)) {
		return false;
	}
	if (entry.internal || IsUnowned(entry)) {
		return true;
	}
	const auto kind = ObjectKindOf(entry.type);
	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::JOB_ENTRY:
		return role.CanAny(kind.acl_class, entry.permissions, kind.privileges);
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY: {
		if (role.CanAny(kind.acl_class, entry.permissions, kind.privileges)) {
			return true;
		}
		const auto grants = [&](std::span<const AclItem> acl) {
			return (role.HeldModes(acl) & ACL_COLUMN_PRIVILEGES) != AclMode::NoRights;
		};
		if (entry.type == CatalogType::VIEW_ENTRY) {
			auto &column_acls = entry.Cast<ViewCatalogEntry>().column_acls;
			return std::any_of(column_acls.begin(), column_acls.end(),
			                   [&](const ColumnAcl &column) { return grants(column.acl); });
		}
		auto columns = entry.Cast<TableCatalogEntry>().GetColumns().Logical();
		return std::any_of(columns.begin(), columns.end(),
		                   [&](const ColumnDefinition &column) { return grants(column.Acl()); });
	}
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		return std::any_of(macro.macros.begin(), macro.macros.end(), [&](const unique_ptr<MacroFunction> &overload) {
			return role.CanAny(kind.acl_class, macro.OverloadPermissions(*overload), kind.privileges);
		});
	}
	case CatalogType::INDEX_ENTRY: {
		auto &index = entry.Cast<IndexCatalogEntry>();
		auto relation = index.GetRelation(index.ParentCatalog().GetCatalogTransaction(context));
		return !relation || EntryVisible(context, role, *relation);
	}
	default:
		return true;
	}
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
		if (auto server = FindForeignServer(context, database.GetName())) {
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
	const auto any_acl = [&](const vector<AclItem> &acl) {
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
	for (auto &row : permissions.defaults) {
		if (named(row.role) || any_acl(row.acl)) {
			return true;
		}
	}
	for (auto &row : permissions.Rows().rows) {
		if (any_acl(row.acl)) {
			return true;
		}
	}
	if (entry.type == CatalogType::ROLE_ENTRY) {
		auto &member_of = entry.Cast<RoleEntry>().MemberOf();
		return std::any_of(member_of.begin(), member_of.end(),
		                   [&](const Membership &edge) { return named(edge.grantor); });
	}
	if (entry.type == CatalogType::VIEW_ENTRY) {
		auto &column_acls = entry.Cast<ViewCatalogEntry>().column_acls;
		return std::any_of(column_acls.begin(), column_acls.end(),
		                   [&](const ColumnAcl &column) { return any_acl(column.acl); });
	}
	if (entry.type != CatalogType::TABLE_ENTRY) {
		return false;
	}
	for (auto &column : entry.Cast<TableCatalogEntry>().GetColumns().Logical()) {
		if (any_acl(column.Acl())) {
			return true;
		}
	}
	return false;
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

void RequireEntryPrivilege(const RoleAccess &role, const CatalogEntry &entry, AclMode any) {
	if (!role.CanAny(ObjectKindOf(entry.type).acl_class, entry.permissions, any)) {
		ThrowPermissionDenied(entry);
	}
}

void RequireSuperuser(const RoleAccess &role) {
	if (!role.is_superuser) {
		throw PermissionException("permission denied: superuser required");
	}
}

void RequireOwner(const RoleAccess &role, const CatalogEntry &entry, idx_t owner,
                  optional_ptr<const MacroFunction> overload) {
	if (!role.Owns(owner)) {
		throw PermissionException("must be owner of %s %s", ObjectNounOf(entry.type, overload),
		                          entry.name.GetIdentifierName());
	}
}

bool IsAdminOf(ClientContext &context, const RoleAccess &role, idx_t target) {
	return context.security && context.security->MembershipsOf(role.role).IsAdminOf(target);
}

static void RequireRoleOwner(ClientContext &context, const RoleAccess &role, const RoleEntry &target) {
	if (role.is_superuser) {
		return;
	}
	string detail;
	if (target.IsSuperuser()) {
		detail = "The current user must have the SUPERUSER attribute.";
	} else if (!role.Has(RoleOption::CreateRole)) {
		detail = "The current user must have the CREATEROLE attribute.";
	} else if (!IsAdminOf(context, role, target.oid)) {
		detail = StringUtil::Format("The current user must have the ADMIN option on role \"%s\".",
		                            target.name.GetIdentifierName());
	} else {
		return;
	}
	throw PermissionException({{"detail", detail}}, "permission denied");
}

void RequireOwner(ClientContext &context, const RoleAccess &role, CatalogEntry &entry,
                  optional_ptr<MacroFunction> overload) {
	if (entry.type == CatalogType::ROLE_ENTRY) {
		RequireRoleOwner(context, role, entry.Cast<RoleEntry>());
		return;
	}
	if (entry.type == CatalogType::MACRO_ENTRY || entry.type == CatalogType::TABLE_MACRO_ENTRY) {
		auto &macro = entry.Cast<MacroCatalogEntry>();
		if (overload) {
			RequireOwner(role, entry, macro.OverloadPermissions(*overload).owner, overload.get());
			return;
		}
		for (auto &function : macro.macros) {
			RequireOwner(role, entry, macro.OverloadPermissions(*function).owner, function.get());
		}
		return;
	}
	auto owner = entry.permissions.owner;
	if (entry.type == CatalogType::INDEX_ENTRY) {
		auto relation =
		    entry.Cast<IndexCatalogEntry>().GetRelation(entry.ParentCatalog().GetCatalogTransaction(context));
		if (!relation) {
			return;
		}
		owner = relation->permissions.owner;
	}
	RequireOwner(role, entry, owner);
}

static void RequireSchemaCreate(ClientContext &context, const RoleAccess &role, SchemaCatalogEntry &schema) {
	if (role.is_superuser) {
		return;
	}
	if (schema.ParentCatalog().GetAttached().IsTemporary()) {
		auto &database = ClientData::Get(context).default_database;
		auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database);
		if (entry && !role.Can(CatalogType::DATABASE_ENTRY, entry->permissions, AclMode::CreateTemp)) {
			throw PermissionException("permission denied to create temporary tables in database \"%s\"",
			                          database.GetIdentifierName());
		}
		return;
	}
	RequireEntryPrivilege(role, schema, AclMode::Create);
}

void RequireUse(ClientContext &context, const RoleAccess &role, CatalogEntry &entry, EntryUse use,
                optional_ptr<MacroFunction> overload) {
	switch (use) {
	case EntryUse::REFERENCE:
		return;
	case EntryUse::OWN:
		RequireOwner(context, role, entry, overload);
		if (entry.type == CatalogType::SUBSCRIPTION_ENTRY && !role.is_superuser &&
		    !entry.GetInfo()->Cast<CreateSubscriptionInfo>().password_required) {
			throw PermissionException(
			    unordered_map<string, string> {{"hint", "Subscriptions with the password_required option set to false "
			                                            "may only be created or modified by the superuser."}},
			    "password_required=false is superuser-only");
		}
		return;
	case EntryUse::RENAME:
		RequireOwner(context, role, entry, overload);
		if (ObjectKindOf(entry.type).container == ObjectContainer::DATABASE) {
			RequireDatabasePrivilege(context, role, entry.ParentCatalog().GetName(), AclMode::Create);
		} else {
			RequireSchemaCreate(context, role, entry.ParentSchema(context));
		}
		return;
	case EntryUse::DROP: {
		const auto container = ObjectKindOf(entry.type).container;
		if (container == ObjectContainer::DATABASE || container == ObjectContainer::CLUSTER) {
			RequireOwner(context, role, entry);
		} else if (!role.Owns(entry.ParentSchema(context).permissions.owner)) {
			RequireOwner(context, role, entry, overload);
		}
		return;
	}
	case EntryUse::CREATE:
		RequireSchemaCreate(context, role, entry.Cast<SchemaCatalogEntry>());
		return;
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
                                         CatalogType type, const Identifier &name, EntryUse use,
                                         OnEntryNotFound if_not_found) {
	auto entry = catalog.LookupTopLevelEntry(catalog.GetCatalogTransaction(context), type, name);
	if (!entry) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingObject(type, name);
		}
		return nullptr;
	}
	RequireUse(context, role, *entry, use);
	return entry;
}

optional_ptr<CatalogEntry> ClusterEntry(ClientContext &context, const RoleAccess &role, CatalogType type,
                                        const Identifier &name, EntryUse use, OnEntryNotFound if_not_found) {
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
	return *ClusterEntry(context, RoleAccess::System(), CatalogType::DATABASE_ENTRY, name, EntryUse::REFERENCE,
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
	return ClusterEntry(context, RoleAccess::System(), CatalogType::ROLE_ENTRY, name, EntryUse::REFERENCE,
	                    OnEntryNotFound::THROW_EXCEPTION)
	    ->oid;
}

optional_idx RoleSpecKeyword(ClientContext &context, std::string_view name) {
	if (name == "CURRENT_USER" || name == "CURRENT_ROLE") {
		return context.current_role_id;
	}
	if (name == "SESSION_USER") {
		return context.session_role;
	}
	return optional_idx();
}

idx_t RoleSpecId(ClientContext &context, std::string_view name) {
	const auto keyword = RoleSpecKeyword(context, name);
	return keyword.IsValid() ? keyword.GetIndex() : RoleId(context, Identifier(name));
}

optional_ptr<RoleEntry> RoleSpecEntry(ClientContext &context, std::string_view name, OnEntryNotFound if_not_found) {
	const auto keyword = RoleSpecKeyword(context, name);
	if (!keyword.IsValid()) {
		auto entry = ClusterEntry(context, RoleAccess::System(), CatalogType::ROLE_ENTRY, Identifier(name),
		                          EntryUse::REFERENCE, if_not_found);
		return entry ? &entry->Cast<RoleEntry>() : nullptr;
	}
	optional_ptr<CatalogEntry> entry;
	if (auto cluster = RoleCatalog(context)) {
		entry = cluster->LookupOwner(cluster->GetCatalogTransaction(context), keyword.GetIndex());
	}
	if (entry && entry->type == CatalogType::ROLE_ENTRY) {
		return &entry->Cast<RoleEntry>();
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException::MissingObject(CatalogType::ROLE_ENTRY, Identifier(name));
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
	return !catalog.IsDuckCatalog() || catalog.GetAttached().IsTemporary() ||
	       IsSereneDBStorageVersion(catalog.GetAttached().GetStorageManager().GetStorageVersion());
}

void AssignOwner(ClientContext &context, Permissions &permissions, Catalog &catalog, CatalogType objtype,
                 optional_ptr<SchemaCatalogEntry> schema, idx_t owner) {
	if (!StoresPermissions(catalog)) {
		return;
	}
	auto database = ClusterEntry(context, CatalogType::DATABASE_ENTRY, catalog.GetName());
	permissions.AssignOwner(owner, objtype, database ? &database->permissions : nullptr,
	                        schema ? optional_idx(schema->oid) : optional_idx());
}

void RequireDatabasePrivilege(ClientContext &context, const RoleAccess &role, const Identifier &database,
                              AclMode need) {
	if (role.is_superuser) {
		return;
	}
	if (auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database)) {
		RequireEntryPrivilege(role, *entry, need);
	}
}

void RequireForeignDataWrapperUsage(ClientContext &context, const RoleAccess &role, const Identifier &database,
                                    std::string_view wrapper) {
	auto &config = DBConfig::GetConfig(context);
	auto &found = config.GetForeignDataWrapper(wrapper);
	if (role.Owns(config.bootstrap_role)) {
		return;
	}
	auto entry = ClusterEntry(context, CatalogType::DATABASE_ENTRY, database);
	if (!entry ||
	    !role.Can(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY,
	              entry->permissions.RowAcl(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, found.oid), AclMode::Usage)) {
		ThrowPermissionDenied(CatalogType::FOREIGN_DATA_WRAPPER_ENTRY, wrapper);
	}
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
	reference<const LogicalType> element = type;
	while (element.get().id() == LogicalTypeId::LIST || element.get().id() == LogicalTypeId::ARRAY) {
		element = element.get().id() == LogicalTypeId::LIST ? ListType::GetChildType(element)
		                                                    : ArrayType::GetChildType(element);
	}
	if (auto extension = element.get().GetExtensionInfo()) {
		auto oid = extension->properties.find(ExtensionTypeInfo::CATALOG_OID_PROPERTY);
		if (oid != extension->properties.end()) {
			auto entry = catalog.LookupOwner(catalog.GetCatalogTransaction(context), oid->second.GetValue<idx_t>());
			if (entry && entry->type == CatalogType::TYPE_ENTRY && !IsUnowned(*entry)) {
				RequireEntryPrivilege(role, *entry, AclMode::Usage);
			}
		}
	}
}

void CheckFunctionCalls(ClientContext &context, const RoleAccess &role, std::span<const FunctionCall> calls) {
	if (role.is_superuser) {
		return;
	}
	for (auto &call : calls) {
		if (!call.macro_overload.IsValid()) {
			if (!MayCall(context, role, call.capability)) {
				ThrowPermissionDenied(call.type, call.name);
			}
			continue;
		}
		auto catalog = Catalog::GetCatalogEntry(context, call.catalog);
		if (!catalog) {
			continue;
		}
		auto entry = catalog->LookupOwner(catalog->GetCatalogTransaction(context), call.macro_overload.GetIndex());
		if (!entry || (entry->type != CatalogType::MACRO_ENTRY && entry->type != CatalogType::TABLE_MACRO_ENTRY)) {
			continue;
		}
		auto &macro = entry->Cast<MacroCatalogEntry>();
		auto overload = macro.FindOverload(call.macro_overload.GetIndex());
		if (overload && !role.Can(macro.type, macro.OverloadPermissions(*overload), AclMode::Execute)) {
			ThrowPermissionDenied(macro, overload.get());
		}
	}
}

} // namespace duckdb
