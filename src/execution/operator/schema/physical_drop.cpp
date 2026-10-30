#include "duckdb/execution/operator/schema/physical_drop.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/execution/operator/helper/physical_reset.hpp"
#include "duckdb/main/client_data.hpp"
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
	vector<unique_ptr<DropInfo>> drops;
	catalog.ScanSchemas(context, [&](SchemaCatalogEntry &schema) {
		for (auto type : {CatalogType::TABLE_ENTRY, CatalogType::SEQUENCE_ENTRY, CatalogType::MACRO_ENTRY,
		                  CatalogType::TABLE_MACRO_ENTRY, CatalogType::TYPE_ENTRY, CatalogType::JOB_ENTRY}) {
			schema.Scan(context, type, [&](CatalogEntry &entry) {
				if (entry.internal) {
					return;
				}
				auto drop = make_uniq<DropInfo>();
				drop->type = entry.type;
				drop->SetQualifiedName(
				    QualifiedName::FromCatalogSchema(catalog.GetName(), entry.ParentSchemaPath(), entry.name));
				drop->cascade = true;
				drop->if_not_found = OnEntryNotFound::RETURN_NULL;
				drops.push_back(std::move(drop));
			});
		}
	});
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
		PhysicalReset::ResetAll(context, SetScope::AUTOMATIC);
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
	case CatalogType::SCHEMA_ENTRY: {
		// the catalog is the leading component of the resolved path ([catalog, parent schemas..., schema])
		auto &catalog_name = info->GetQualifiedName().Path().front();
		auto &catalog = Catalog::GetCatalog(context.client, catalog_name);
		catalog.DropEntry(context.client, *info);
		// PG-compatible: leave search_path alone. The dropped schema becomes an
		// invalid entry that lookups will simply skip (see GetResolvedDefault).
		break;
	}
	case CatalogType::SECRET_ENTRY: {
		// Note: the schema param is used to optionally pass the storage to drop from
		D_ASSERT(info->extra_drop_info);
		auto &extra_info = info->extra_drop_info->Cast<ExtraDropSecretInfo>();
		SecretManager::Get(context.client)
		    .DropSecretByName(context.client, info->GetQualifiedName().Name(), info->if_not_found,
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
		if (!table_entry.DropTrigger(transaction, info->GetQualifiedName().Name(), info->cascade)) {
			if (info->if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				throw CatalogException("Trigger with name %s does not exist on table %s",
				                       info->GetQualifiedName().Name(), base_table_ref.Table());
			}
		}
		break;
	}
	default: {
		// the catalog may be empty -> the default catalog (e.g. for an unresolved DROP ... IF EXISTS of a missing
		// entry)
		auto &catalog = Catalog::GetCatalog(context.client, info->GetQualifiedName().Catalog());
		catalog.DropEntry(context.client, *info);
		break;
	}
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
