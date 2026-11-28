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
#include "duckdb/parser/column_list.hpp"

#include <absl/functional/function_ref.h>

namespace duckdb {
class AttachedDatabase;
class Catalog;
class ClientContext;

struct StatementNaming {
	std::string_view noun;
	std::span<const Identifier> name;
};

struct SchemaView {
	bool shown = false;
	bool owned = false;
	bool unowned = false;
};

struct PrivilegeTarget {
	PrivilegeTarget(const CatalogEntry &entry) : PrivilegeTarget(entry, entry.permissions) { // NOLINT
	}
	PrivilegeTarget(const CatalogEntry &entry, const Permissions &permissions)
	    : entry(&entry), type(entry.type), oid(entry.oid), permissions(permissions) {
	}
	PrivilegeTarget(CatalogType type, const Permissions &permissions) : type(type), permissions(permissions) {
	}
	static PrivilegeTarget Columns(const CatalogEntry &relation, std::span<const LogicalIndex> columns) {
		PrivilegeTarget target(relation);
		target.columns = columns;
		target.of_columns = true;
		return target;
	}
	static PrivilegeTarget Columns(CatalogType type, const Permissions &permissions, const ColumnList &list,
	                               std::span<const LogicalIndex> columns) {
		PrivilegeTarget target(type, permissions);
		target.column_list = &list;
		target.columns = columns;
		target.of_columns = true;
		return target;
	}
	static PrivilegeTarget In(const CatalogEntry &entry, const SchemaView &schema) {
		PrivilegeTarget target(entry);
		target.schema = &schema;
		return target;
	}
	static PrivilegeTarget Role(idx_t role) {
		static const Permissions ROLE_PERMISSIONS {};
		PrivilegeTarget target(CatalogType::ROLE_ENTRY, ROLE_PERMISSIONS);
		target.oid = role;
		return target;
	}
	static PrivilegeTarget Role(RoleCapability role) {
		return Role(PredefinedRoleOid(role));
	}

	optional_ptr<const CatalogEntry> entry;
	CatalogType type;
	idx_t oid = 0;
	const Permissions &permissions;
	std::span<const LogicalIndex> columns;
	optional_ptr<const ColumnList> column_list;
	bool of_columns = false;
	optional_ptr<const SchemaView> schema;
};

enum class StatementRule : uint8_t {
	Export,
	Load,
	UpdateExtensions,
	CopyDatabase,
	Connect,
	ExternalResource,
	Detach,
	Attach,
	CreateSecret,
	DropSecret,
	CreateTablespace,
	CreateDatabase,
	AlterDatabaseOwner,
	CreateSubscription,
	CreateRole,
	CreateSuperuserRole,
	CreateCreateDbRole,
	CreateReplicationRole,
	CreateBypassRlsRole,
	DropRole,
	SetSessionAuthorization
};

constexpr bool UseFits(AclMode use, CatalogType type) {
	if (type == CatalogType::ROLE_ENTRY) {
		return (use & ACL_GRANTABLE & ~AclMode::Usage) == AclMode::NoRights;
	}
	if ((use & (AclMode::SetRole | AclMode::Admin | AclMode::Member)) != AclMode::NoRights) {
		return false;
	}
	auto &kind = ObjectKindOf(type);
	const auto privileges = kind.routine ? kind.privileges | AclMode::Execute : kind.privileges;
	return (use & ACL_GRANTABLE & ~privileges) == AclMode::NoRights;
}

enum class RoleUse : uint8_t { Owner, Acl, Setting, Grantor };

struct RoleReference {
	idx_t role = 0;
	RoleUse use = RoleUse::Owner;
	idx_t overload_oid = 0;
	optional_ptr<const Identifier> column;
	optional_ptr<const DefaultAcl> default_acl;
};

DUCKDB_API const Permissions &PermissionsOf(const PrivilegeTarget &target);
DUCKDB_API const RoleAccess &Role(ClientContext &context, optional_idx bound = optional_idx());
DUCKDB_API const RoleAccess &Role(ExpressionState &state);
DUCKDB_API bool ForEachSeer(const CatalogEntry &entry, absl::FunctionRef<bool(idx_t)> visit);
DUCKDB_API bool AnyRoleReference(const CatalogEntry &entry, absl::FunctionRef<bool(const RoleReference &)> visit);
DUCKDB_API SchemaView ViewOf(ClientContext &context, const RoleAccess &role, const CatalogEntry &schema);

class PrivilegeCheck;
inline PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target, AclMode use);
inline PrivilegeCheck Check(const RoleAccess &role, const PrivilegeTarget &target, AclMode use);
inline PrivilegeCheck Check(const RoleAccess &role, StatementRule rule);

class [[nodiscard]] PrivilegeCheck {
public:
	static constexpr auto RULES =
	    AclMode::See | AclMode::Own | AclMode::Drop | AclMode::Rename | AclMode::Create | AclMode::SetRole;

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
		NoTemporaryTables,
		Hidden,
		Statement
	};

	PrivilegeCheck(Denial denial_p, optional_ptr<const CatalogEntry> object_p) : object(object_p), denial(denial_p) {
	}

	friend PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target,
	                            AclMode use);
	friend PrivilegeCheck Check(const RoleAccess &role, const PrivilegeTarget &target, AclMode use);
	friend PrivilegeCheck Check(const RoleAccess &role, StatementRule rule);
	DUCKDB_API static bool Holds(const RoleAccess &role, const PrivilegeTarget &target, AclMode use);
	DUCKDB_API static PrivilegeCheck Runs(const RoleAccess &role, StatementRule rule);
	DUCKDB_API static PrivilegeCheck Rules(ClientContext &context, const RoleAccess &role,
	                                       const PrivilegeTarget &target, AclMode use);
	static PrivilegeCheck Ownership(const RoleAccess &role, const PrivilegeTarget &target);
	static PrivilegeCheck CreateIn(ClientContext &context, const RoleAccess &role, const CatalogEntry &container,
	                               Denial denied);
	static bool Sees(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target);
	[[noreturn]] DUCKDB_API void Throw(optional_ptr<const StatementNaming> naming) const;

	optional_ptr<const CatalogEntry> object;
	Denial denial = Denial::None;
	StatementRule rule = StatementRule::Export;
};

inline PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target,
                            AclMode use) {
	if (role.is_superuser) {
		return {};
	}
	if ((use & PrivilegeCheck::RULES) != AclMode::NoRights) {
		return PrivilegeCheck::Rules(context, role, target, use);
	}
	if (PrivilegeCheck::Holds(role, target, use)) {
		return {};
	}
	return {PrivilegeCheck::Denial::Privilege, target.entry};
}

inline PrivilegeCheck Check(const RoleAccess &role, const PrivilegeTarget &target, AclMode use) {
	D_ASSERT((use & PrivilegeCheck::RULES) == AclMode::NoRights);
	if (role.is_superuser || PrivilegeCheck::Holds(role, target, use)) {
		return {};
	}
	return {PrivilegeCheck::Denial::Privilege, target.entry};
}

template <AclMode USE>
PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const PrivilegeTarget &target) {
	static_assert(USE != AclMode::NoRights);
	return Check(context, role, target, USE);
}

template <AclMode USE, class ENTRY>
PrivilegeCheck Check(ClientContext &context, const RoleAccess &role, const ENTRY &entry) {
	if constexpr (requires { ENTRY::Type; }) {
		static_assert(UseFits(USE, ENTRY::Type));
	}
	return Check<USE>(context, role, PrivilegeTarget(entry));
}

template <AclMode USE>
PrivilegeCheck Check(const RoleAccess &role, const PrivilegeTarget &target) {
	static_assert(USE != AclMode::NoRights && (USE & PrivilegeCheck::RULES) == AclMode::NoRights);
	return Check(role, target, USE);
}

inline PrivilegeCheck Check(const RoleAccess &role, StatementRule rule) {
	return role.is_superuser ? PrivilegeCheck() : PrivilegeCheck::Runs(role, rule);
}

template <StatementRule RULE>
PrivilegeCheck Check(const RoleAccess &role) {
	return Check(role, RULE);
}

} // namespace duckdb
