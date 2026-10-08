#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/transaction/commit_state.hpp"

namespace duckdb {

IndexDataTableInfo::IndexDataTableInfo(shared_ptr<DataTableInfo> info_p) : info(std::move(info_p)) {
}

void DuckIndexEntry::SetAsRoot(optional_ptr<CatalogTransaction> transaction, optional_ptr<CatalogEntry> previous) {
	if (catalog.UsesCatalogLog() || !info || !info->info) {
		return;
	}
	info->info->GetIndexes().RenameIndex(oid, name);
}

void DuckIndexEntry::Rollback(CatalogEntry &prev_entry) {
	if (!info || !info->info) {
		return;
	}
	if (prev_entry.type == CatalogType::INVALID) {
		info->info->GetIndexes().RemoveIndex(oid);
	}
}

DuckIndexEntry::DuckIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
                               TableCatalogEntry &table_p)
    : IndexCatalogEntry(catalog, schema, create_info), initial_index_size(0) {
	auto &table = table_p.Cast<DuckTableEntry>();
	auto &storage = table.GetStorage();
	info = make_shared_ptr<IndexDataTableInfo>(storage.GetDataTableInfo());
}

DuckIndexEntry::DuckIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateIndexInfo &create_info,
                               shared_ptr<IndexDataTableInfo> storage_info)
    : IndexCatalogEntry(catalog, schema, create_info), info(std::move(storage_info)), initial_index_size(0) {
}

unique_ptr<CatalogEntry> DuckIndexEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateIndexInfo>();

	auto result = make_uniq<DuckIndexEntry>(catalog, ParentSchema(context), cast_info, info);
	result->initial_index_size = initial_index_size;

	return std::move(result);
}

Identifier DuckIndexEntry::GetSchemaName() const {
	return GetDataTableInfo().GetSchemaName();
}

Identifier DuckIndexEntry::GetTableName() const {
	return GetDataTableInfo().GetTableName();
}

optional_ptr<CatalogEntry> DuckIndexEntry::GetRelation(CatalogTransaction transaction) const {
	return catalog.Cast<DuckCatalog>().GetOidIndex().GetVisible(table_oid, transaction.view);
}

DataTableInfo &DuckIndexEntry::GetDataTableInfo() const {
	return *info->info;
}

void DuckIndexEntry::CommitDrop(CommitDropState &drop_state) {
	if (!info || !info->info) {
		return;
	}
	drop_state.RemoveIndex(GetDataTableInfo().GetIndexes(), oid);
}

} // namespace duckdb
