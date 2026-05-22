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

namespace duckdb {
class SchemaCatalogEntry;
class TypeConstructorSet;

using constructor_registration_t = void (*)(TypeConstructorSet &set);

struct DefaultType {
	const char *name;
	LogicalType type;
	//! Registers the constructors accepting this type's modifiers, or null if it takes no modifiers
	constructor_registration_t register_constructors;
};

class DefaultTypeGenerator : public DefaultGenerator {
public:
	DefaultTypeGenerator(Catalog &catalog, SchemaCatalogEntry &schema);

	SchemaCatalogEntry &schema;

public:
	DUCKDB_API static LogicalType GetDefaultType(const Identifier &name);
	DUCKDB_API static LogicalType TryDefaultBind(const string &name, const vector<pair<string, Value>> &params);

	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;
};

} // namespace duckdb
