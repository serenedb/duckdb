//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry_retriever.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/catalog/permissions.hpp"

namespace duckdb {

class ClientContext;
class Catalog;
class CatalogEntry;

using catalog_entry_callback_t = std::function<void(CatalogEntry &)>;

// Wraps the Catalog::GetEntry method
class CatalogEntryRetriever {
public:
	explicit CatalogEntryRetriever(ClientContext &context) : context(context) {
	}
	CatalogEntryRetriever(const CatalogEntryRetriever &other)
	    : callback(other.callback), context(other.context), principal(other.principal) {
	}

public:
	void Inherit(const CatalogEntryRetriever &parent);
	ClientContext &GetContext() {
		return context;
	}

	//! Look up an entry described by the (catalog/schema-qualified) EntryLookupInfo
	optional_ptr<CatalogEntry> GetEntry(const EntryLookupInfo &lookup_info,
	                                    OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	template <class T>
	T &GetEntry(const QualifiedName &name) {
		auto entry = GetEntry(EntryLookupInfo(T::Type, name), OnEntryNotFound::THROW_EXCEPTION);
		if (entry->type != T::Type) {
			throw CatalogException("%s is not an %s", name.Name().GetIdentifierName(), T::Name);
		}
		return entry->template Cast<T>();
	}
	//! Look up a (catalog/schema-qualified) type
	LogicalType GetType(const QualifiedName &name, OnEntryNotFound on_entry_not_found = OnEntryNotFound::RETURN_NULL);
	//! Look up the schema described by the (catalog-qualified) EntryLookupInfo
	optional_ptr<SchemaCatalogEntry> GetSchema(const EntryLookupInfo &schema_lookup,
	                                           OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);

	//! Deprecated: the catalog/schema qualification is now carried inside the EntryLookupInfo / QualifiedName - fold it
	//! in there instead of passing it separately.
	[[deprecated(
	    "Fold catalog/schema into the EntryLookupInfo and use GetEntry(EntryLookupInfo)")]] optional_ptr<CatalogEntry>
	GetEntry(const Identifier &catalog, const Identifier &schema, const EntryLookupInfo &lookup_info,
	         OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	[[deprecated(
	    "Fold the schema into the EntryLookupInfo and use GetEntry(EntryLookupInfo)")]] optional_ptr<CatalogEntry>
	GetEntry(Catalog &catalog, const Identifier &schema, const EntryLookupInfo &lookup_info,
	         OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	[[deprecated("Fold catalog/schema into the QualifiedName and use GetType(QualifiedName)")]] LogicalType
	GetType(const Identifier &catalog, const Identifier &schema, const Identifier &name,
	        OnEntryNotFound on_entry_not_found = OnEntryNotFound::RETURN_NULL);
	[[deprecated("Fold the schema into the QualifiedName and use GetType(QualifiedName)")]] LogicalType
	GetType(Catalog &catalog, const Identifier &schema, const Identifier &name,
	        OnEntryNotFound on_entry_not_found = OnEntryNotFound::RETURN_NULL);
	[[deprecated("Fold the catalog into the EntryLookupInfo and use GetSchema(EntryLookupInfo)")]] optional_ptr<
	    SchemaCatalogEntry>
	GetSchema(const Identifier &catalog, const EntryLookupInfo &schema_lookup,
	          OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);

	const CatalogSearchPath &GetSearchPath() const;
	void SetSearchPath(vector<CatalogSearchEntry> entries);
	void SetSearchPath(vector<CatalogSearchEntry> entries, const Identifier &view_catalog,
	                   const vector<NameBinding> &bindings);
	void ReplaceSearchPath(vector<CatalogSearchEntry> entries);
	void SwapSearchPath(CatalogEntryRetriever &other);
	void RecordBindings(const Identifier &view_catalog, vector<NameBinding> &bindings);

	void SetCallback(catalog_entry_callback_t callback);
	catalog_entry_callback_t GetCallback();

	optional_ptr<BoundAtClause> GetAtClause() const;
	void SetAtClause(optional_ptr<BoundAtClause> at_clause);

	optional_ptr<const RoleAccess> GetPrincipal() const {
		return principal;
	}
	void SetPrincipal(optional_ptr<const RoleAccess> principal_p) {
		principal = principal_p;
	}

private:
	optional_ptr<CatalogEntry> ReturnAndCallback(optional_ptr<CatalogEntry> result);
	optional_ptr<CatalogEntry> GetBoundEntry(const EntryLookupInfo &lookup_info);
	void RecordBinding(const EntryLookupInfo &lookup_info, CatalogEntry &entry);

private:
	//! (optional) callback, called on every successful entry retrieval
	catalog_entry_callback_t callback = nullptr;
	ClientContext &context;
	shared_ptr<CatalogSearchPath> search_path;
	optional_ptr<BoundAtClause> at_clause;
	optional_ptr<const RoleAccess> principal;
	optional_ptr<const vector<NameBinding>> bindings;
	optional_ptr<vector<NameBinding>> recorded_bindings;
	Identifier bindings_catalog;
};

} // namespace duckdb
