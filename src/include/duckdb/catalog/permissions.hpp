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
#include "duckdb/execution/expression_executor_state.hpp"
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
DUCKDB_API RoleAccess CallerAccess(ExpressionState &state);

class PrivilegeCheck;
inline PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, PrivilegeTarget target, AclMode use);

class [[nodiscard]] PrivilegeCheck {
public:
	explicit operator bool() const {
		return denial == Denial::None;
	}
	void Require(optional_ptr<const StatementNaming> naming = nullptr) const {
		if (denial != Denial::None) {
			Throw(naming);
		}
	}

private:
	enum class Denial : uint8_t {
		None,
		Privilege,
		NotOwner,
		SuperuserRole,
		NoCreateRole,
		NoAdminOption,
		NoTemporaryTables
	};

	PrivilegeCheck() = default;
	PrivilegeCheck(Denial denial_p, const CatalogEntry &object_p) : object(object_p), denial(denial_p) {
	}

	friend PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, PrivilegeTarget target, AclMode use);
	DUCKDB_API static bool Holds(const RoleAccess &role, PrivilegeTarget target, AclMode use);
	DUCKDB_API static PrivilegeCheck Rules(ClientContext &context, const RoleAccess &role, PrivilegeTarget target,
	                                       AclMode use);
	static PrivilegeCheck Ownership(const RoleAccess &role, PrivilegeTarget target);
	static PrivilegeCheck CreateIn(ClientContext &context, const RoleAccess &role, const CatalogEntry &container);
	[[noreturn]] DUCKDB_API void Throw(optional_ptr<const StatementNaming> naming) const;

	optional_ptr<const CatalogEntry> object;
	Denial denial = Denial::None;
};

inline PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, PrivilegeTarget target, AclMode use) {
	if (role.is_superuser) {
		return {};
	}
	if ((use & (AclMode::Own | AclMode::Drop | AclMode::Rename | AclMode::Create)) != AclMode::NoRights) {
		return PrivilegeCheck::Rules(context, role, target, use);
	}
	if (PrivilegeCheck::Holds(role, target, use)) {
		return {};
	}
	return {PrivilegeCheck::Denial::Privilege, target.entry};
}

DUCKDB_API void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type,
                                 Catalog &catalog);

} // namespace duckdb
