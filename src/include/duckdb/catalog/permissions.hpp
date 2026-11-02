//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/permissions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/role_capability.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

#include <functional>
#include <span>

namespace duckdb {
class CatalogEntry;
class ColumnList;

constexpr idx_t ACL_ID_PUBLIC = 0;

enum class AclMode : uint64_t {
	NoRights = 0x0,
	Insert = 0x1,
	Select = 0x2,
	Update = 0x4,
	Delete = 0x8,
	Truncate = 0x10,
	References = 0x20,
	Trigger = 0x40,
	Execute = 0x80,
	Usage = 0x100,
	Create = 0x200,
	CreateTemp = 0x400,
	Connect = 0x800,
	Set = 0x1000,
	AlterSystem = 0x2000,
	Maintain = 0x4000,
};

constexpr AclMode operator|(AclMode a, AclMode b) {
	return static_cast<AclMode>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}
constexpr AclMode operator&(AclMode a, AclMode b) {
	return static_cast<AclMode>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}
constexpr AclMode operator~(AclMode a) {
	return static_cast<AclMode>(~static_cast<uint64_t>(a));
}
constexpr AclMode &operator|=(AclMode &a, AclMode b) {
	a = a | b;
	return a;
}
constexpr AclMode &operator&=(AclMode &a, AclMode b) {
	a = a & b;
	return a;
}

constexpr AclMode ACL_COLUMN_PRIVILEGES = AclMode::Select | AclMode::Insert | AclMode::Update | AclMode::References;

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

struct AclItem {
	idx_t grantee = 0;
	idx_t grantor = 0;
	AclMode privs = AclMode::NoRights;
	AclMode grant_option = AclMode::NoRights;

	bool operator==(const AclItem &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static AclItem Deserialize(Deserializer &deserializer);
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

struct DefaultAcl {
	idx_t role = 0;
	idx_t scope = 0;
	CatalogType objtype = CatalogType::INVALID;
	vector<AclItem> acl;

	bool operator==(const DefaultAcl &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static DefaultAcl Deserialize(Deserializer &deserializer);
};

struct ObjectAcl {
	idx_t oid = 0;
	vector<AclItem> acl;

	bool operator==(const ObjectAcl &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static ObjectAcl Deserialize(Deserializer &deserializer);
};

struct ColumnPrivilege {
	Identifier column;
	AclMode privileges = AclMode::NoRights;

	bool operator==(const ColumnPrivilege &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static ColumnPrivilege Deserialize(Deserializer &deserializer);
};

struct Permissions {
	idx_t owner = 0;
	vector<AclItem> acl;
	vector<DefaultAcl> defaults;
	vector<ObjectAcl> foreign_data_wrappers;

	bool operator==(const Permissions &rhs) const = default;

	static CatalogType AclClass(CatalogType entry_type);
	static AclMode AllPrivileges(CatalogType acl_class);
	static AclMode PublicPrivileges(CatalogType acl_class);
	static bool TryParsePrivilege(std::string_view keyword, CatalogType acl_class, AclMode &result);
	static vector<AclItem> AclDefault(CatalogType acl_class, idx_t owner);

	std::span<const AclItem> ForeignDataWrapperAcl(idx_t wrapper) const;
	void AssignOwner(idx_t new_owner, CatalogType objtype, optional_ptr<const Permissions> database,
	                 optional_idx schema);

	void Serialize(Serializer &serializer) const;
	static Permissions Deserialize(Deserializer &deserializer);
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
	bool Can(CatalogType type, const Permissions &permissions, AclMode need) const;
	bool CanAny(CatalogType type, const Permissions &permissions, AclMode need) const;
	bool CanColumns(const Permissions &permissions, AclMode need, std::span<const std::span<const AclItem>> acls) const;
	bool CanAnyColumn(const Permissions &permissions, AclMode need, const ColumnList &columns) const;
	AclMode HeldModes(std::span<const AclItem> acl) const;
	AclMode GrantableModes(std::span<const AclItem> acl) const;
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

	explicit SecuritySnapshot(vector<Role> roles);
	virtual ~SecuritySnapshot();

	SecuritySnapshot(const SecuritySnapshot &) = delete;
	SecuritySnapshot &operator=(const SecuritySnapshot &) = delete;

	optional_ptr<const RoleAccess> Access(idx_t role) const;

private:
	vector<idx_t> arena;
	vector<RoleAccess> roles;
};

DUCKDB_API bool IsUnowned(const CatalogEntry &entry);
DUCKDB_API bool CanUseSchema(const RoleAccess &role, const CatalogEntry &schema);
DUCKDB_API bool ReferencesRole(const CatalogEntry &entry, idx_t role);
DUCKDB_API void ForEachRoleReference(const CatalogEntry &entry, const std::function<void(idx_t role)> &callback);
DUCKDB_API vector<idx_t> RoleReferences(const CatalogEntry &entry);
DUCKDB_API string PermissionKindName(CatalogType type);
DUCKDB_API string PermissionKindName(const CatalogEntry &entry);
[[noreturn]] DUCKDB_API void ThrowPermissionDenied(const CatalogEntry &entry);

} // namespace duckdb
