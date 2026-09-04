//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_permissions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! A privilege bitmask. Core never interprets these bits; it carries them so
//! every catalog entry has one home for ownership and grants, and the
//! access-control layer above reads them. Values mirror the server's bitmask,
//! like AccessVerb.
enum class AclMode : uint64_t {
	NoRights = 0,
	Select = 1ULL << 0,
	Insert = 1ULL << 1,
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

inline AclMode operator|(AclMode a, AclMode b) {
	return static_cast<AclMode>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}
inline AclMode operator&(AclMode a, AclMode b) {
	return static_cast<AclMode>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}
inline AclMode &operator|=(AclMode &a, AclMode b) {
	a = a | b;
	return a;
}

//! One grant on a catalog entry.
struct AclItem {
	idx_t grantee = 0;
	idx_t grantor = 0;
	AclMode privs = AclMode::NoRights;
	AclMode grant_option = AclMode::NoRights;

	bool operator==(const AclItem &rhs) const = default;
};

//! Ownership and grants of a catalog entry, carried by every entry kind.
struct CatalogPermissions {
	idx_t owner = 0;
	vector<AclItem> acl;
	//! Per-column grants, parallel to the relation's logical columns
	vector<vector<AclItem>> column_acl;
};

} // namespace duckdb
