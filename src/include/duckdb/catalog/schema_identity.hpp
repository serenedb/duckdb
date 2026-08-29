//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/schema_identity.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class SchemaCatalogEntry;

//! The part of a schema that outlives its entry versions. Altering a schema chains a new entry and the superseded
//! one is destroyed at cleanup, so an entry inside the schema holds this instead of the version it was created
//! under -- the same reason an index holds DataTableInfo rather than the TableCatalogEntry it was built on.
class SchemaIdentity {
public:
	virtual ~SchemaIdentity();

	//! The newest version of the schema entry. Re-seated by an alter before it commits and restored by UndoAlter,
	//! exactly as a table rename does with the name it keeps on the shared DataTableInfo.
	SchemaCatalogEntry &Schema() const {
		return *schema.get_mutable();
	}
	void Adopt(SchemaCatalogEntry &new_schema) {
		schema = &new_schema;
	}

private:
	optional_ptr<SchemaCatalogEntry> schema;
};

} // namespace duckdb
