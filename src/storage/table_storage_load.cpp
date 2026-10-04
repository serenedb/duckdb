#include "duckdb/storage/table_storage_load.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table_io_manager.hpp"

namespace duckdb {

namespace {

struct IndexDefinition {
	idx_t oid;
	Identifier name;
	IndexConstraintType constraint_type = IndexConstraintType::NONE;
	vector<LogicalIndex> columns;
	optional_ptr<DuckIndexEntry> entry;

	bool IsART() const {
		if (!entry) {
			return true;
		}
		return entry->index_type.empty() || entry->index_type == ART::TYPE_NAME;
	}
};

vector<IndexDefinition> GetIndexDefinitions(DuckTableEntry &table, const DataTableInfo &info) {
	vector<IndexDefinition> result;
	auto &columns = table.GetColumns();
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type == ConstraintType::UNIQUE) {
			auto &unique = constraint->Cast<UniqueConstraint>();
			IndexDefinition definition;
			definition.oid = unique.index_oid;
			definition.name = unique.GetName(table.name);
			definition.constraint_type =
			    unique.IsPrimaryKey() ? IndexConstraintType::PRIMARY : IndexConstraintType::UNIQUE;
			definition.columns = unique.GetLogicalIndexes(columns);
			result.push_back(std::move(definition));
		} else if (constraint->type == ConstraintType::FOREIGN_KEY) {
			auto &foreign_key = constraint->Cast<ForeignKeyConstraint>();
			if (foreign_key.info.type != ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE &&
			    foreign_key.info.type != ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				continue;
			}
			IndexDefinition definition;
			definition.oid = foreign_key.oid;
			definition.name =
			    Identifier("FOREIGN_" + table.name.GetIdentifierName() + "_" + to_string(foreign_key.oid));
			definition.constraint_type = IndexConstraintType::FOREIGN;
			for (auto &key : foreign_key.info.fk_keys) {
				definition.columns.push_back(columns.GetColumn(key).Logical());
			}
			result.push_back(std::move(definition));
		}
	}
	auto transaction = CatalogTransaction::GetSystemTransaction(table.ParentCatalog().GetDatabase());
	table.ParentSchema(transaction).Scan(CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
		auto &index = entry.Cast<DuckIndexEntry>();
		if (!index.info || index.info->info.get() != &info) {
			return;
		}
		IndexDefinition definition;
		definition.oid = index.oid;
		definition.name = index.name;
		definition.entry = index;
		result.push_back(std::move(definition));
	});
	return result;
}

IndexStorageInfo FreshIndexInfo(const IndexDefinition &definition) {
	IndexStorageInfo info(definition.name);
	info.options["catalog_oid"] = Value::UBIGINT(definition.oid);
	return info;
}

idx_t StoredIndexOid(const IndexStorageInfo &info) {
	auto entry = info.options.find("catalog_oid");
	if (entry == info.options.end()) {
		return 0;
	}
	return entry->second.GetValue<idx_t>();
}

void AttachIndexInstance(DuckTableEntry &table, DataTable &storage, const IndexDefinition &definition,
                         IndexStorageInfo info) {
	if (definition.entry) {
		auto create_info = definition.entry->GetInfo();
		create_info->oid = definition.entry->oid;
		storage.AddIndex(
		    make_uniq<UnboundIndex>(std::move(create_info), std::move(info), TableIOManager::Get(storage), storage.db),
		    definition.oid);
		return;
	}
	storage.AddIndex(table.GetColumns(), definition.columns, definition.constraint_type, std::move(info));
}

} // namespace

TableStorageLoad::TableStorageLoad(DuckCatalog &catalog_p, ClientContext &context_p)
    : catalog(catalog_p), load_context(context_p) {
	catalog.ScanSchemas([&](SchemaCatalogEntry &schema) {
		schema.Scan(CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.type == CatalogType::TABLE_ENTRY && entry.Cast<TableCatalogEntry>().IsDuckTable()) {
				tables.emplace(entry.oid, entry.Cast<DuckTableEntry>());
			}
		});
	});
}

TableStorageLoad::~TableStorageLoad() {
}

optional_ptr<DuckTableEntry> TableStorageLoad::Find(idx_t table_oid) {
	auto entry = storage.find(table_oid);
	if (entry == storage.end()) {
		return nullptr;
	}
	return entry->second->Cast<DuckTableEntry>();
}

SchemaCatalogEntry &TableStorageLoad::GetSchema(CatalogTransaction transaction, idx_t table_oid) {
	auto table = tables.find(table_oid);
	if (table != tables.end()) {
		return table->second.get().ParentSchema(transaction);
	}
	return catalog.GetSchema(transaction, *catalog.GetDefaultSchema());
}

void TableStorageLoad::LoadCheckpoint(BoundCreateTableInfo &info) {
	auto table = tables.find(info.Base().oid);
	if (table == tables.end()) {
		return;
	}
	auto index_infos = std::move(info.indexes);
	info.indexes.clear();
	auto &schema = table->second.get().ParentSchema(CatalogTransaction::GetSystemTransaction(catalog.GetDatabase()));
	Attach(table->second, make_uniq<DuckTableEntry>(catalog, schema, info), std::move(index_infos));
}

void TableStorageLoad::Create(ClientContext &context, unique_ptr<CreateInfo> info) {
	auto table = tables.find(info->oid);
	if (table == tables.end()) {
		return;
	}
	info->Cast<CreateTableInfo>().constraints.clear();
	auto &schema = table->second.get().ParentSchema(context);
	auto bound_info = Binder::BindCreateTableCheckpoint(std::move(info), schema);
	Attach(table->second, make_uniq<DuckTableEntry>(catalog, schema, *bound_info), {});
}

static void UseIndexLayoutOf(DuckTableEntry &table, DataTableInfo &info) {
	vector<idx_t> logical_oids;
	vector<idx_t> physical_oids;
	for (auto &column : table.GetColumns().Logical()) {
		logical_oids.push_back(column.CatalogOid());
		if (!column.Generated()) {
			physical_oids.push_back(column.CatalogOid());
		}
	}
	info.SetIndexColumnLayout(std::move(logical_oids), std::move(physical_oids));
}

void TableStorageLoad::Attach(DuckTableEntry &table, unique_ptr<CatalogEntry> entry,
                              vector<IndexStorageInfo> index_infos) {
	auto &shadow = entry->Cast<DuckTableEntry>();
	auto &shadow_storage = shadow.GetStorage();
	UseIndexLayoutOf(table, *shadow_storage.GetDataTableInfo());

	unordered_map<idx_t, IndexStorageInfo> stored;
	for (auto &index_info : index_infos) {
		auto oid = StoredIndexOid(index_info);
		if (oid) {
			stored.emplace(oid, std::move(index_info));
		}
	}
	auto &loaded = loaded_indexes[table.oid];
	loaded.clear();
	for (auto &definition : GetIndexDefinitions(table, *table.GetStorage().GetDataTableInfo())) {
		auto stored_info = stored.find(definition.oid);
		if (stored_info == stored.end()) {
			continue;
		}
		AttachIndexInstance(table, shadow_storage, definition, std::move(stored_info->second));
		loaded.insert(definition.oid);
		index_names[definition.oid] = definition.name;
	}
	BindExternalIndexes(table, *shadow_storage.GetDataTableInfo());
	shadow.SetAsRoot(nullptr, nullptr);

	auto &slot = storage[table.oid];
	if (slot) {
		retired.push_back(std::move(slot));
	}
	slot = std::move(entry);
}

void TableStorageLoad::BindExternalIndexes(DuckTableEntry &table, DataTableInfo &info) {
	auto &index_types = DBConfig::GetConfig(load_context).GetIndexTypes();
	for (auto &type_name : info.GetIndexes().DistinctIndexTypes()) {
		auto index_type = index_types.FindByName(type_name);
		if (!index_type || !index_type->defer_implicit_bind) {
			continue;
		}
		auto bind = [&]() {
			auto &search_path = *ClientData::Get(load_context).catalog_search_path;
			search_path.Set(CatalogSearchEntry(table.catalog.GetName(), table.ParentSchemaName()),
			                CatalogSetPathType::SET_SCHEMA);
			try {
				info.GetIndexes().Bind(load_context, table, index_type->name);
			} catch (...) {
				search_path.Reset();
				throw;
			}
			search_path.Reset();
		};
		if (load_context.transaction.HasActiveTransaction()) {
			bind();
		} else {
			load_context.RunFunctionInTransaction(bind);
		}
	}
}

void TableStorageLoad::Alter(ClientContext &context, optional_idx table_oid, AlterInfo &info) {
	if (!table_oid.IsValid()) {
		return;
	}
	auto entry = storage.find(table_oid.GetIndex());
	if (entry == storage.end()) {
		return;
	}
	auto altered = entry->second->AlterEntry(context, info);
	if (!altered) {
		return;
	}
	altered->SetAsRoot(nullptr, nullptr);
	retired.push_back(std::move(entry->second));
	entry->second = std::move(altered);
}

void TableStorageLoad::Drop(optional_idx table_oid) {
	if (!table_oid.IsValid()) {
		return;
	}
	auto entry = storage.find(table_oid.GetIndex());
	if (entry == storage.end()) {
		return;
	}
	retired.push_back(std::move(entry->second));
	storage.erase(entry);
}

void TableStorageLoad::CreateIndex(optional_idx table_oid, unique_ptr<CreateInfo> info, IndexStorageInfo storage_info) {
	if (!table_oid.IsValid()) {
		return;
	}
	pending_indexes.push_back(PendingIndex {table_oid.GetIndex(), info->oid, std::move(storage_info)});
}

void TableStorageLoad::AttachPendingIndexes() {
	auto pending = std::move(pending_indexes);
	pending_indexes.clear();
	for (auto &index : pending) {
		AttachIndex(std::move(index));
	}
}

void TableStorageLoad::AttachIndex(PendingIndex pending) {
	auto entry = storage.find(pending.table_oid);
	auto table = tables.find(pending.table_oid);
	if (entry == storage.end() || table == tables.end()) {
		return;
	}
	auto &final_table = table->second.get();
	auto &shadow = entry->second->Cast<DuckTableEntry>();
	for (auto &definition : GetIndexDefinitions(final_table, *final_table.GetStorage().GetDataTableInfo())) {
		if (definition.oid != pending.index_oid) {
			continue;
		}
		pending.storage_info.options["catalog_oid"] = Value::UBIGINT(pending.index_oid);
		UseIndexLayoutOf(final_table, *shadow.GetStorage().GetDataTableInfo());
		AttachIndexInstance(final_table, shadow.GetStorage(), definition, std::move(pending.storage_info));
		BindExternalIndexes(final_table, *shadow.GetStorage().GetDataTableInfo());
		shadow.SetAsRoot(nullptr, nullptr);
		loaded_indexes[pending.table_oid].insert(pending.index_oid);
		index_names[pending.index_oid] = definition.name;
		return;
	}
}

void TableStorageLoad::DropIndex(optional_idx table_oid, idx_t index_oid) {
	if (!table_oid.IsValid()) {
		return;
	}
	pending_indexes.erase(std::remove_if(pending_indexes.begin(), pending_indexes.end(),
	                                     [&](const PendingIndex &pending) { return pending.index_oid == index_oid; }),
	                      pending_indexes.end());
	auto entry = storage.find(table_oid.GetIndex());
	auto name = index_names.find(index_oid);
	if (entry == storage.end() || name == index_names.end()) {
		return;
	}
	entry->second->Cast<DuckTableEntry>().GetStorage().GetDataTableInfo()->GetIndexes().RemoveIndex(name->second);
	loaded_indexes[table_oid.GetIndex()].erase(index_oid);
	index_names.erase(name);
}

void TableStorageLoad::Install() {
	AttachPendingIndexes();
	for (auto &table_entry : tables) {
		auto &table = table_entry.second.get();
		auto source = storage.find(table_entry.first);
		if (source == storage.end()) {
			continue;
		}
		auto previous_info = table.GetStorage().GetDataTableInfo();
		table.ReplaceStorage(source->second->Cast<DuckTableEntry>());
		auto &data_table = table.GetStorage();
		auto info = data_table.GetDataTableInfo();
		auto &loaded = loaded_indexes[table.oid];
		unordered_set<idx_t> defined;
		vector<Identifier> rebuild;
		for (auto &definition : GetIndexDefinitions(table, *previous_info)) {
			defined.insert(definition.oid);
			if (definition.entry) {
				definition.entry->info->info = info;
			}
			if (loaded.count(definition.oid)) {
				continue;
			}
			AttachIndexInstance(table, data_table, definition, FreshIndexInfo(definition));
			if (definition.IsART()) {
				rebuild.push_back(definition.name);
			}
		}
		for (auto &oid : loaded) {
			if (!defined.count(oid)) {
				info->GetIndexes().RemoveIndex(index_names[oid]);
			}
		}
		BindExternalIndexes(table, *info);
		if (rebuild.empty()) {
			continue;
		}
		info->BindIndexes(load_context, ART::TYPE_NAME);
		for (auto entry : info->GetIndexes().IndexEntries()) {
			if (entry->GetBindState() != IndexBindState::BOUND) {
				continue;
			}
			for (auto &name : rebuild) {
				if (entry->GetName() == name) {
					data_table.RebuildIndex(*entry);
				}
			}
		}
	}
	storage.clear();
	retired.clear();
}

} // namespace duckdb
