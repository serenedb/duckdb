#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_drop.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/string_util.hpp"
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
	auto &table_entry = entry_retriever.GetEntry<TableCatalogEntry>(base_table_ref.GetQualifiedName(), AclMode::Own);
	// the trigger lives in the same (possibly nested) schema as its base table
	auto &trigger = info.objects.front().name;
	trigger = table_entry.GetQualifiedName(trigger.Name());
	properties.RegisterDBModify(table_entry.ParentCatalog(), context, DatabaseModificationType::DROP_CATALOG_ENTRY);
}

static string RelationKind(CatalogType type) {
	switch (type) {
	case CatalogType::VIEW_ENTRY:
		return "view";
	case CatalogType::INDEX_ENTRY:
		return "index";
	case CatalogType::SEQUENCE_ENTRY:
		return "sequence";
	default:
		return "table";
	}
}

static string WithArticle(const string &kind) {
	return (kind == "index" ? "an " : "a ") + kind;
}

static void RequireRelationKind(CatalogEntryRetriever &retriever, CatalogType type, const QualifiedName &name,
                                optional_ptr<CatalogEntry> entry) {
	auto holder = entry;
	for (idx_t i = 0; !holder && i < std::size(RELATION_KINDS); i++) {
		if (RELATION_KINDS[i] != type) {
			holder = retriever.GetEntry(EntryLookupInfo(RELATION_KINDS[i], name), OnEntryNotFound::RETURN_NULL);
		}
	}
	if (!holder || holder->type == type) {
		return;
	}
	auto extra_info = Exception::InitializeExtraInfo("WRONG_OBJECT_TYPE", optional_idx());
	auto actual_kind = RelationKind(holder->type);
	extra_info["hint"] =
	    StringUtil::Format("Use DROP %s to remove %s.", StringUtil::Upper(actual_kind), WithArticle(actual_kind));
	throw CatalogException(extra_info, StringUtil::Format("\"%s\" is not %s", name.Name().GetIdentifierName(),
	                                                      WithArticle(RelationKind(type))));
}

bool Binder::BindDropCatalogLevel(const DropInfo &info, const ObjectReference &object) {
	auto entry =
	    entry_retriever.GetEntry(EntryLookupInfo(info.type, object.name, AclMode::Drop), OnEntryNotFound::RETURN_NULL);
	if (entry && entry->type == CatalogType::TABLESPACE_ENTRY && entry->internal) {
		throw PermissionException("permission denied for tablespace %s", entry->name.GetIdentifierName());
	}
	if (entry || !entry_retriever.Hides(EntryLookupInfo(info.type, object.name))) {
		return true;
	}
	if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException::MissingObject(info.type, object.name.Name());
	}
	return false;
}

bool Binder::BindDrop(const DropInfo &info, ObjectReference &object, StatementProperties &properties) {
	object.type = info.type;
	switch (info.type) {
	case CatalogType::SCHEMA_ENTRY: {
		// resolve the leading component of the dotted path into a catalog, leaving the path as
		// [catalog, parent schemas..., schema] (mirrors CREATE SCHEMA - see Binder::BindCreateSchema)
		object.name = ResolveCatalog(entry_retriever, object.name);
		// dropping a schema is never read-only because there are no temporary schemas. The catalog is the leading
		// component of the resolved path ([catalog, parent schemas..., schema])
		auto &path = object.name.Path();
		auto &catalog = Catalog::GetCatalog(context, path.front());
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::DROP_CATALOG_ENTRY);
		const vector<Identifier> schema_path(path.begin() + 1, path.end());
		if (entry_retriever.GetSchema(path.front(), schema_path, AclMode::Drop, OnEntryNotFound::RETURN_NULL) ||
		    !entry_retriever.Hides(path.front(), schema_path)) {
			return true;
		}
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return false;
		}
		throw CatalogException::MissingEntry(EntryLookupInfo(info.type, object.name), string());
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
		object.name = BindTableName(object.name);
		auto catalog = Catalog::GetCatalogEntry(context, object.name.Catalog());
		if (catalog) {
			// mark catalog as accessed
			properties.RegisterDBRead(*catalog, context);
		}
		optional_ptr<CatalogEntry> entry;
		if (info.type == CatalogType::MACRO_ENTRY || info.type == CatalogType::TABLE_MACRO_ENTRY) {
			// We also support "DROP MACRO" (instead of "DROP MACRO TABLE") for table macros
			entry = BindRoutine({object.name, object.func_parameters, object.has_func_args,
			                     info.is_routine || !info.is_procedure, info.is_routine || info.is_procedure, info.type,
			                     info.is_macro && !object.has_func_args},
			                    AclMode::Drop, info.if_not_found, true)
			            .entry;
			if (entry && entry->type == CatalogType::TABLE_MACRO_ENTRY) {
				// Change type to table macro so future lookups get the correct one
				object.type = CatalogType::TABLE_MACRO_ENTRY;
			}
		} else {
			const EntryLookupInfo lookup(info.type, object.name, AclMode::Drop);
			entry = entry_retriever.GetEntry(lookup, OnEntryNotFound::RETURN_NULL);
			if (catalog && catalog->Compatibility() == SqlCompatibility::POSTGRES &&
			    Catalog::InRelationNamespace(info.type)) {
				RequireRelationKind(entry_retriever, info.type, object.name, entry);
			}
			if (!entry && info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
				entry = entry_retriever.GetEntry(lookup, OnEntryNotFound::THROW_EXCEPTION);
			}
		}
		if (!entry) {
			return !entry_retriever.Hides(EntryLookupInfo(info.type, object.name));
		}
		if (entry->internal) {
			throw CatalogException("Cannot drop internal catalog entry %s!", entry->name);
		}
		// keep the entry's full (possibly nested) schema path so execution navigates the same schema
		object.name = QualifiedName::FromCatalogSchema(
		    entry->ParentCatalog().GetName(),
		    entry->ParentSchemaPath(entry->ParentCatalog().GetCatalogTransaction(context)), object.name.Name());
		if (!entry->temporary) {
			// we can only drop temporary schema entries in read-only mode
			properties.RegisterDBModify(entry->ParentCatalog(), context, DatabaseModificationType::DROP_CATALOG_ENTRY);
		}
		return true;
	}
	case CatalogType::ROLE_ENTRY:
		Check<StatementRule::DropRole>(CheckAs()).Require();
		BindDropRole(object.name.Name());
		return true;
	case CatalogType::FOREIGN_SERVER_ENTRY:
	case CatalogType::SUBSCRIPTION_ENTRY: {
		auto &catalog = Catalog::GetCatalog(context, BindCatalog(object.name.Catalog()));
		object.name = QualifiedName(catalog.GetName(), Identifier(), object.name.Name());
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::DROP_CATALOG_ENTRY);
		return BindDropCatalogLevel(info, object);
	}
	case CatalogType::TABLESPACE_ENTRY:
	case CatalogType::DATABASE_ENTRY:
		return BindDropCatalogLevel(info, object);
	default:
		throw BinderException("Unknown catalog type for drop statement: '%s'", CatalogTypeToString(info.type));
	}
}

BoundStatement Binder::Bind(DropStatement &stmt) {
	BoundStatement result;

	auto &properties = GetStatementProperties();
	auto &info = *stmt.info;
	switch (info.type) {
	case CatalogType::PREPARED_STATEMENT:
		// dropping prepared statements is always possible
		// it also does not require a valid transaction
		properties.requires_valid_transaction = false;
		break;
	case CatalogType::SECRET_ENTRY:
		Check<StatementRule::DropSecret>(CheckAs()).Require();
		//! Secrets are stored in the secret manager; they can always be dropped
		properties.requires_valid_transaction = false;
		break;
	case CatalogType::TRIGGER_ENTRY:
		BindDropTrigger(info, properties);
		break;
	default: {
		vector<ObjectReference> visible;
		for (auto &object : info.objects) {
			if (BindDrop(info, object, properties)) {
				visible.push_back(std::move(object));
			}
		}
		info.objects = std::move(visible);
		break;
	}
	}
	result.plan = make_uniq<LogicalDrop>(std::move(stmt.info));
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	properties.result_eagerness = ResultEagerness::FORCED;
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
