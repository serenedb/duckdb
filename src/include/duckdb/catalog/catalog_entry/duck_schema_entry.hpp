//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/duck_schema_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_coordinate_system_info.hpp"

namespace duckdb {

//! Everything a schema holds, shared by every version of the schema entry. Altering a schema chains a new entry,
//! and its contents must not move with it -- the same handover a table rename makes with its DataTable.
//! `case_sensitive` keys every set by the exact name, for a catalog that folds identifiers by its own rules.
class SchemaCatalogSets {
public:
	SchemaCatalogSets(Catalog &catalog, bool case_sensitive);
	virtual ~SchemaCatalogSets() = default;

	//! Get the catalog set for the specified type
	virtual CatalogSet &Get(CatalogType type);
	void Verify(Catalog &catalog);

	const shared_ptr<SchemaIdentity> &GetIdentity() const {
		return identity;
	}

private:
	//! What the entries inside these sets point at. Held beside the sets rather than being them: an entry that
	//! owned the set that owns it is a cycle, and the whole schema leaks with it.
	shared_ptr<SchemaIdentity> identity;
	//! The catalog set holding the tables
	CatalogSet tables;
	//! The catalog set holding the indexes
	CatalogSet indexes;
	//! The catalog set holding the table functions
	CatalogSet table_functions;
	//! The catalog set holding the copy functions
	CatalogSet copy_functions;
	//! The catalog set holding the pragma functions
	CatalogSet pragma_functions;
	//! The catalog set holding the scalar and aggregate functions
	CatalogSet functions;
	//! The catalog set holding the sequences
	CatalogSet sequences;
	//! The catalog set holding the collations
	CatalogSet collations;
	//! The catalog set holding the types
	CatalogSet types;
	//! The catalog set holding the coordinate systems
	CatalogSet coordinate_systems;
};

//! A schema in the catalog
class DuckSchemaEntry : public SchemaCatalogEntry {
public:
	DuckSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

protected:
	//! Supersede the version that currently holds `sets`, taking the schema's whole contents over
	DuckSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, const shared_ptr<SchemaCatalogSets> &sets);

	//! Held by every version of the schema entry, so the contents survive the version that created them
	shared_ptr<SchemaCatalogSets> sets;

public:
	optional_ptr<CatalogEntry> AddEntry(CatalogTransaction transaction, unique_ptr<StandardEntry> entry,
	                                    OnCreateConflict on_conflict);
	//! `replaces` is the name the entry a REPLACE_ON_CONFLICT supersedes is filed under, when that is not the
	//! name the new entry carries -- a rename that is also a redefinition.
	optional_ptr<CatalogEntry> AddEntryInternal(CatalogTransaction transaction, unique_ptr<StandardEntry> entry,
	                                            OnCreateConflict on_conflict, LogicalDependencyList dependencies,
	                                            optional_ptr<const Identifier> replaces = nullptr);

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateCoordinateSystem(CatalogTransaction transaction,
	                                                  CreateCoordinateSystemInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;
	CatalogSet::EntryLookup LookupEntryDetailed(CatalogTransaction transaction,
	                                            const EntryLookupInfo &lookup_info) override;
	SimilarCatalogEntry GetSimilarEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;

	//! This version is about to be destroyed: hand the schema's contents back to the version it superseded,
	//! which every entry inside the schema resolves through.
	void Rollback(CatalogEntry &prev_entry) override;
	//! The alter that produced a superseding version was refused: reclaim the contents for this one.
	void UndoAlter(ClientContext &context, AlterInfo &info) override;

	void Verify(Catalog &catalog) override;

	//! Get the catalog set for the specified type
	CatalogSet &GetCatalogSet(CatalogType type);

private:
	void OnDropEntry(CatalogTransaction transaction, CatalogEntry &entry);
};
} // namespace duckdb
