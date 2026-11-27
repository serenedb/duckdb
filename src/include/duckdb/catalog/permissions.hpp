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
#include "duckdb/common/identifier.hpp"

namespace duckdb {
class AttachedDatabase;
class Catalog;
class ClientContext;

struct StatementNaming {
	std::string_view noun;
	std::span<const Identifier> name;
};

struct PrivilegeTarget {
	PrivilegeTarget(const CatalogEntry &entry) : PrivilegeTarget(entry, entry.permissions) { // NOLINT
	}
	PrivilegeTarget(const CatalogEntry &entry, const Permissions &permissions)
	    : entry(entry), permissions(permissions) {
	}
	static PrivilegeTarget Columns(const CatalogEntry &relation, std::span<const LogicalIndex> columns) {
		PrivilegeTarget target(relation);
		target.columns = columns;
		target.of_columns = true;
		return target;
	}

	const CatalogEntry &entry;
	const Permissions &permissions;
	std::span<const LogicalIndex> columns;
	bool of_columns = false;
};

DUCKDB_API const Permissions &PermissionsOf(const PrivilegeTarget &target);
DUCKDB_API const RoleAccess &Role(ClientContext &context, optional_idx bound = optional_idx());
DUCKDB_API const RoleAccess &Role(ExpressionState &state);

class PrivilegeCheck;
inline PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target, AclMode use);

class [[nodiscard]] PrivilegeCheck {
public:
	PrivilegeCheck() = default;
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
		ParentPrivilege,
		NoSetRole,
		SuperuserRole,
		NoCreateRole,
		NoAdminOption,
		NoTemporaryTables
	};

	PrivilegeCheck(Denial denial_p, const CatalogEntry &object_p) : object(object_p), denial(denial_p) {
	}

	friend PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target,
	                            AclMode use);
	DUCKDB_API static bool Holds(const RoleAccess &role, const PrivilegeTarget &target, AclMode use);
	DUCKDB_API static PrivilegeCheck Rules(ClientContext &context, const RoleAccess &role,
	                                       const PrivilegeTarget &target, AclMode use);
	static PrivilegeCheck Ownership(const RoleAccess &role, const PrivilegeTarget &target);
	static PrivilegeCheck CreateIn(ClientContext &context, const RoleAccess &role, const CatalogEntry &container,
	                               Denial denied);
	[[noreturn]] DUCKDB_API void Throw(optional_ptr<const StatementNaming> naming) const;

	optional_ptr<const CatalogEntry> object;
	Denial denial = Denial::None;
};

inline PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target,
                            AclMode use) {
	if (role.is_superuser) {
		return {};
	}
	constexpr auto rules = AclMode::Own | AclMode::Drop | AclMode::Rename | AclMode::Create | AclMode::SetRole;
	if ((use & rules) != AclMode::NoRights) {
		return PrivilegeCheck::Rules(context, role, target, use);
	}
	if (PrivilegeCheck::Holds(role, target, use)) {
		return {};
	}
	return {PrivilegeCheck::Denial::Privilege, target.entry};
}

} // namespace duckdb
