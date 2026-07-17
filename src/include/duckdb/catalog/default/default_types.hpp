//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/default/default_types.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"

namespace duckdb {
class SchemaIdentity;

struct DefaultType {
	const char *name;
	LogicalType type;
	bind_logical_type_function_t bind_function;
};

class DefaultTypeGenerator : public DefaultGenerator {
public:
	DefaultTypeGenerator(Catalog &catalog, SchemaIdentity &identity);

	//! Not a schema entry: an alter chains a new one, and this generator outlives the version that built it
	SchemaIdentity &identity;

public:
	DUCKDB_API static LogicalType GetDefaultType(const Identifier &name);
	DUCKDB_API static LogicalType TryDefaultBind(const string &name, const vector<pair<string, Value>> &params);

	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;
};

} // namespace duckdb
