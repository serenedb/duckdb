#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

enum class EntryNamespace : uint8_t { NONE, RELATION, FUNCTION, TYPE };

static EntryNamespace NamespaceOf(CatalogType type) {
	if (Catalog::InRelationNamespace(type)) {
		return EntryNamespace::RELATION;
	}
	switch (type) {
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
	case CatalogType::COPY_FUNCTION_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::WINDOW_FUNCTION_ENTRY:
		return EntryNamespace::FUNCTION;
	case CatalogType::TYPE_ENTRY:
		return EntryNamespace::TYPE;
	default:
		return EntryNamespace::NONE;
	}
}

optional_ptr<CatalogEntry> CatalogEntryRetriever::GetBoundEntry(const EntryLookupInfo &lookup_info) {
	const auto name_space = NamespaceOf(lookup_info.GetCatalogType());
	if (name_space == EntryNamespace::NONE) {
		return nullptr;
	}
	for (auto &dependency : bindings->Set()) {
		if (dependency.entry.name != lookup_info.GetEntryIdentifier() ||
		    NamespaceOf(dependency.entry.type) != name_space) {
			continue;
		}
		vector<Identifier> path {dependency.catalog};
		path.insert(path.end(), dependency.entry.schema_path.begin(), dependency.entry.schema_path.end());
		auto entry = Catalog::GetEntry(
		    *this, EntryLookupInfo(lookup_info, QualifiedName(std::move(path), dependency.entry.name)),
		    OnEntryNotFound::RETURN_NULL);
		if (entry) {
			return entry;
		}
	}
	return nullptr;
}

optional_ptr<CatalogEntry> CatalogEntryRetriever::GetEntry(const EntryLookupInfo &lookup_info,
                                                           OnEntryNotFound on_entry_not_found) {
	if (bindings && IsInvalidCatalog(lookup_info.GetCatalog()) && IsInvalidSchema(lookup_info.GetSchema())) {
		auto entry = GetBoundEntry(lookup_info);
		if (entry) {
			return ReturnAndCallback(entry);
		}
	}
	return ReturnAndCallback(Catalog::GetEntry(*this, lookup_info, on_entry_not_found));
}

LogicalType CatalogEntryRetriever::GetType(const QualifiedName &name, OnEntryNotFound on_entry_not_found) {
	EntryLookupInfo lookup_info(CatalogType::TYPE_ENTRY, name);
	auto result = GetEntry(lookup_info, on_entry_not_found);
	if (!result) {
		return LogicalType::INVALID;
	}
	auto &type_entry = result->Cast<TypeCatalogEntry>();
	return type_entry.user_type;
}

optional_ptr<SchemaCatalogEntry> CatalogEntryRetriever::GetSchema(const EntryLookupInfo &schema_lookup_p,
                                                                  OnEntryNotFound on_entry_not_found) {
	EntryLookupInfo schema_lookup(schema_lookup_p, at_clause);
	auto result = Catalog::GetSchema(*this, schema_lookup, on_entry_not_found);
	if (!result) {
		return result;
	}
	if (callback) {
		// Call the callback if it's set
		callback(*result);
	}
	return result;
}

LogicalType CatalogEntryRetriever::GetType(Catalog &catalog, const Identifier &schema, const Identifier &name,
                                           OnEntryNotFound on_entry_not_found) {
	return GetType(QualifiedName(catalog.GetName(), schema, name), on_entry_not_found);
}

LogicalType CatalogEntryRetriever::GetType(const Identifier &catalog, const Identifier &schema, const Identifier &name,
                                           OnEntryNotFound on_entry_not_found) {
	return GetType(QualifiedName(catalog, schema, name), on_entry_not_found);
}

optional_ptr<CatalogEntry> CatalogEntryRetriever::GetEntry(const Identifier &catalog, const Identifier &schema,
                                                           const EntryLookupInfo &lookup_info,
                                                           OnEntryNotFound on_entry_not_found) {
	return GetEntry(EntryLookupInfo(lookup_info, QualifiedName(catalog, schema, lookup_info.GetEntryIdentifier())),
	                on_entry_not_found);
}

optional_ptr<CatalogEntry> CatalogEntryRetriever::GetEntry(Catalog &catalog, const Identifier &schema,
                                                           const EntryLookupInfo &lookup_info,
                                                           OnEntryNotFound on_entry_not_found) {
	return GetEntry(
	    EntryLookupInfo(lookup_info, QualifiedName(catalog.GetName(), schema, lookup_info.GetEntryIdentifier())),
	    on_entry_not_found);
}

optional_ptr<SchemaCatalogEntry> CatalogEntryRetriever::GetSchema(const Identifier &catalog,
                                                                  const EntryLookupInfo &schema_lookup,
                                                                  OnEntryNotFound on_entry_not_found) {
	return GetSchema(
	    EntryLookupInfo(schema_lookup, QualifiedName(catalog, Identifier(), schema_lookup.GetEntryIdentifier())),
	    on_entry_not_found);
}

optional_ptr<CatalogEntry> CatalogEntryRetriever::ReturnAndCallback(optional_ptr<CatalogEntry> result) {
	if (!result) {
		return result;
	}
	if (callback) {
		// Call the callback if it's set
		callback(*result);
	}
	return result;
}

void CatalogEntryRetriever::Inherit(const CatalogEntryRetriever &parent) {
	this->callback = parent.callback;
	this->search_path = parent.search_path;
	this->at_clause = parent.at_clause;
	this->principal = parent.principal;
	this->bindings = parent.bindings;
}

const CatalogSearchPath &CatalogEntryRetriever::GetSearchPath() const {
	if (search_path) {
		return *search_path;
	}
	return *ClientData::Get(context).catalog_search_path;
}

static vector<CatalogSearchEntry> SearchEntriesOfCatalogs(vector<CatalogSearchEntry> entries) {
	vector<CatalogSearchEntry> new_path;
	for (auto &entry : entries) {
		if (IsInvalidCatalog(entry.GetCatalog()) || entry.GetCatalog() == SYSTEM_CATALOG ||
		    entry.GetCatalog() == TEMP_CATALOG) {
			continue;
		}
		new_path.push_back(std::move(entry));
	}
	return new_path;
}

void CatalogEntryRetriever::SetSearchPath(vector<CatalogSearchEntry> entries, const LogicalDependencyList &bindings_p) {
	search_path = make_shared_ptr<CatalogSearchPath>(context, SearchEntriesOfCatalogs(std::move(entries)), false);
	bindings = &bindings_p;
}

void CatalogEntryRetriever::SetSearchPath(vector<CatalogSearchEntry> entries) {
	bindings = nullptr;
	auto new_path = SearchEntriesOfCatalogs(std::move(entries));
	if (new_path.empty()) {
		return;
	}

	// push the set paths from the ClientContext behind the provided paths
	auto &client_search_path = *ClientData::Get(context).catalog_search_path;
	auto &set_paths = client_search_path.GetSetPaths();
	for (auto path : set_paths) {
		if (IsInvalidCatalog(path.GetCatalog())) {
			path.SetCatalog(DatabaseManager::TryGetDefaultDatabase(context));
			if (IsInvalidCatalog(path.GetCatalog())) {
				continue;
			}
		}
		new_path.push_back(std::move(path));
	}

	this->search_path = make_shared_ptr<CatalogSearchPath>(context, std::move(new_path));
}

optional_ptr<BoundAtClause> CatalogEntryRetriever::GetAtClause() const {
	return at_clause;
}

void CatalogEntryRetriever::SetAtClause(optional_ptr<BoundAtClause> at_clause_p) {
	at_clause = at_clause_p;
}

void CatalogEntryRetriever::SetCallback(catalog_entry_callback_t callback) {
	this->callback = std::move(callback);
}

catalog_entry_callback_t CatalogEntryRetriever::GetCallback() {
	return callback;
}

} // namespace duckdb
