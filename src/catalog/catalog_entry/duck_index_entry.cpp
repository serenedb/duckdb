#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
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
	if (CreatedObject(prev_entry)) {
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

unique_ptr<CatalogEntry> DuckIndexEntry::AlterEntry(CatalogTransaction transaction, AlterInfo &info) {
	if (auto new_schema = info.GetNewSchema()) {
		auto &context = transaction.GetContext();
		auto previous_path = ParentSchemaPath(transaction);
		auto result = CopyToSchema(context, *new_schema);
		auto &index = result->Cast<IndexCatalogEntry>();
		auto new_path = index.ParentSchemaPath(transaction);
		LogicalDependencyList dependencies;
		for (auto &dependency : index.dependencies.Set()) {
			auto moved = dependency;
			if (moved.entry.type == CatalogType::TABLE_ENTRY &&
			    CatalogEntryInfo::SamePath(moved.entry.schema_path, previous_path)) {
				moved.entry.schema_path = new_path;
			}
			dependencies.AddDependency(moved);
		}
		index.dependencies = std::move(dependencies);
		index.sql = index.GetInfo()->ToString();
		return result;
	}
	if (info.type != AlterType::REPLACE_DEFINITION) {
		return IndexCatalogEntry::AlterEntry(transaction, info);
	}
	auto &definition = info.Cast<ReplaceDefinitionInfo>().definition->Cast<CreateIndexInfo>();
	auto result = Copy(transaction.GetContext());
	auto &index = result->Cast<DuckIndexEntry>();
	index.sql = definition.sql;
	index.column_ids = definition.column_ids;
	index.dependencies = definition.dependencies;
	index.expressions.clear();
	for (auto &expr : definition.expressions) {
		index.expressions.push_back(expr->Copy());
	}
	index.parsed_expressions.clear();
	for (auto &expr : definition.parsed_expressions) {
		index.parsed_expressions.push_back(expr->Copy());
	}
	index.where_clause = definition.where_clause ? definition.where_clause->Copy() : nullptr;
	return result;
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
