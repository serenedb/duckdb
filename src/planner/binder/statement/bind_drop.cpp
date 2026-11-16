#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_drop.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

void Binder::BindDropTrigger(DropInfo &info, StatementProperties &properties) {
	if (!info.extra_drop_info) {
		throw BinderException("DROP TRIGGER requires an ON clause specifying the table");
	}
	auto &trigger_extra = info.extra_drop_info->Cast<ExtraDropTriggerInfo>();
	if (!trigger_extra.base_table) {
		throw BinderException("DROP TRIGGER requires an ON clause specifying the table");
	}
	auto &base_table_ref = trigger_extra.base_table->Cast<BaseTableRef>();
	// resolve the (possibly nested) catalog/schema qualification of the base table
	base_table_ref.SetQualifiedName(BindTableName(base_table_ref.GetQualifiedName()));
	// IF EXISTS only guards the trigger, not the table (PostgreSQL-compatible behavior).
	auto &table_entry = entry_retriever.GetEntry<TableCatalogEntry>(base_table_ref.GetQualifiedName(), EntryUse::OWN);
	// the trigger lives in the same (possibly nested) schema as its base table
	info.SetQualifiedName(table_entry.GetQualifiedName(info.GetQualifiedName().Name()));
	properties.RegisterDBModify(table_entry.ParentCatalog(), context, DatabaseModificationType::DROP_CATALOG_ENTRY);
}

void Binder::BindDrop(DropInfo &info, StatementProperties &properties) {
	switch (info.type) {
	case CatalogType::PREPARED_STATEMENT:
		// dropping prepared statements is always possible
		// it also does not require a valid transaction
		properties.requires_valid_transaction = false;
		break;
	case CatalogType::SCHEMA_ENTRY: {
		// resolve the leading component of the dotted path into a catalog, leaving the path as
		// [catalog, parent schemas..., schema] (mirrors CREATE SCHEMA - see Binder::BindCreateSchema)
		info.SetQualifiedName(ResolveCatalog(context, info.GetQualifiedName()));
		// dropping a schema is never read-only because there are no temporary schemas. The catalog is the leading
		// component of the resolved path ([catalog, parent schemas..., schema])
		auto &path = info.GetQualifiedName().Path();
		auto &catalog = Catalog::GetCatalog(context, path.front());
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::DROP_CATALOG_ENTRY);
		entry_retriever.GetSchema(path.front(), vector<Identifier>(path.begin() + 1, path.end()), EntryUse::DROP,
		                          OnEntryNotFound::RETURN_NULL);
		break;
	}
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::TABLE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::JOB_ENTRY:
	case CatalogType::TOKENIZER_ENTRY: {
		// Resolve the catalog + (possibly nested) schema path. A leading component is the catalog when it names an
		// attached database, and otherwise the outermost schema of a nested schema path. The entry lookup below
		// navigates whatever qualification comes out of this.
		info.SetQualifiedName(BindTableName(info.GetQualifiedName()));
		auto catalog = Catalog::GetCatalogEntry(context, info.GetQualifiedName().Catalog());
		if (catalog) {
			// mark catalog as accessed
			properties.RegisterDBRead(*catalog, context);
		}
		optional_ptr<CatalogEntry> entry;
		if (info.type == CatalogType::MACRO_ENTRY || info.type == CatalogType::TABLE_MACRO_ENTRY) {
			// We also support "DROP MACRO" (instead of "DROP MACRO TABLE") for table macros
			entry = BindRoutine({info.GetQualifiedName(), info.func_parameters, info.has_func_args, !info.is_procedure,
			                     info.is_procedure, info.type, info.is_macro && !info.has_func_args},
			                    EntryUse::DROP, info.if_not_found)
			            .entry;
			if (entry && entry->type == CatalogType::TABLE_MACRO_ENTRY) {
				// Change type to table macro so future lookups get the correct one
				info.type = CatalogType::TABLE_MACRO_ENTRY;
			}
		} else {
			EntryLookupInfo entry_lookup(info.type, info.GetQualifiedName(), EntryUse::DROP);
			auto if_not_found = info.if_not_found;
			if (catalog && catalog->Compatibility() == SqlCompatibility::POSTGRES &&
			    Catalog::InRelationNamespace(info.type)) {
				if_not_found = OnEntryNotFound::RETURN_NULL;
			}
			entry = entry_retriever.GetEntry(entry_lookup, if_not_found);
		}
		if (!entry) {
			break;
		}
		if (entry->internal) {
			throw CatalogException("Cannot drop internal catalog entry %s!", entry->name);
		}
		// keep the entry's full (possibly nested) schema path so execution navigates the same schema
		info.SetQualifiedName(QualifiedName::FromCatalogSchema(
		    entry->ParentCatalog().GetName(),
		    entry->ParentSchemaPath(entry->ParentCatalog().GetCatalogTransaction(context)),
		    info.GetQualifiedName().Name()));
		if (!entry->temporary) {
			// we can only drop temporary schema entries in read-only mode
			properties.RegisterDBModify(entry->ParentCatalog(), context, DatabaseModificationType::DROP_CATALOG_ENTRY);
		}
		break;
	}
	case CatalogType::SECRET_ENTRY: {
		RequireSuperuser(CheckAs());
		//! Secrets are stored in the secret manager; they can always be dropped
		properties.requires_valid_transaction = false;
		break;
	}
	case CatalogType::TRIGGER_ENTRY:
		BindDropTrigger(info, properties);
		break;
	case CatalogType::ROLE_ENTRY:
		CheckDropRole(info);
		info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
		break;
	case CatalogType::TABLESPACE_ENTRY:
		break;
	case CatalogType::DATABASE_ENTRY:
		ClusterEntry(context, CheckAs(), CatalogType::DATABASE_ENTRY, info.GetQualifiedName().Name(), EntryUse::DROP,
		             OnEntryNotFound::RETURN_NULL);
		info.SetQualifiedName(ClusterName(context, info.GetQualifiedName()));
		break;
	case CatalogType::FOREIGN_SERVER_ENTRY: {
		auto catalog = BindCatalog(info.GetQualifiedName().Catalog());
		info.SetQualifiedName(QualifiedName(catalog, Identifier(), info.GetQualifiedName().Name()));
		auto &server_catalog = Catalog::GetCatalog(context, catalog);
		properties.RegisterDBModify(server_catalog, context, DatabaseModificationType::DROP_CATALOG_ENTRY);
		TopLevelEntry(context, CheckAs(), server_catalog, CatalogType::FOREIGN_SERVER_ENTRY,
		              info.GetQualifiedName().Name(), EntryUse::DROP, OnEntryNotFound::RETURN_NULL);
		break;
	}
	case CatalogType::SUBSCRIPTION_ENTRY: {
		auto &subscription_catalog = SubscriptionCatalog(info.GetQualifiedName().Catalog());
		auto name = info.GetQualifiedName().Name();
		info.SetQualifiedName(QualifiedName(subscription_catalog.GetName(), Identifier(), name));
		properties.RegisterDBModify(subscription_catalog, context, DatabaseModificationType::DROP_CATALOG_ENTRY);
		TopLevelEntry(context, CheckAs(), subscription_catalog, CatalogType::SUBSCRIPTION_ENTRY, name, EntryUse::DROP,
		              info.if_not_found);
		break;
	}
	default:
		throw BinderException("Unknown catalog type for drop statement: '%s'", CatalogTypeToString(info.type));
	}
}

BoundStatement Binder::Bind(DropStatement &stmt) {
	BoundStatement result;

	auto &properties = GetStatementProperties();
	for (auto &info : stmt.infos) {
		BindDrop(*info, properties);
	}
	result.plan = make_uniq<LogicalDrop>(std::move(stmt.infos));
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	properties.result_eagerness = ResultEagerness::FORCED;
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
