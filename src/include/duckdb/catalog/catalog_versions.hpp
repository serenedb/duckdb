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
#include "duckdb/transaction/transaction_data.hpp"

namespace duckdb {

class CatalogVersions {
public:
	optional_ptr<CatalogEntry> GetVisible(const SnapshotView &view) const;
	optional_ptr<CatalogEntry> GetCommitted() const;
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
	optional_ptr<CatalogEntry> GetCommitted(idx_t oid) const;

	void Install(CatalogEntry &version, const CatalogEntry &object);
	void Commit(CatalogEntry &version, const CatalogEntry &object);
	void RevertCommit(CatalogEntry &version, const CatalogEntry &object);
	void Rollback(CatalogEntry &version, const CatalogEntry &object);
	void Unlink(CatalogEntry &version);

private:
	shared_ptr<CatalogVersions> GetOrCreate(const CatalogEntry &object);

private:
	mutable absl::Mutex lock;
	unordered_map<idx_t, shared_ptr<CatalogVersions>> versions ABSL_GUARDED_BY(lock);
};

} // namespace duckdb
