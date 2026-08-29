//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/default/default_coordinate_systems.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/common/array_ptr.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"

namespace duckdb {
class SchemaIdentity;

class DefaultCoordinateSystemGenerator : public DefaultGenerator {
public:
	DefaultCoordinateSystemGenerator(Catalog &catalog, SchemaIdentity &identity);

	//! Not a schema entry: an alter chains a new one, and this generator outlives the version that built it
	SchemaIdentity &identity;

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;
};

} // namespace duckdb
