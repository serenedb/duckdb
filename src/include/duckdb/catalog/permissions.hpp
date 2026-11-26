//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/permissions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/acl.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/role_graph.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
class AttachedDatabase;
class Catalog;
class ClientContext;

struct StatementNaming {
	std::string_view noun;
	const QualifiedName &name;
};

struct PrivilegeTarget {
	PrivilegeTarget(const CatalogEntry &entry) : PrivilegeTarget(entry, entry.permissions) { // NOLINT
	}
	PrivilegeTarget(const CatalogEntry &entry, const Permissions &permissions)
	    : entry(entry), permissions(permissions) {
	}

	const CatalogEntry &entry;
	const Permissions &permissions;
};

DUCKDB_API const Permissions &PermissionsOf(PrivilegeTarget target);
DUCKDB_API bool EntryVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry);
constexpr uint64_t VISIBLE_TO_ALL = ~uint64_t(0);
DUCKDB_API uint64_t VisibilityBits(const CatalogEntry &entry);
DUCKDB_API uint64_t ViewerBits(const RoleAccess &role);
DUCKDB_API bool DatabaseVisible(ClientContext &context, const RoleAccess &role, AttachedDatabase &database);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(CatalogType type, std::string_view name);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(const CatalogEntry &entry);
DUCKDB_API bool Allowed(const RoleAccess &role, PrivilegeTarget target, AclMode use);
DUCKDB_API bool AllowedAny(const RoleAccess &role, PrivilegeTarget target, AclMode privileges);
DUCKDB_API void Require(ClientContext &context, const RoleAccess &role, PrivilegeTarget target, AclMode use,
                        optional_ptr<const StatementNaming> naming = nullptr);
DUCKDB_API void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type,
                                 Catalog &catalog);

} // namespace duckdb
