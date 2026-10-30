//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_versions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/transaction/transaction_data.hpp"

#include <functional>

namespace duckdb {

class CatalogVersions {
public:
	optional_ptr<CatalogEntry> GetVisible(const SnapshotView &view) const;
	optional_ptr<CatalogEntry> GetCommitted() const;
	optional_ptr<CatalogEntry> GetLatestCommitted() const;
	bool HasPending() const;
	bool IsEmpty() const;

	void Install(CatalogEntry &version);
	void Commit(CatalogEntry &version);
	void RevertCommit(CatalogEntry &version);
	void Rollback(CatalogEntry &version);
	void Unlink(CatalogEntry &version);

private:
	atomic<CatalogEntry *> committed {nullptr};
	atomic<CatalogEntry *> pending {nullptr};
	atomic<transaction_t> pending_transaction {MAX_TRANSACTION_ID};
};

class CatalogOidIndex {
public:
	static bool IsIndexed(const CatalogEntry &entry);

	shared_ptr<CatalogVersions> Find(idx_t oid) const;
	optional_ptr<CatalogEntry> GetVisible(idx_t oid, const SnapshotView &view) const;
	optional_ptr<CatalogEntry> GetVisibleOwner(idx_t oid, const SnapshotView &view) const;
	optional_ptr<CatalogEntry> GetCommitted(idx_t oid) const;
	optional_ptr<CatalogEntry> GetCommittedDrop(idx_t oid) const;
	void ScanVisible(const SnapshotView &view, const std::function<void(CatalogEntry &)> &callback) const;

	void Install(CatalogEntry &version, const CatalogEntry &object);
	void Commit(CatalogEntry &version, const CatalogEntry &object);
	void RevertCommit(CatalogEntry &version, const CatalogEntry &object);
	void Rollback(CatalogEntry &version, const CatalogEntry &object);
	void Unlink(CatalogEntry &version);

private:
	shared_ptr<CatalogVersions> GetOrCreate(const CatalogEntry &object);
	void AddAliases(const CatalogEntry &version, const shared_ptr<CatalogVersions> &object_versions);
	void Erase(unordered_map<idx_t, shared_ptr<CatalogVersions>>::iterator it) ABSL_EXCLUSIVE_LOCKS_REQUIRED(lock);

private:
	mutable absl::Mutex lock;
	unordered_map<idx_t, shared_ptr<CatalogVersions>> versions ABSL_GUARDED_BY(lock);
	unordered_map<idx_t, vector<idx_t>> aliases ABSL_GUARDED_BY(lock);
};

} // namespace duckdb
