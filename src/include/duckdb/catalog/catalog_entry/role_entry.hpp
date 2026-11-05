//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/role_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/permissions.hpp"

namespace duckdb {

class RoleEntry : public InCatalogEntry {
public:
	static constexpr const CatalogType Type = CatalogType::ROLE_ENTRY;
	static constexpr const char *Name = "role";

	RoleEntry(Catalog &catalog, Identifier name, idx_t oid, RoleOption options, vector<Membership> member_of,
	          RoleCapability capabilities, bool predefined);

	RoleOption Options() const {
		return options;
	}
	bool IsSuperuser() const {
		return HasOption(options, RoleOption::Superuser);
	}
	const vector<Membership> &MemberOf() const {
		return member_of;
	}
	RoleCapability Capabilities() const {
		return capabilities;
	}
	bool IsPredefined() const {
		return predefined;
	}

protected:
	RoleOption options;
	vector<Membership> member_of;
	RoleCapability capabilities;
	bool predefined;
};

} // namespace duckdb
