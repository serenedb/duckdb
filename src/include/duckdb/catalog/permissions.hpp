//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/permissions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/privileges.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/entry_use.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/enums/role_capability.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include <algorithm>
#include <functional>
#include <span>

namespace duckdb {
class AttachedDatabase;
class Catalog;
class CatalogEntry;
class ClientContext;
class ColumnList;
struct CreateInfo;
class DuckCatalog;
class FunctionEntry;
class MacroCatalogEntry;
class MacroFunction;
class RoleEntry;
class SchemaCatalogEntry;
class TableCatalogEntry;

constexpr idx_t ACL_ID_PUBLIC = 0;

DUCKDB_API AclMode FindPrivilege(std::string_view keyword);

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

struct AclItem {
	idx_t grantee = 0;
	idx_t grantor = 0;
	AclMode privs = AclMode::NoRights;
	AclMode grant_option = AclMode::NoRights;

	bool operator==(const AclItem &rhs) const = default;
	bool GrantsNothing() const {
		return privs == AclMode::NoRights && grant_option == AclMode::NoRights;
	}

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

struct ColumnPrivilege {
	Identifier column;
	AclMode privileges = AclMode::NoRights;

	bool operator==(const ColumnPrivilege &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static ColumnPrivilege Deserialize(Deserializer &deserializer);
};

struct ColumnAcl {
	Identifier column;
	vector<AclItem> acl;

	bool operator==(const ColumnAcl &rhs) const = default;

	void Serialize(Serializer &serializer) const;
	static ColumnAcl Deserialize(Deserializer &deserializer);
};

struct GrantRow {
	CatalogType kind = CatalogType::INVALID;
	idx_t oid = 0;
	string name;
	vector<AclItem> acl;

	bool operator==(const GrantRow &rhs) const = default;
	bool Is(CatalogType kind, idx_t oid, std::string_view name) const;

	void Serialize(Serializer &serializer) const;
	static GrantRow Deserialize(Deserializer &deserializer);
};

struct GrantRows {
	vector<GrantRow> rows;

	bool operator==(const GrantRows &rhs) const = default;
	std::span<const GrantRow> OfKind(CatalogType kind) const;

	void Serialize(Serializer &serializer) const;
	static shared_ptr<GrantRows> Deserialize(Deserializer &deserializer);
};

struct Permissions {
	idx_t owner = 0;
	vector<AclItem> acl;
	vector<DefaultAcl> defaults;
	shared_ptr<GrantRows> rows;

	bool operator==(const Permissions &rhs) const;

	const GrantRows &Rows() const;
	GrantRows &MutableRows();

	static bool TryParsePrivilege(std::string_view keyword, CatalogType acl_class, AclMode &result);
	static vector<AclItem> AclDefault(CatalogType acl_class, idx_t owner);

	optional_ptr<const GrantRow> FindRow(CatalogType kind, idx_t oid, std::string_view name = {}) const;
	std::span<const AclItem> RowAcl(CatalogType kind, idx_t oid, std::string_view name = {}) const;
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

struct ConstraintKeyReader {
	const RoleAccess &role;
	const TableCatalogEntry &table;

	DUCKDB_API bool CanRead(std::span<const LogicalIndex> columns) const;
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

struct FunctionCall {
	CatalogType type = CatalogType::INVALID;
	string name;
	RoleCapability capability = RoleCapability::None;
	Identifier catalog;
	optional_idx macro_overload;
};

DUCKDB_API bool IsUnowned(const CatalogEntry &entry);
DUCKDB_API std::span<const AclItem> FindColumnAcl(const vector<ColumnAcl> &columns, const Identifier &column);
DUCKDB_API std::span<const AclItem> ColumnAclAt(const CatalogEntry &entry, idx_t column_index);
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
DUCKDB_API void RequireSuperuser(const RoleAccess &role);
DUCKDB_API bool IsAdminOf(ClientContext &context, const RoleAccess &role, idx_t target);
DUCKDB_API void RequireOwner(const RoleAccess &role, const CatalogEntry &entry, idx_t owner,
                             optional_ptr<const MacroFunction> overload = nullptr);
DUCKDB_API void RequireOwner(ClientContext &context, const RoleAccess &role, CatalogEntry &entry,
                             optional_ptr<MacroFunction> overload = nullptr);
DUCKDB_API void RequireUse(ClientContext &context, const RoleAccess &role, CatalogEntry &entry, EntryUse use,
                           optional_ptr<MacroFunction> overload = nullptr);
DUCKDB_API void RequireCreate(ClientContext &context, const RoleAccess &role, SchemaCatalogEntry &schema,
                              const CreateInfo &info, const Identifier &name);
DUCKDB_API void RequireDatabasePrivilege(ClientContext &context, const RoleAccess &role, const Identifier &database,
                                         AclMode need);
DUCKDB_API void RequireForeignDataWrapperUsage(ClientContext &context, const RoleAccess &role,
                                               const Identifier &database, std::string_view wrapper);
DUCKDB_API void RequireUserSchema(const SchemaCatalogEntry &schema, const Identifier &name);
DUCKDB_API void RequireSchemaCreate(ClientContext &context, const RoleAccess &role, SchemaCatalogEntry &schema);
DUCKDB_API void RequireTypeUsage(ClientContext &context, const RoleAccess &role, const LogicalType &type,
                                 Catalog &catalog);
DUCKDB_API void CheckMacroCall(ClientContext &context, const RoleAccess &role, MacroCatalogEntry &macro,
                               const MacroFunction &overload);
DUCKDB_API FunctionCall FunctionCallOf(const CatalogEntry &function);
DUCKDB_API FunctionCall MacroCallOf(MacroCatalogEntry &macro, const MacroFunction &overload);
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
