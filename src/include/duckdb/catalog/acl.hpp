//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/acl.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/privileges.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/vector.hpp"

#include <span>
#include <string_view>

namespace duckdb {

constexpr idx_t ACL_ID_PUBLIC = 0;

DUCKDB_API AclMode FindPrivilege(std::string_view keyword);

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

struct DefaultAcl {
	idx_t role = 0;
	idx_t scope = 0;
	CatalogType objtype = CatalogType::INVALID;
	vector<AclItem> acl;
	idx_t oid = 0;

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

struct Permissions {
	idx_t owner = 0;
	vector<AclItem> acl;
	vector<DefaultAcl> defaults;
	bool acl_emptied = false;

	bool operator==(const Permissions &rhs) const = default;
	bool UsesDefaultAcl() const {
		return acl.empty() && !acl_emptied;
	}

	static bool TryParsePrivilege(std::string_view keyword, CatalogType acl_class, AclMode &result);
	static vector<AclItem> AclDefault(CatalogType acl_class, idx_t owner);

	void AssignOwner(idx_t new_owner, CatalogType objtype, optional_ptr<const Permissions> database,
	                 optional_idx schema);

	void Serialize(Serializer &serializer) const;
	static Permissions Deserialize(Deserializer &deserializer);
};

DUCKDB_API std::span<const AclItem> FindColumnAcl(const vector<ColumnAcl> &columns, const Identifier &column);

} // namespace duckdb
