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
#include "duckdb/catalog/schema_identity.hpp"

namespace duckdb {
class SchemaCatalogEntry;

//! A StandardEntry is a catalog entry that is a member of a schema
class StandardEntry : public InCatalogEntry {
public:
	StandardEntry(CatalogType type, SchemaCatalogEntry &schema, Catalog &catalog, Identifier name);
	~StandardEntry() override {
	}

	//! The dependencies of the entry, can be empty
	LogicalDependencyList dependencies;

public:
	//! The schema this entry belongs to, mutable even through a const entry: a const entry still builds
	//! non-const entries from its schema, which is what the reference member this replaces allowed.
	SchemaCatalogEntry &Schema() const {
		return schema_identity->Schema();
	}

	//! Resolved through the schema's identity rather than held directly: altering the schema chains a new entry
	//! and destroys the superseded one, which a reference captured at construction would outlive.
	SchemaCatalogEntry &ParentSchema() override {
		return schema_identity->Schema();
	}
	const SchemaCatalogEntry &ParentSchema() const override {
		return schema_identity->Schema();
	}

private:
	shared_ptr<SchemaIdentity> schema_identity;
};

} // namespace duckdb
