//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/role_graph.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/acl.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/enums/role_capability.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <algorithm>

namespace duckdb {
class Catalog;
class StandardEntry;

enum class RoleOption : uint32_t {
	None = 0x0,
	Superuser = 0x1,
	Inherit = 0x2,
	CreateRole = 0x4,
	CreateDb = 0x8,
	Login = 0x10,
	Replication = 0x20,
	BypassRls = 0x40,
};

constexpr RoleOption operator|(RoleOption a, RoleOption b) {
	return static_cast<RoleOption>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr RoleOption operator&(RoleOption a, RoleOption b) {
	return static_cast<RoleOption>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr RoleOption operator~(RoleOption a) {
	return static_cast<RoleOption>(~static_cast<uint32_t>(a));
}
constexpr bool HasOption(RoleOption options, RoleOption option) {
	return (options & option) == option;
}

struct RoleOptionKeyword {
	RoleOption option;
	std::string_view keyword;
};

inline constexpr RoleOptionKeyword ROLE_OPTIONS[] = {
    {RoleOption::Superuser, "SUPERUSER"}, {RoleOption::CreateDb, "CREATEDB"}, {RoleOption::CreateRole, "CREATEROLE"},
    {RoleOption::Inherit, "INHERIT"},     {RoleOption::Login, "LOGIN"},       {RoleOption::Replication, "REPLICATION"},
    {RoleOption::BypassRls, "BYPASSRLS"},
};

struct Membership {
	idx_t role = 0;
	idx_t grantor = 0;
	bool admin_option = false;
	bool inherit_option = true;
	bool set_option = true;

	bool operator==(const Membership &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static Membership Deserialize(Deserializer &deserializer);
};

struct RoleAccess {
	idx_t role = 0;
	RoleOption options = RoleOption::None;
	RoleCapability capabilities = RoleCapability::None;
	bool is_superuser = false;
	std::span<const uint64_t> closure_filter;
	std::span<const idx_t> closure;
	std::span<const idx_t> settable;

	DUCKDB_API static const RoleAccess &System();
	bool IsSystem() const {
		return this == &System();
	}
	bool Has(RoleOption option) const {
		return HasOption(options, option);
	}
	bool Has(RoleCapability capability) const {
		return is_superuser || (capabilities & capability) == capability;
	}
	bool Uses(idx_t other) const;
	bool MemberOf(idx_t other) const {
		return is_superuser || Uses(other);
	}
	bool Owns(idx_t owner) const {
		return MemberOf(owner);
	}
	bool CanSet(idx_t other) const;
	bool Can(CatalogType type, std::span<const AclItem> acl, AclMode need) const;
	bool Can(CatalogType type, const Permissions &permissions, AclMode need) const;
	bool CanAny(CatalogType type, const Permissions &permissions, AclMode need) const;
	bool CanColumns(const StandardEntry &relation, AclMode need, std::span<const idx_t> columns) const;
	AclMode HeldModes(std::span<const AclItem> acl) const;
	AclMode GrantableModes(std::span<const AclItem> acl) const;
};

struct RoleMemberships {
	vector<idx_t> members;
	vector<idx_t> admin;

	bool IsMember(idx_t role) const {
		return std::binary_search(members.begin(), members.end(), role);
	}
	bool IsAdminOf(idx_t role) const {
		return std::binary_search(admin.begin(), admin.end(), role);
	}
};

class SecuritySnapshot {
public:
	struct Role {
		idx_t role = 0;
		RoleOption options = RoleOption::None;
		RoleCapability capabilities = RoleCapability::None;
		vector<idx_t> closure;
		vector<idx_t> settable;
	};
	struct Node {
		string name;
		RoleOption options = RoleOption::None;
		RoleCapability capabilities = RoleCapability::None;
		vector<Membership> member_of;
	};

	SecuritySnapshot(unordered_map<idx_t, Node> nodes, unordered_map<string, idx_t> database_owners,
	                 idx_t database_owner_role);
	~SecuritySnapshot();

	SecuritySnapshot(const SecuritySnapshot &) = delete;
	SecuritySnapshot &operator=(const SecuritySnapshot &) = delete;

	DUCKDB_API static shared_ptr<const SecuritySnapshot> Build(Catalog &cluster, CatalogTransaction transaction);
	shared_ptr<const SecuritySnapshot> ForDatabase(const Identifier &database) const;

	optional_ptr<const RoleAccess> Access(idx_t role) const;
	RoleAccess AccessOf(idx_t role) const;
	optional_ptr<const Node> Find(idx_t role) const;
	optional_idx FindByName(std::string_view name) const;
	std::string_view NameOf(idx_t role) const;
	RoleMemberships MembershipsOf(idx_t role) const;
	optional_idx BestAdmin(idx_t member, idx_t role) const;

private:
	vector<idx_t> arena;
	vector<RoleAccess> roles;
	unordered_map<idx_t, Node> nodes;
	unordered_map<string, idx_t> by_name;
	unordered_map<string, idx_t> database_owners;
	idx_t database_owner_role;
	mutable mutex derived_lock;
	mutable unordered_map<idx_t, shared_ptr<const SecuritySnapshot>> derived;
};

} // namespace duckdb
