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
#include "duckdb/transaction/transaction_data.hpp"

namespace duckdb {

class CatalogVersions {
public:
	optional_ptr<CatalogEntry> GetVisible(const SnapshotView &view) const;
	optional_ptr<CatalogEntry> GetCommitted() const;
	bool HasPending() const;

	void Install(CatalogEntry &version);
	void Commit(CatalogEntry &version);
	void Rollback(CatalogEntry &version);
	void Forget(CatalogEntry &version);

private:
	atomic<CatalogEntry *> committed {nullptr};
	atomic<CatalogEntry *> pending {nullptr};
	atomic<transaction_t> pending_transaction {MAX_TRANSACTION_ID};
};

} // namespace duckdb
