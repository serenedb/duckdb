//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/function_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/permissions.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"

namespace duckdb {

//! A function in the catalog
class FunctionEntry : public StandardEntry {
public:
	FunctionEntry(CatalogType type, Catalog &catalog, SchemaCatalogEntry &schema, CreateFunctionInfo &info);

	Identifier alias_of;
	vector<FunctionDescription> descriptions;
	bool superuser_only = false;
	RoleCapability superuser_capability = RoleCapability::None;

	bool MayCall(const RoleAccess &role) const {
		return !superuser_only || role.is_superuser ||
		       (superuser_capability != RoleCapability::None && role.Has(superuser_capability));
	}
};
} // namespace duckdb
