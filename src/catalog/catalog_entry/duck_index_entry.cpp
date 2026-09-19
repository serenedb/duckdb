#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/transaction/commit_state.hpp"

namespace duckdb {

IndexDataTableInfo::IndexDataTableInfo(shared_ptr<DataTableInfo> info_p, const Identifier &index_name_p)
    : info(std::move(info_p)), index_name(index_name_p) {
}

unique_ptr<CatalogEntry> DuckIndexEntry::AlterEntry(ClientContext &context, AlterInfo &alter_info) {
	auto result = CatalogEntry::AlterEntry(context, alter_info);
	if (alter_info.type == AlterType::RENAME && info && info->info) {
		info->info->GetIndexes().RenameIndex(name, result->name);
	}
	return result;
}

void DuckIndexEntry::UndoAlter(ClientContext &context, AlterInfo &alter_info) {
	if (info && info->info) {
		info->info->GetIndexes().RenameIndex(alter_info.Cast<RenameInfo>().new_name, name);
	}
}

void DuckIndexEntry::Rollback(CatalogEntry &prev_entry) {
	if (!info || !info->info) {
		return;
	}
	auto &indexes = info->info->GetIndexes();
	if (prev_entry.type == CatalogType::INVALID) {
		indexes.RemoveIndex(name);
		return;
	}
	indexes.RenameIndex(name, prev_entry.name);
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

DataTableInfo &DuckIndexEntry::GetDataTableInfo() const {
	return *info->info;
}

void DuckIndexEntry::CommitDrop(CommitDropState &drop_state) {
	if (!info || !info->info) {
		return;
	}
	drop_state.RemoveIndex(GetDataTableInfo().GetIndexes(), name);
}

} // namespace duckdb
