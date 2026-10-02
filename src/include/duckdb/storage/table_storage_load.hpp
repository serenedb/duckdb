//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table_storage_load.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/index_storage_info.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

namespace duckdb {

class TableStorageLoad {
public:
	TableStorageLoad(DuckCatalog &catalog, ClientContext &context);
	~TableStorageLoad();

	optional_ptr<DuckTableEntry> Find(idx_t table_oid);
	SchemaCatalogEntry &GetSchema(CatalogTransaction transaction, idx_t table_oid);
	void LoadCheckpoint(BoundCreateTableInfo &info);
	void Create(ClientContext &context, unique_ptr<CreateInfo> info);
	void Alter(ClientContext &context, optional_idx table_oid, AlterInfo &info);
	void Drop(optional_idx table_oid);
	void CreateIndex(optional_idx table_oid, unique_ptr<CreateInfo> info, IndexStorageInfo storage_info);
	void DropIndex(optional_idx table_oid, idx_t index_oid);
	void AttachPendingIndexes();
	void Install();

private:
	struct PendingIndex {
		idx_t table_oid;
		idx_t index_oid;
		IndexStorageInfo storage_info;
	};

	void Attach(DuckTableEntry &table, unique_ptr<CatalogEntry> storage, vector<IndexStorageInfo> index_infos);
	void AttachIndex(PendingIndex pending);
	void BindExternalIndexes(DuckTableEntry &table, DataTableInfo &info);

private:
	vector<PendingIndex> pending_indexes;
	DuckCatalog &catalog;
	ClientContext &load_context;
	unordered_map<idx_t, reference<DuckTableEntry>> tables;
	unordered_map<idx_t, unique_ptr<CatalogEntry>> storage;
	unordered_map<idx_t, unordered_set<idx_t>> loaded_indexes;
	unordered_map<idx_t, idx_t> index_entry_oids;
	vector<unique_ptr<CatalogEntry>> retired;
};

} // namespace duckdb
