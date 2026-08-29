#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/transaction/commit_state.hpp"

namespace duckdb {

IndexDataTableInfo::IndexDataTableInfo(shared_ptr<DataTableInfo> info_p, const Identifier &index_name_p)
    : info(std::move(info_p)), index_name(index_name_p) {
}

void DuckIndexEntry::Rollback(CatalogEntry &prev_entry) {
	if (!prev_entry.deleted) {
		return;
	}
	auto table_info = TryGetDataTableInfo();
	if (!table_info) {
		return;
	}
	table_info->GetIndexes().RemoveIndex(name);
}

DuckIndexEntry::DuckIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
                               TableCatalogEntry &table_p)
    : IndexCatalogEntry(catalog, schema, create_info), initial_index_size(0) {
	auto &table = table_p.Cast<DuckTableEntry>();
	auto &storage = table.GetStorage();
	info = make_shared_ptr<IndexDataTableInfo>(storage.GetDataTableInfo(), name);
}

DuckIndexEntry::DuckIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
                               shared_ptr<IndexDataTableInfo> storage_info)
    : IndexCatalogEntry(catalog, schema, create_info), info(std::move(storage_info)), initial_index_size(0) {
}

unique_ptr<CatalogEntry> DuckIndexEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateIndexInfo>();

	auto result = make_uniq<DuckIndexEntry>(catalog, Schema(), cast_info, info);
	result->initial_index_size = initial_index_size;

	return std::move(result);
}

Identifier DuckIndexEntry::GetSchemaName() const {
	return GetDataTableInfo().GetSchemaName();
}

Identifier DuckIndexEntry::GetTableName() const {
	return GetDataTableInfo().GetTableName();
}

DataTableInfo &DuckIndexEntry::GetDataTableInfo() const {
	return *info->info;
}

optional_ptr<DataTableInfo> DuckIndexEntry::TryGetDataTableInfo() const {
	if (!info) {
		return nullptr;
	}
	return info->info.get();
}

void DuckIndexEntry::CommitDrop(CommitDropState &drop_state) {
	auto table_info = TryGetDataTableInfo();
	if (!table_info) {
		return;
	}
	drop_state.RemoveIndex(table_info->GetIndexes(), name);
}

} // namespace duckdb
