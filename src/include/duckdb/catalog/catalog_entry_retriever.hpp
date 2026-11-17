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
	CatalogEntryRetriever(ClientContext &context, const ActingRoles &acting) : context(context), acting(&acting) {
	}
	CatalogEntryRetriever(const CatalogEntryRetriever &other)
	    : callback(other.callback), context(other.context), acting(other.acting) {
	}

public:
	static CatalogEntryRetriever Internal(ClientContext &context) {
		static const ActingRoles system {&RoleAccess::System(), &RoleAccess::System(), &RoleAccess::System()};
		return CatalogEntryRetriever(context, system);
	}
	void Inherit(const CatalogEntryRetriever &parent);
	ClientContext &GetContext() {
		return context;
	}

	//! Look up an entry described by the (catalog/schema-qualified) EntryLookupInfo
	optional_ptr<CatalogEntry> GetEntry(const EntryLookupInfo &lookup_info,
	                                    OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	template <class T>
	T &GetEntry(const QualifiedName &name, EntryUse use = EntryUse::REFERENCE) {
		auto entry = GetEntry(EntryLookupInfo(T::Type, name), OnEntryNotFound::THROW_EXCEPTION);
		if (entry->type != T::Type) {
			throw CatalogException("%s is not an %s", name.Name().GetIdentifierName(), T::Name);
		}
		CheckUse(*entry, use);
		return entry->template Cast<T>();
	}
	//! Look up a (catalog/schema-qualified) type
	LogicalType GetType(const QualifiedName &name, OnEntryNotFound on_entry_not_found = OnEntryNotFound::RETURN_NULL);
	//! Look up the schema described by the (catalog-qualified) EntryLookupInfo
	optional_ptr<SchemaCatalogEntry> GetSchema(const EntryLookupInfo &schema_lookup,
	                                           OnEntryNotFound on_entry_not_found = OnEntryNotFound::THROW_EXCEPTION);
	optional_ptr<SchemaCatalogEntry> GetSchema(const Identifier &catalog, const vector<Identifier> &schema_path,
	                                           EntryUse use, OnEntryNotFound on_entry_not_found);

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
	//! A table's expressions store names, so they are bound with a path that doesn't depend on the session:
	//! a temporary object, a search_path or an object created later can't change what a stored name means.
	//! TODO(mkornaukhov): store OIDs in table expressions instead of names.
	void SetTableSearchPath(const SchemaCatalogEntry &schema);
	void SetSearchPath(vector<CatalogSearchEntry> entries, const Identifier &view_catalog,
	                   const vector<NameBinding> &bindings);
	void ReplaceSearchPath(vector<CatalogSearchEntry> entries);
	void SwapSearchPath(CatalogEntryRetriever &other);
	void RecordBindings(const Identifier &view_catalog, vector<NameBinding> &bindings);

	void SetCallback(catalog_entry_callback_t callback);
	catalog_entry_callback_t GetCallback();

	optional_ptr<BoundAtClause> GetAtClause() const;
	void SetAtClause(optional_ptr<BoundAtClause> at_clause);

	DUCKDB_API const RoleAccess &CheckAs() const;

private:
	optional_ptr<CatalogEntry> ReturnAndCallback(optional_ptr<CatalogEntry> result);
	optional_ptr<CatalogEntry> GetBoundEntry(const EntryLookupInfo &lookup_info);
	void RecordBinding(const EntryLookupInfo &lookup_info, CatalogEntry &entry);
	void CheckUse(CatalogEntry &entry, EntryUse use) {
		if (use != EntryUse::REFERENCE) {
			RequireUse(context, CheckAs(), entry, use);
		}
	}

private:
	//! (optional) callback, called on every successful entry retrieval
	catalog_entry_callback_t callback = nullptr;
	ClientContext &context;
	shared_ptr<CatalogSearchPath> search_path;
	optional_ptr<BoundAtClause> at_clause;
	optional_ptr<const ActingRoles> acting;
	optional_ptr<const vector<NameBinding>> bindings;
	optional_ptr<vector<NameBinding>> recorded_bindings;
	Identifier bindings_catalog;
};

} // namespace duckdb
