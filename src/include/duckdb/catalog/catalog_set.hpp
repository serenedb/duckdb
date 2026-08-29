//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/catalog/similar_catalog_entry.hpp"
#include <functional>
#include <memory>

namespace duckdb {
struct AlterInfo;
struct ChangeOwnershipInfo;

class ClientContext;
class LogicalDependencyList;

class DuckCatalog;
class SchemaIdentity;
class TableCatalogEntry;
class SequenceCatalogEntry;

class CatalogEntryMap {
public:
	explicit CatalogEntryMap(bool case_sensitive = false) : entries(IdentifierCompare(case_sensitive)) {
	}

public:
	void AddEntry(unique_ptr<CatalogEntry> entry);
	void UpdateEntry(unique_ptr<CatalogEntry> entry);
	void DropEntry(CatalogEntry &entry);
	identifier_tree_t<unique_ptr<CatalogEntry>> &Entries();
	optional_ptr<CatalogEntry> GetEntry(const Identifier &name);
	bool IsCaseSensitive() const {
		return entries.key_comp().case_sensitive;
	}

private:
	//! Mapping of identifier to catalog entry
	identifier_tree_t<unique_ptr<CatalogEntry>> entries;
};

//! The Catalog Set stores (key, value) map of a set of CatalogEntries
class CatalogSet {
public:
	struct EntryLookup {
		enum class FailureReason { SUCCESS, DELETED, NOT_PRESENT, INVISIBLE };
		optional_ptr<CatalogEntry> result;
		FailureReason reason;
	};

public:
	//! `case_sensitive` keys the set by the exact name rather than by duckdb's case-insensitive identifier
	//! comparison, for a catalog that folds identifiers by its own rules before they get here.
	DUCKDB_API explicit CatalogSet(Catalog &catalog, unique_ptr<DefaultGenerator> defaults = nullptr,
	                               bool case_sensitive = false);
	~CatalogSet();

	//! Create an entry in the catalog set. Returns whether or not it was
	//! successful.
	DUCKDB_API bool CreateEntry(CatalogTransaction transaction, const Identifier &name, unique_ptr<CatalogEntry> value,
	                            const LogicalDependencyList &dependencies);
	DUCKDB_API bool CreateEntry(ClientContext &context, const Identifier &name, unique_ptr<CatalogEntry> value,
	                            const LogicalDependencyList &dependencies);
	//! Creates `value` under its own name, or -- when an entry named `replaces` exists -- installs it as an
	//! alter of that entry: a replace hands the object's edges over and takes the rename path, rather than
	//! leaving a drop tombstone that VerifyCommitDrop would refuse. Returns whether the set took the write.
	DUCKDB_API bool CreateOrReplaceEntry(CatalogTransaction transaction, const Identifier &replaces,
	                                     unique_ptr<CatalogEntry> value, const LogicalDependencyList &dependencies);
	//! Whether the committed version `transaction` resolves under `name` has since been dropped by a
	//! concurrent commit: the transaction still reads it, a committed read no longer finds it. Writing a
	//! new version over such an entry would silently resurrect it.
	DUCKDB_API bool CommittedVersionVanished(CatalogTransaction transaction, const Identifier &name);

	DUCKDB_API bool AlterEntry(CatalogTransaction transaction, const Identifier &name, AlterInfo &alter_info);
	//! Alter with the replacement entry supplied by the caller, for a catalog that computes the new version
	//! itself rather than deriving it from the AlterInfo. A null `value` falls back to asking the entry.
	DUCKDB_API bool AlterEntry(CatalogTransaction transaction, const Identifier &name, AlterInfo &alter_info,
	                           unique_ptr<CatalogEntry> value);

	DUCKDB_API bool DropEntry(CatalogTransaction transaction, const Identifier &name, bool cascade,
	                          bool allow_drop_internal = false);
	DUCKDB_API bool DropEntry(ClientContext &context, const Identifier &name, bool cascade,
	                          bool allow_drop_internal = false);
	//! Verify that the entry referenced by the dependency is still alive
	DUCKDB_API void VerifyExistenceOfDependency(transaction_t commit_id, CatalogEntry &entry);
	//! Verify we can still drop the entry while committing
	DUCKDB_API void CommitDrop(transaction_t commit_id, transaction_t start_time, CatalogEntry &entry);

	DUCKDB_API DuckCatalog &GetCatalog();

	bool AlterOwnership(CatalogTransaction transaction, ChangeOwnershipInfo &info);

	void CleanupEntry(CatalogEntry &catalog_entry);

	//! Returns the entry with the specified name
	DUCKDB_API EntryLookup GetEntryDetailed(CatalogTransaction transaction, const Identifier &name);
	DUCKDB_API optional_ptr<CatalogEntry> GetEntry(CatalogTransaction transaction, const Identifier &name);
	DUCKDB_API optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const Identifier &name);

	//! Gets the entry that is most similar to the given name (i.e. smallest levenshtein distance), or empty string if
	//! none is found. The returned pair consists of the entry name and the distance (smaller means closer).
	SimilarCatalogEntry SimilarEntry(CatalogTransaction transaction, const Identifier &name);

	//! Rollback <entry> to be the currently valid entry for a certain catalog
	//! entry
	void Undo(CatalogEntry &entry);

	//! Scan the catalog set, invoking the callback method for every committed entry
	DUCKDB_API void Scan(const std::function<void(CatalogEntry &)> &callback);
	//! Scan the catalog set, invoking the callback method for every entry
	DUCKDB_API void ScanWithPrefix(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback,
	                               const Identifier &prefix);
	DUCKDB_API void Scan(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback);
	DUCKDB_API void ScanWithReturn(CatalogTransaction transaction, const std::function<bool(CatalogEntry &)> &callback);
	DUCKDB_API void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	DUCKDB_API void ScanWithReturn(ClientContext &context, const std::function<bool(CatalogEntry &)> &callback);

	template <class T>
	vector<reference<T>> GetEntries(CatalogTransaction transaction) {
		vector<reference<T>> result;
		Scan(transaction, [&](CatalogEntry &entry) { result.push_back(entry.Cast<T>()); });
		return result;
	}

	DUCKDB_API bool CreatedByOtherActiveTransaction(CatalogTransaction transaction, transaction_t timestamp);
	DUCKDB_API bool CommittedAfterStarting(CatalogTransaction transaction, transaction_t timestamp);
	DUCKDB_API bool HasConflict(CatalogTransaction transaction, transaction_t timestamp);
	DUCKDB_API bool UseTimestamp(CatalogTransaction transaction, transaction_t timestamp);
	static bool IsCommitted(transaction_t timestamp);

	static void UpdateTimestamp(CatalogEntry &entry, transaction_t timestamp);

	mutex &GetCatalogLock() {
		return catalog_lock;
	}

	void Verify(Catalog &catalog);

	//! Override the default generator - this should not be used after the catalog set has been used
	void SetDefaultGenerator(unique_ptr<DefaultGenerator> defaults);

	//! File every entry placed in this set in the catalog's by-id map, under `slot` inside the schema `owner`
	//! currently heads -- or at the catalog root when there is no owner. A set that never enables this stays
	//! out of by-id lookups.
	void EnableOidLookup(optional_ptr<SchemaIdentity> owner, CatalogType slot);

private:
	bool DropDependencies(CatalogTransaction transaction, const Identifier &name, bool cascade,
	                      bool allow_drop_internal = false);
	//! Given a root entry, gets the entry valid for this transaction, 'visible' is used to indicate whether the entry
	//! is actually visible to the transaction
	CatalogEntry &GetEntryForTransaction(CatalogTransaction transaction, CatalogEntry &current, bool &visible);
	//! Given a root entry, gets the entry valid for this transaction
	CatalogEntry &GetEntryForTransaction(CatalogTransaction transaction, CatalogEntry &current);
	CatalogEntry &GetCommittedEntry(CatalogEntry &current);
	optional_ptr<CatalogEntry> GetEntryInternal(CatalogTransaction transaction, const Identifier &name);
	optional_ptr<CatalogEntry> CreateCommittedEntry(unique_ptr<CatalogEntry> entry);

	//! Create all default entries
	void CreateDefaultEntries(CatalogTransaction transaction, unique_lock<mutex> &lock);
	//! Attempt to create a default entry with the specified name. Returns the entry if successful, nullptr otherwise.
	optional_ptr<CatalogEntry> CreateDefaultEntry(CatalogTransaction transaction, const Identifier &name,
	                                              unique_lock<mutex> &lock);

	bool DropEntryInternal(CatalogTransaction transaction, const Identifier &name, bool allow_drop_internal = false);
	void ClearLocalStorage(CatalogTransaction transaction, const Identifier &name);

	bool CreateEntryInternal(CatalogTransaction transaction, const Identifier &name, unique_ptr<CatalogEntry> value,
	                         unique_lock<mutex> &read_lock, bool should_be_empty = true);
	void AddOidLocation(CatalogEntry &entry);
	void RemoveOidLocation(CatalogEntry &entry);
	void CheckCatalogEntryInvariants(CatalogEntry &value, const Identifier &name);
	//! Verify that the previous entry in the chain is dropped.
	bool VerifyVacancy(CatalogTransaction transaction, CatalogEntry &entry);
	//! Start the catalog entry chain with a dummy node
	bool StartChain(CatalogTransaction transaction, const Identifier &name, unique_lock<mutex> &read_lock);
	bool RenameEntryInternal(CatalogTransaction transaction, CatalogEntry &old, const Identifier &new_name,
	                         AlterInfo &alter_info, unique_lock<mutex> &read_lock);

private:
	DuckCatalog &catalog;
	//! The catalog lock is used to make changes to the data
	mutex catalog_lock;
	CatalogEntryMap map;
	//! The generator used to generate default internal entries
	unique_ptr<DefaultGenerator> defaults;
	//! Where a by-id lookup files entries placed here; INVALID keeps the set out of the by-id map
	optional_ptr<SchemaIdentity> oid_owner;
	CatalogType oid_slot = CatalogType::INVALID;
	//! The owning schema's oid, cached at the first placement (constant across schema versions)
	idx_t oid_schema = DConstants::INVALID_INDEX;
};
} // namespace duckdb
