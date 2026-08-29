//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/dependency_list.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry_map.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/catalog/dependency.hpp"

namespace duckdb {
class Catalog;
class CatalogEntry;
struct CreateInfo;
class SchemaCatalogEntry;
struct CatalogTransaction;
class LogicalDependencyList;

//! A minimal representation of a CreateInfo / CatalogEntry
//! enough to look up the entry inside SchemaCatalogEntry::GetEntry
struct LogicalDependency {
public:
	CatalogEntryInfo entry;
	Identifier catalog;
	//! The dependent's pieces binding this entry (see DependencyPiece). Mutable because pieces of a
	//! duplicate-entry add merge into the member already in the set; hash and equality ignore them.
	mutable vector<DependencyPiece> pieces;
	//! Whether the dependent falls with this subject instead of blocking its drop. Mutable and ignored
	//! by hash and equality for the same reason pieces are; never persisted.
	mutable bool automatic = false;

public:
	explicit LogicalDependency(CatalogEntry &entry);
	LogicalDependency();
	LogicalDependency(optional_ptr<Catalog> catalog, CatalogEntryInfo entry, Identifier catalog_str);
	bool operator==(const LogicalDependency &other) const;

public:
	void Serialize(Serializer &serializer) const;
	static LogicalDependency Deserialize(Deserializer &deserializer);
};

struct LogicalDependencyHashFunction {
	uint64_t operator()(const LogicalDependency &a) const;
};

struct LogicalDependencyEquality {
	bool operator()(const LogicalDependency &a, const LogicalDependency &b) const;
};

//! The LogicalDependencyList containing LogicalDependency objects, not looked up in the catalog yet
class LogicalDependencyList {
	using create_info_set_t =
	    unordered_set<LogicalDependency, LogicalDependencyHashFunction, LogicalDependencyEquality>;

public:
	DUCKDB_API void AddDependency(CatalogEntry &entry);
	DUCKDB_API void AddDependency(const LogicalDependency &entry);
	DUCKDB_API bool Contains(CatalogEntry &entry) const;

public:
	void Serialize(Serializer &serializer) const;
	static LogicalDependencyList Deserialize(Deserializer &deserializer);
	bool operator==(const LogicalDependencyList &other) const;
	const create_info_set_t &Set() const;

private:
	create_info_set_t set;
};

} // namespace duckdb
