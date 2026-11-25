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
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/role_spec.hpp"

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

struct StatementNaming {
	std::string_view noun;
	const QualifiedName &name;
};

struct RelationChecks {
	optional_ptr<const RoleAccess> role;
	idx_t reference = 0;
};

DUCKDB_API bool IsUnowned(const CatalogEntry &entry);
DUCKDB_API bool CanUseSchema(const RoleAccess &role, const CatalogEntry &schema);
DUCKDB_API bool EntryVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry);
DUCKDB_API bool LookupVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry);
constexpr uint64_t VISIBLE_TO_ALL = ~uint64_t(0);
DUCKDB_API uint64_t VisibilityBits(const CatalogEntry &entry);
DUCKDB_API uint64_t ViewerBits(const RoleAccess &role);
DUCKDB_API bool DatabaseVisible(ClientContext &context, const RoleAccess &role, AttachedDatabase &database);
DUCKDB_API void CheckReference(ClientContext &context, const RoleAccess &role, CatalogEntry &entry);
DUCKDB_API bool ReferencesRole(const CatalogEntry &entry, idx_t role);
DUCKDB_API void ForEachRoleReference(const CatalogEntry &entry, const std::function<void(idx_t role)> &callback);
DUCKDB_API string RoleOptionsToSQL(RoleOption set_options, RoleOption clear_options);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(CatalogType type, std::string_view name,
                                                   optional_ptr<const MacroFunction> overload = nullptr);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(const CatalogEntry &entry,
                                                   optional_ptr<const MacroFunction> overload = nullptr);
DUCKDB_API bool Allowed(const RoleAccess &role, const CatalogEntry &entry, AclMode use,
                        optional_ptr<const MacroFunction> overload = nullptr);
DUCKDB_API void Require(ClientContext &context, const RoleAccess &role, CatalogEntry &entry, AclMode use,
                        optional_ptr<const MacroFunction> overload = nullptr,
                        optional_ptr<const StatementNaming> naming = nullptr);
DUCKDB_API void RequireSuperuser(const RoleAccess &role);
DUCKDB_API void RequireUserSchema(const SchemaCatalogEntry &schema, const Identifier &name);
DUCKDB_API void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type,
                                 Catalog &catalog);
DUCKDB_API void AssignOwner(ClientContext &context, Permissions &permissions, Catalog &catalog, CatalogType objtype,
                            optional_ptr<SchemaCatalogEntry> schema, idx_t owner);
DUCKDB_API idx_t RoleId(ClientContext &context, const Identifier &name);
DUCKDB_API idx_t RoleSpecId(ClientContext &context, const RoleSpec &role);
DUCKDB_API optional_ptr<RoleEntry> RoleSpecEntry(ClientContext &context, const RoleSpec &role,
                                                 OnEntryNotFound if_not_found);
DUCKDB_API string RoleName(ClientContext &context, idx_t role);

} // namespace duckdb
