#include "duckdb/execution/operator/schema/physical_drop.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
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
	catalog.ScanSchemas(context, [&](SchemaCatalogEntry &schema) {
		for (auto type : types) {
			schema.Scan(context, type, [&](CatalogEntry &entry) {
				if (!entry.internal) {
					entries.push_back(entry);
				}
			});
		}
	});
	vector<unique_ptr<DropInfo>> drops;
	for (auto &entry : catalog.DropOrder(catalog.GetCatalogTransaction(context), entries)) {
		auto drop = make_uniq<DropInfo>();
		drop->type = entry.get().type;
		drop->SetQualifiedName(
		    QualifiedName::FromCatalogSchema(catalog.GetName(), entry.get().ParentSchemaPath(), entry.get().name));
		drop->cascade = true;
		drop->if_not_found = OnEntryNotFound::RETURN_NULL;
		drops.push_back(std::move(drop));
	}
	for (auto &drop : drops) {
		catalog.DropEntry(context, *drop);
	}
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
		PhysicalReset::ResetAll(context, SetScope::AUTOMATIC, true);
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
	switch (target.type) {
	case CatalogType::SCHEMA_ENTRY: {
		auto &path = target.GetQualifiedName().Path();
		return Catalog::GetSchema(context, path.front(), vector<Identifier>(path.begin() + 1, path.end()),
		                          OnEntryNotFound::RETURN_NULL)
		    .get();
	}
	case CatalogType::DATABASE_ENTRY:
	case CatalogType::FOREIGN_SERVER_ENTRY:
	case CatalogType::SUBSCRIPTION_ENTRY:
		return nullptr;
	default:
		return Catalog::GetEntry(context, EntryLookupInfo(target.type, target.GetQualifiedName()),
		                         OnEntryNotFound::RETURN_NULL);
	}
}

static void DropTogether(ClientContext &context, const vector<reference<DropInfo>> &infos) {
	vector<reference<Catalog>> catalogs;
	reference_map_t<Catalog, catalog_entry_vector_t> entries;
	catalog_entry_map_t<vector<reference<DropInfo>>> targets;
	for (auto &target : infos) {
		auto entry = DropTarget(context, target);
		if (!entry) {
			DropCatalog(context, target).DropEntry(context, target);
			continue;
		}
		auto &listed = targets[*entry];
		if (listed.empty()) {
			auto &catalog_entries = entries[entry->ParentCatalog()];
			if (catalog_entries.empty()) {
				catalogs.push_back(entry->ParentCatalog());
			}
			catalog_entries.push_back(*entry);
		}
		listed.push_back(target);
	}
	const bool cascade =
	    std::all_of(infos.begin(), infos.end(), [](const reference<DropInfo> &info) { return info.get().cascade; });
	for (auto &catalog : catalogs) {
		auto &catalog_entries = entries[catalog];
		auto transaction = catalog.get().GetCatalogTransaction(context);
		if (!cascade && catalog_entries.size() > 1) {
			catalog.get().RefuseDroppingTogether(transaction, catalog_entries);
		}
		vector<idx_t> together;
		for (auto &entry : catalog_entries) {
			together.push_back(entry.get().oid);
		}
		for (auto &entry : catalog.get().DropOrder(transaction, catalog_entries)) {
			for (auto &target : targets[entry]) {
				auto drop = target.get().Copy();
				drop->if_not_found = OnEntryNotFound::RETURN_NULL;
				drop->dropped_together = together;
				DropCatalog(context, *drop).DropEntry(context, *drop);
			}
		}
	}
}

static void DropPreparedStatement(ExecutionContext &context, const DropInfo &info) {
	auto &name = info.GetQualifiedName().Name();
	if (info.extra_drop_info) {
		Discard(context, info.extra_drop_info->Cast<ExtraDropDiscardInfo>().target);
	} else if (name.empty()) {
		DeallocateAll(context.client);
	} else if (!ClientData::Get(context.client).prepared_statements.erase(name) &&
	           info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException::MissingPreparedStatement(name);
	}
}

static void DropSecret(ExecutionContext &context, const DropInfo &info) {
	// Note: the schema param is used to optionally pass the storage to drop from
	D_ASSERT(info.extra_drop_info);
	auto &extra_info = info.extra_drop_info->Cast<ExtraDropSecretInfo>();
	SecretManager::Get(context.client)
	    .DropSecretByName(context.client, info.GetQualifiedName().Name(), info.if_not_found, extra_info.persist_mode,
	                      Identifier(extra_info.secret_storage));
}

static void DropTrigger(ExecutionContext &context, const DropInfo &info) {
	// Triggers are stored per-table; must be dropped through the table, not the schema
	if (!info.extra_drop_info) {
		throw InternalException("DROP TRIGGER: missing extra_drop_info (expected ExtraDropTriggerInfo)");
	}
	auto &trigger_extra = info.extra_drop_info->Cast<ExtraDropTriggerInfo>();
	if (!trigger_extra.base_table) {
		throw InternalException("DROP TRIGGER: ExtraDropTriggerInfo has no base_table");
	}
	auto &base_table_ref = trigger_extra.base_table->Cast<BaseTableRef>();
	auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context.client, base_table_ref.GetQualifiedName());
	auto transaction = table_entry.ParentCatalog().GetCatalogTransaction(context.client);
	if (info.if_not_found != OnEntryNotFound::THROW_EXCEPTION &&
	    !table_entry.GetTrigger(transaction, info.GetQualifiedName().Name())) {
		return;
	}
	if (!table_entry.DropTrigger(transaction, info.GetQualifiedName().Name(), info.cascade)) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException("Trigger with name %s does not exist on table %s", info.GetQualifiedName().Name(),
			                       base_table_ref.Table());
		}
	}
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType PhysicalDrop::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                               OperatorSourceInput &input) const {
	vector<reference<DropInfo>> roles;
	vector<reference<DropInfo>> together;
	for (auto &info : infos) {
		switch (info->type) {
		case CatalogType::PREPARED_STATEMENT:
			DropPreparedStatement(context, *info);
			break;
		case CatalogType::SECRET_ENTRY:
			DropSecret(context, *info);
			break;
		case CatalogType::TRIGGER_ENTRY:
			DropTrigger(context, *info);
			break;
		case CatalogType::ROLE_ENTRY:
			roles.push_back(*info);
			break;
		default:
			together.push_back(*info);
			break;
		}
	}
	if (!roles.empty()) {
		auto &catalog = Catalog::GetSystemCatalog(context.client);
		catalog.DropRole(catalog.GetCatalogTransaction(context.client), roles);
	}
	DropTogether(context.client, together);
	return SourceResultType::FINISHED;
}

} // namespace duckdb
