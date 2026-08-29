//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/default/default_views.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/catalog/default/default_generator.hpp"

namespace duckdb {
class SchemaIdentity;

class DefaultViewGenerator : public DefaultGenerator {
public:
	DefaultViewGenerator(Catalog &catalog, SchemaIdentity &identity);

	//! Not a schema entry: an alter chains a new one, and this generator outlives the version that built it
	SchemaIdentity &identity;

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;
};

} // namespace duckdb
