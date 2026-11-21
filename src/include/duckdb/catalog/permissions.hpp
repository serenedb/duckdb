//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/permissions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/acl.hpp"
#include "duckdb/catalog/role_graph.hpp"
#include "duckdb/common/enums/entry_use.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include <functional>

namespace duckdb {
class AttachedDatabase;
class Catalog;
class CatalogEntry;
class ClientContext;
class MacroFunction;
class RoleEntry;
class SchemaCatalogEntry;
class StandardEntry;

struct ActingRoles {
	optional_ptr<const RoleAccess> lookups;
	optional_ptr<const RoleAccess> privileges;
	optional_ptr<const RoleAccess> current_user;
	bool security_definer = false;
};

struct RelationNeed {
	RelationNeed(const StandardEntry &relation_p, const RoleAccess &role_p) : relation(relation_p), role(role_p) {
	}

	void Select(idx_t column);

	const StandardEntry &relation;
	RoleAccess role;
	AclMode modes = AclMode::NoRights;
	vector<idx_t> selected;
	vector<idx_t> inserted;
	vector<idx_t> updated;
};

struct FunctionCall {
	CatalogType type = CatalogType::INVALID;
	string name;
	RoleCapability capability = RoleCapability::None;
	Identifier catalog;
	optional_idx macro_overload;
};

DUCKDB_API bool IsUnowned(const CatalogEntry &entry);
DUCKDB_API bool CanUseSchema(const RoleAccess &role, const CatalogEntry &schema);
DUCKDB_API void CheckBuiltinCall(ClientContext &context, const RoleAccess &role, const CatalogEntry &function);
DUCKDB_API bool MayCallFunction(ClientContext &context, const RoleAccess &role, const CatalogEntry &function);
DUCKDB_API bool EntryVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry);
DUCKDB_API bool DatabaseVisible(ClientContext &context, const RoleAccess &role, AttachedDatabase &database);
DUCKDB_API void CheckReference(ClientContext &context, const RoleAccess &role, CatalogEntry &entry);
DUCKDB_API bool ReferencesRole(const CatalogEntry &entry, idx_t role);
DUCKDB_API void ForEachRoleReference(const CatalogEntry &entry, const std::function<void(idx_t role)> &callback);
DUCKDB_API string RoleOptionsToSQL(RoleOption set_options, RoleOption clear_options);
DUCKDB_API string ValidUntilToSQL(int64_t valid_until);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(CatalogType type, std::string_view name,
                                                   optional_ptr<const MacroFunction> overload = nullptr);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(const CatalogEntry &entry,
                                                   optional_ptr<const MacroFunction> overload = nullptr);
DUCKDB_API void RequireEntryPrivilege(const RoleAccess &role, const CatalogEntry &entry, AclMode any);
DUCKDB_API void Require(const RelationNeed &need);
DUCKDB_API void RequireSuperuser(const RoleAccess &role);
DUCKDB_API bool IsAdminOf(ClientContext &context, const RoleAccess &role, idx_t target);
DUCKDB_API void RequireOwner(const RoleAccess &role, const CatalogEntry &entry, idx_t owner,
                             optional_ptr<const MacroFunction> overload = nullptr);
DUCKDB_API void RequireOwner(ClientContext &context, const RoleAccess &role, CatalogEntry &entry,
                             optional_ptr<MacroFunction> overload = nullptr);
DUCKDB_API void RequireUse(ClientContext &context, const RoleAccess &role, CatalogEntry &entry, EntryUse use,
                           optional_ptr<MacroFunction> overload = nullptr);
DUCKDB_API void RequireDatabasePrivilege(ClientContext &context, const RoleAccess &role, const Identifier &database,
                                         AclMode need);
DUCKDB_API void RequireForeignDataWrapperUsage(ClientContext &context, const RoleAccess &role,
                                               const Identifier &database, std::string_view wrapper);
DUCKDB_API void RequireUserSchema(const SchemaCatalogEntry &schema, const Identifier &name);
DUCKDB_API void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type,
                                 Catalog &catalog);
DUCKDB_API void CheckFunctionCalls(ClientContext &context, const RoleAccess &role, std::span<const FunctionCall> calls);
DUCKDB_API void AssignOwner(ClientContext &context, Permissions &permissions, Catalog &catalog, CatalogType objtype,
                            optional_ptr<SchemaCatalogEntry> schema, idx_t owner);
DUCKDB_API optional_ptr<CatalogEntry> ClusterEntry(ClientContext &context, CatalogType type, const Identifier &name);
DUCKDB_API optional_ptr<CatalogEntry> TopLevelEntry(ClientContext &context, const RoleAccess &role, Catalog &catalog,
                                                    CatalogType type, const Identifier &name, EntryUse use,
                                                    OnEntryNotFound if_not_found);
DUCKDB_API optional_ptr<CatalogEntry> ClusterEntry(ClientContext &context, const RoleAccess &role, CatalogType type,
                                                   const Identifier &name, EntryUse use, OnEntryNotFound if_not_found);
DUCKDB_API CatalogEntry &ClusterDatabase(ClientContext &context, const Identifier &name);
DUCKDB_API QualifiedName ClusterName(ClientContext &context, const QualifiedName &name);
DUCKDB_API idx_t RoleId(ClientContext &context, const Identifier &name);
DUCKDB_API optional_idx RoleSpecKeyword(ClientContext &context, std::string_view name);
DUCKDB_API idx_t RoleSpecId(ClientContext &context, std::string_view name);
DUCKDB_API optional_ptr<RoleEntry> RoleSpecEntry(ClientContext &context, std::string_view name,
                                                 OnEntryNotFound if_not_found);
DUCKDB_API string RoleName(ClientContext &context, idx_t role);

} // namespace duckdb
