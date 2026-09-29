//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table_storage_load.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/storage/index_storage_info.hpp"

namespace duckdb {
class CatalogEntry;
class ClientContext;
class DuckCatalog;
class DuckTableEntry;
class SchemaCatalogEntry;
struct AlterInfo;
struct BoundCreateTableInfo;
struct CreateInfo;
struct DataTableInfo;

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
	void Install();

private:
	void Attach(DuckTableEntry &table, unique_ptr<CatalogEntry> storage, vector<IndexStorageInfo> index_infos);
	void BindExternalIndexes(DuckTableEntry &table, DataTableInfo &info);

private:
	DuckCatalog &catalog;
	ClientContext &load_context;
	unordered_map<idx_t, reference<DuckTableEntry>> tables;
	unordered_map<idx_t, unique_ptr<CatalogEntry>> storage;
	unordered_map<idx_t, unordered_set<idx_t>> loaded_indexes;
	unordered_map<idx_t, Identifier> index_names;
	vector<unique_ptr<CatalogEntry>> retired;
};

} // namespace duckdb
