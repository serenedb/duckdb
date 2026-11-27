#include "duckdb/execution/operator/schema/physical_drop.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/privileges.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/execution/operator/helper/physical_reset.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

static void DropTemporaryObjects(ClientContext &context) {
	auto &catalog = Catalog::GetCatalog(context, TEMP_CATALOG);
	vector<CatalogType> types {CatalogType::TABLE_ENTRY, CatalogType::SEQUENCE_ENTRY, CatalogType::MACRO_ENTRY,
	                           CatalogType::TABLE_MACRO_ENTRY, CatalogType::TYPE_ENTRY};
	if (!catalog.IsDuckCatalog()) {
		types.push_back(CatalogType::JOB_ENTRY);
	}
	catalog_entry_vector_t entries;
	catalog_entry_map_t<vector<DropInfo>> drops;
	catalog.ScanSchemas(context, [&](SchemaCatalogEntry &schema) {
		for (auto type : types) {
			schema.Scan(context, type, [&](CatalogEntry &entry) {
				if (entry.internal) {
					return;
				}
				entries.push_back(entry);
				auto &drop = drops[entry].emplace_back();
				drop.type = entry.type;
				drop.SetQualifiedName(
				    QualifiedName::FromCatalogSchema(catalog.GetName(), entry.ParentSchemaPath(), entry.name));
				drop.cascade = true;
				drop.if_not_found = OnEntryNotFound::RETURN_NULL;
			});
		}
	});
	catalog.DropEntries(context, entries, drops);
}

static void ResetSequences(ClientContext &context) {
	auto &session = SequenceSession::Get(context);
	lock_guard<mutex> guard(session.lock);
	session.values.clear();
}

static void DeallocateAll(ClientContext &context) {
	auto &statements = ClientData::Get(context).prepared_statements;
	for (auto it = statements.begin(); it != statements.end();) {
		if (ClientContext::IsPreparedStatementHandle(it->first)) {
			++it;
		} else {
			statements.erase(it++);
		}
	}
}

static void Discard(ExecutionContext &context, DiscardTarget target) {
	switch (target) {
	case DiscardTarget::ALL:
		DeallocateAll(context.client);
		PhysicalReset::ResetAll(context.client, SetScope::AUTOMATIC, true);
		DropTemporaryObjects(context.client);
		ResetSequences(context.client);
		break;
	case DiscardTarget::PLANS:
		break;
	case DiscardTarget::SEQUENCES:
		ResetSequences(context.client);
		break;
	case DiscardTarget::TEMP:
		DropTemporaryObjects(context.client);
		break;
	}
}

static Catalog &DropCatalog(ClientContext &context, const DropInfo &target) {
	if (target.type == CatalogType::SCHEMA_ENTRY) {
		// the catalog is the leading component of the resolved path ([catalog, parent schemas..., schema])
		return Catalog::GetCatalog(context, target.GetQualifiedName().Path().front());
	}
	// the catalog may be empty -> the default catalog (e.g. for an unresolved DROP ... IF EXISTS of a missing
	// entry)
	return Catalog::GetDDLCatalog(context, target.type, target.GetQualifiedName().Catalog());
}

static optional_ptr<CatalogEntry> DropTarget(ClientContext &context, const DropInfo &target) {
	if (target.type == CatalogType::SCHEMA_ENTRY) {
		auto &path = target.GetQualifiedName().Path();
		return Catalog::GetSchema(context, path.front(), vector<Identifier>(path.begin() + 1, path.end()),
		                          OnEntryNotFound::RETURN_NULL)
		    .get();
	}
	if (IsCatalogLevel(target.type)) {
		return nullptr;
	}
	return Catalog::GetEntry(context, EntryLookupInfo(target.type, target.GetQualifiedName()),
	                         OnEntryNotFound::RETURN_NULL);
}

static void DropTogether(ClientContext &context, const DropInfo &info) {
	DropInfo statement(info);
	statement.objects.clear();
	vector<reference<Catalog>> catalogs;
	reference_map_t<Catalog, catalog_entry_vector_t> entries;
	reference_map_t<Catalog, catalog_entry_map_t<vector<DropInfo>>> drops;
	for (auto &object : info.objects) {
		DropInfo drop(statement);
		drop.type = object.type;
		drop.SetQualifiedName(object.name);
		drop.func_parameters = object.func_parameters;
		drop.has_func_args = object.has_func_args;
		auto entry = DropTarget(context, drop);
		if (!entry || entry->type != drop.type) {
			DropCatalog(context, drop).DropEntry(context, drop);
			continue;
		}
		drop.if_not_found = OnEntryNotFound::RETURN_NULL;
		auto &catalog = entry->ParentCatalog();
		auto &catalog_drops = drops[catalog];
		if (catalog_drops.empty()) {
			catalogs.push_back(catalog);
		}
		auto &entry_drops = catalog_drops[*entry];
		if (entry_drops.empty()) {
			entries[catalog].push_back(*entry);
		}
		entry_drops.push_back(std::move(drop));
	}
	for (auto &catalog : catalogs) {
		catalog.get().DropEntries(context, entries[catalog], drops[catalog]);
	}
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalDrop::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                               OperatorSourceInput &input) const {
	switch (info->type) {
	case CatalogType::PREPARED_STATEMENT: {
		auto &name = info->GetQualifiedName().Name();
		if (info->extra_drop_info) {
			Discard(context, info->extra_drop_info->Cast<ExtraDropDiscardInfo>().target);
		} else if (name.empty()) {
			DeallocateAll(context.client);
		} else if (!ClientData::Get(context.client).prepared_statements.erase(name) &&
		           info->if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException::MissingPreparedStatement(name);
		}
		break;
	}
	case CatalogType::SECRET_ENTRY: {
		// Note: the schema param is used to optionally pass the storage to drop from
		D_ASSERT(info->extra_drop_info);
		auto &extra_info = info->extra_drop_info->Cast<ExtraDropSecretInfo>();
		SecretManager::Get(context.client)
		    .DropSecretByName(context.client, info->objects.front().name.Name(), info->if_not_found,
		                      extra_info.persist_mode, Identifier(extra_info.secret_storage));
		break;
	}
	case CatalogType::TRIGGER_ENTRY: {
		// Triggers are stored per-table; must be dropped through the table, not the schema
		if (!info->extra_drop_info) {
			throw InternalException("DROP TRIGGER: missing extra_drop_info (expected ExtraDropTriggerInfo)");
		}
		auto &trigger_extra = info->extra_drop_info->Cast<ExtraDropTriggerInfo>();
		if (!trigger_extra.base_table) {
			throw InternalException("DROP TRIGGER: ExtraDropTriggerInfo has no base_table");
		}
		auto &base_table_ref = trigger_extra.base_table->Cast<BaseTableRef>();
		auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context.client, base_table_ref.GetQualifiedName());
		auto transaction = table_entry.ParentCatalog().GetCatalogTransaction(context.client);
		auto &trigger = info->objects.front().name.Name();
		if (info->if_not_found != OnEntryNotFound::THROW_EXCEPTION && !table_entry.GetTrigger(transaction, trigger)) {
			break;
		}
		if (!table_entry.DropTrigger(transaction, trigger, info->cascade)) {
			if (info->if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				throw CatalogException("Trigger with name %s does not exist on table %s", trigger,
				                       base_table_ref.Table());
			}
		}
		break;
	}
	case CatalogType::ROLE_ENTRY: {
		auto &catalog = Catalog::GetSystemCatalog(context.client);
		catalog.DropRole(catalog.GetCatalogTransaction(context.client), *info);
		break;
	}
	default:
		DropTogether(context.client, *info);
		break;
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
