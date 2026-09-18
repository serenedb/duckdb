//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/standard_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/catalog/schema_info.hpp"

namespace duckdb {
class SchemaCatalogEntry;

//! A StandardEntry is a catalog entry that is a member of a schema
class StandardEntry : public InCatalogEntry {
public:
	StandardEntry(CatalogType type, SchemaCatalogEntry &schema, Catalog &catalog, Identifier name, idx_t oid = 0);
	~StandardEntry() override {
	}

	shared_ptr<SchemaInfo> schema_info;
	//! The dependencies of the entry, can be empty
	LogicalDependencyList dependencies;

public:
	Identifier ParentSchemaName() const override {
		return schema_info->name;
	}
	idx_t ParentSchemaOid() const {
		return schema_info->oid;
	}
	SchemaCatalogEntry &ParentSchema(CatalogTransaction transaction) const override;
	using CatalogEntry::ParentSchema;
};

} // namespace duckdb
