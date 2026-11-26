#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"

#include <absl/algorithm/container.h>

namespace duckdb {

static bool SameBinding(const NameBinding &binding, const EntryLookupInfo &lookup_info) {
	return binding.type == lookup_info.GetCatalogType() && binding.argument_count == lookup_info.GetArgumentCount() &&
	       binding.name == lookup_info.GetEntryIdentifier();
}

optional_ptr<CatalogEntry> CatalogEntryRetriever::GetBoundEntry(const EntryLookupInfo &lookup_info) {
	auto binding =
	    absl::c_find_if(*bindings, [&](const NameBinding &candidate) { return SameBinding(candidate, lookup_info); });
	if (binding == bindings->end()) {
		return nullptr;
	}
	auto &catalog = binding->catalog.empty() ? bindings_catalog : binding->catalog;
	return Catalog::GetEntry(
	    *this,
	    EntryLookupInfo(lookup_info, QualifiedName::FromCatalogSchema(catalog, binding->schema_path, binding->name)),
	    OnEntryNotFound::RETURN_NULL);
}

void CatalogEntryRetriever::RecordBinding(const EntryLookupInfo &lookup_info, CatalogEntry &entry) {
	auto &catalog = entry.ParentCatalog();
	if (catalog.IsSystemCatalog() || catalog.IsTemporaryCatalog()) {
		return;
	}
	if (absl::c_any_of(*recorded_bindings,
	                   [&](const NameBinding &binding) { return SameBinding(binding, lookup_info); })) {
		return;
	}
	recorded_bindings->push_back(NameBinding {lookup_info.GetCatalogType(), lookup_info.GetEntryIdentifier(),
	                                          lookup_info.GetArgumentCount(),
	                                          catalog.GetName() == bindings_catalog ? Identifier() : catalog.GetName(),
	                                          DependencyManager::GetSchemaPath(entry)});
}

optional_ptr<CatalogEntry> CatalogEntryRetriever::GetEntry(const EntryLookupInfo &lookup_info,
                                                           OnEntryNotFound on_entry_not_found) {
	optional_ptr<CatalogEntry> entry;
	if ((bindings || recorded_bindings) && IsInvalidCatalog(lookup_info.GetCatalog()) &&
	    IsInvalidSchema(lookup_info.GetSchema())) {
		entry = bindings ? GetBoundEntry(lookup_info) : nullptr;
		if (!entry) {
			entry = Catalog::GetEntry(*this, lookup_info, on_entry_not_found);
		}
		if (entry && recorded_bindings) {
			RecordBinding(lookup_info, *entry);
		}
	} else {
		entry = Catalog::GetEntry(*this, lookup_info, on_entry_not_found);
	}
	if (entry) {
		CheckUse(*entry, lookup_info.GetUse());
	}
	return ReturnAndCallback(entry);
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
	CheckUse(*result, schema_lookup.GetUse());
	if (callback) {
		// Call the callback if it's set
		callback(*result);
	}
	return result;
}

optional_ptr<SchemaCatalogEntry> CatalogEntryRetriever::GetSchema(const Identifier &catalog,
                                                                  const vector<Identifier> &schema_path, AclMode use,
                                                                  OnEntryNotFound on_entry_not_found) {
	auto result = Catalog::GetSchema(*this, catalog, schema_path, on_entry_not_found);
	if (!result) {
		return result;
	}
	CheckUse(*result, use);
	if (callback) {
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

const RoleAccess &CatalogEntryRetriever::CheckAs() const {
	return acting && acting->lookups ? *acting->lookups : context.CurrentRole();
}

void CatalogEntryRetriever::CheckUse(CatalogEntry &entry, AclMode use) {
	if (use != AclMode::NoRights) {
		Require(context, acting && acting->privileges ? *acting->privileges : context.CurrentRole(), entry, use);
	}
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
	this->function_search_path = parent.function_search_path;
	this->at_clause = parent.at_clause;
	this->bindings = parent.bindings;
	this->recorded_bindings = parent.recorded_bindings;
	this->bindings_catalog = parent.bindings_catalog;
}

const CatalogSearchPath &CatalogEntryRetriever::GetSearchPath() const {
	if (search_path) {
		return *search_path;
	}
	return *ClientData::Get(context).catalog_search_path;
}

static vector<CatalogSearchEntry> SearchEntriesOfCatalogs(vector<CatalogSearchEntry> entries) {
	vector<CatalogSearchEntry> new_path;
	auto &db_manager = DatabaseManager::Get(context);
	for (auto &entry : entries) {
		if (IsInvalidCatalog(entry.GetCatalog()) || db_manager.IsReservedName(entry.GetCatalog())) {
			continue;
		}
		new_path.push_back(std::move(entry));
	}
	return new_path;
}

void CatalogEntryRetriever::SetSearchPath(vector<CatalogSearchEntry> entries, const Identifier &view_catalog,
                                          const vector<NameBinding> &bindings_p) {
	search_path = make_shared_ptr<CatalogSearchPath>(context, SearchEntriesOfCatalogs(std::move(entries)), false);
	bindings = &bindings_p;
	recorded_bindings = nullptr;
	bindings_catalog = view_catalog;
}

void CatalogEntryRetriever::ReplaceSearchPath(vector<CatalogSearchEntry> entries) {
	search_path = make_shared_ptr<CatalogSearchPath>(context, std::move(entries));
	function_search_path = search_path;
	bindings = nullptr;
	recorded_bindings = nullptr;
}

void CatalogEntryRetriever::SwapSearchPath(CatalogEntryRetriever &other) {
	std::swap(search_path, other.search_path);
	std::swap(function_search_path, other.function_search_path);
	std::swap(bindings, other.bindings);
	std::swap(recorded_bindings, other.recorded_bindings);
	std::swap(bindings_catalog, other.bindings_catalog);
}

void CatalogEntryRetriever::RecordBindings(const Identifier &view_catalog, vector<NameBinding> &bindings_p) {
	recorded_bindings = &bindings_p;
	bindings_catalog = view_catalog;
}

void CatalogEntryRetriever::SetSearchPath(vector<CatalogSearchEntry> entries) {
	bindings = nullptr;
	recorded_bindings = nullptr;
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

void CatalogEntryRetriever::SetTableSearchPath(const SchemaCatalogEntry &schema) {
	auto &catalog = schema.ParentCatalog();
	if (catalog.Compatibility() != SqlCompatibility::POSTGRES || catalog.IsTemporaryCatalog()) {
		SetSearchPath({CatalogSearchEntry(catalog.GetName(), schema.name)});
		return;
	}
	D_ASSERT(!catalog.IsSystemCatalog());
	this->search_path = CatalogSearchPath::ForTable(context, schema);
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
