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
#include "duckdb/common/types.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
class AttachedDatabase;
class Catalog;
class CatalogEntry;
class ClientContext;
class MacroFunction;

struct StatementNaming {
	std::string_view noun;
	const QualifiedName &name;
};

DUCKDB_API bool IsUnowned(const CatalogEntry &entry);
DUCKDB_API bool CanUseSchema(const RoleAccess &role, const CatalogEntry &schema);
DUCKDB_API bool EntryVisible(ClientContext &context, const RoleAccess &role, CatalogEntry &entry);
constexpr uint64_t VISIBLE_TO_ALL = ~uint64_t(0);
DUCKDB_API uint64_t VisibilityBits(const CatalogEntry &entry);
DUCKDB_API uint64_t ViewerBits(const RoleAccess &role);
DUCKDB_API bool DatabaseVisible(ClientContext &context, const RoleAccess &role, AttachedDatabase &database);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(CatalogType type, std::string_view name,
                                                   optional_ptr<const MacroFunction> overload = nullptr);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(const CatalogEntry &entry,
                                                   optional_ptr<const MacroFunction> overload = nullptr);
DUCKDB_API bool Allowed(const RoleAccess &role, const CatalogEntry &entry, AclMode use,
                        optional_ptr<const MacroFunction> overload = nullptr);
DUCKDB_API void Require(ClientContext &context, const RoleAccess &role, CatalogEntry &entry, AclMode use,
                        optional_ptr<const MacroFunction> overload = nullptr,
                        optional_ptr<const StatementNaming> naming = nullptr);
DUCKDB_API void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type,
                                 Catalog &catalog);

} // namespace duckdb
