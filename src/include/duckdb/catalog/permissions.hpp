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
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
class Serializer;
class Deserializer;
class ColumnList;
struct AlterPermissionsInfo;

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

struct DefaultAcl {
	idx_t role = 0;
	CatalogType objtype = CatalogType::INVALID;
	vector<AclItem> acl;

	bool operator==(const DefaultAcl &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static DefaultAcl Deserialize(Deserializer &deserializer);
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

	bool operator==(const Permissions &rhs) const = default;

	static CatalogType AclClass(CatalogType entry_type);
	static AclMode AllPrivileges(CatalogType acl_class);
	static AclMode PublicPrivileges(CatalogType acl_class);
	static bool TryParsePrivilege(const string &keyword, CatalogType acl_class, AclMode &result);
	static vector<AclItem> AclDefault(CatalogType acl_class, idx_t owner);

	void Alter(const AlterPermissionsInfo &info, CatalogType entry_type, optional_ptr<ColumnList> columns);

	void Serialize(Serializer &serializer) const;
	static Permissions Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
