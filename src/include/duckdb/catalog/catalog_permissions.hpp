//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_permissions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

//! A privilege bitmask, spelled with the SQL standard's GRANT vocabulary.
//! Which bits a kind of entry accepts, and what holding one permits, is decided
//! by the catalog implementation that fills them in -- this is storage.
enum class AclMode : uint64_t {
	NoRights = 0,
	Insert = 1ULL << 0,
	Select = 1ULL << 1,
	Update = 1ULL << 2,
	Delete = 1ULL << 3,
	Truncate = 1ULL << 4,
	References = 1ULL << 5,
	Trigger = 1ULL << 6,
	Execute = 1ULL << 7,
	Usage = 1ULL << 8,
	Create = 1ULL << 9,
	CreateTemp = 1ULL << 10,
	Connect = 1ULL << 11,
	Set = 1ULL << 12,
	AlterSystem = 1ULL << 13,
	Maintain = 1ULL << 14,
};

inline constexpr AclMode operator&(AclMode lhs, AclMode rhs) noexcept {
	return static_cast<AclMode>(static_cast<uint64_t>(lhs) & static_cast<uint64_t>(rhs));
}
inline constexpr AclMode &operator&=(AclMode &lhs, AclMode rhs) noexcept {
	return lhs = lhs & rhs;
}
inline constexpr AclMode operator|(AclMode lhs, AclMode rhs) noexcept {
	return static_cast<AclMode>(static_cast<uint64_t>(lhs) | static_cast<uint64_t>(rhs));
}
inline constexpr AclMode &operator|=(AclMode &lhs, AclMode rhs) noexcept {
	return lhs = lhs | rhs;
}
inline constexpr AclMode operator^(AclMode lhs, AclMode rhs) noexcept {
	return static_cast<AclMode>(static_cast<uint64_t>(lhs) ^ static_cast<uint64_t>(rhs));
}
inline constexpr AclMode &operator^=(AclMode &lhs, AclMode rhs) noexcept {
	return lhs = lhs ^ rhs;
}
inline constexpr AclMode operator~(AclMode v) noexcept {
	return static_cast<AclMode>(~static_cast<uint64_t>(v));
}

//! One grant: which principal holds what, and who gave it. The principals are
//! host ids -- the identity space CreateInfo::oid and CatalogEntry::oid
//! share -- so an ACL never has to be resolved against anything but the catalog
//! that wrote it.
struct AclItem {
	idx_t grantee = 0;
	idx_t grantor = 0;
	AclMode privs = AclMode::NoRights;
	//! The subset of privs the grantee may re-grant.
	AclMode grant_option = AclMode::NoRights;

	void Serialize(Serializer &serializer) const;
	static AclItem Deserialize(Deserializer &deserializer);
};

//! Who owns a catalog entry and what has been granted on it. It sits on the
//! entry because CatalogSet versioning is what makes a grant transactional: a
//! second home beside the entry would need an MVCC of its own, and every reader
//! that needs an ACL already holds the entry.
//! One column's grants. A column has no entry of its own to keep an ACL on, and
//! postgres gives it no owner either -- the relation's own owner answers for all
//! of them -- so the grants sit with the rest of the entry's permissions.
struct ColumnAclItem {
	//! ColumnDefinition::catalog_oid -- the column's stable identity, not either
	//! of its positions.
	idx_t catalog_oid = 0;
	vector<AclItem> acl;

	void Serialize(Serializer &serializer) const;
	static ColumnAclItem Deserialize(Deserializer &deserializer);
};

struct CatalogPermissions {
	idx_t owner = 0;
	vector<AclItem> acl;
	//! Column-level grants, ordered by ColumnDefinition::catalog_oid. A vector
	//! rather than a map: almost every entry has none, a granted one has a
	//! handful, and one catalog state has to write one frame.
	vector<ColumnAclItem> column_acl;

	void Serialize(Serializer &serializer) const;
	static CatalogPermissions Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
