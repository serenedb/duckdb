//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/replication_lsn_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

struct ReplicationLsnState {
	explicit ReplicationLsnState(uint64_t remote_lsn) : remote_lsn(remote_lsn) {
	}

	atomic<uint64_t> remote_lsn;
	mutex relation_lock;
	//! The remote LSN each synchronized relation was copied at, by the relation's sync id
	unordered_map<idx_t, uint64_t> relation_lsns;
};

//! A catalog entry that tracks the remote LSN it has durably applied. Every version of the entry shares the LSN.
class ReplicationLsnEntry : public InCatalogEntry {
public:
	ReplicationLsnEntry(CatalogType type, Catalog &catalog, Identifier name, idx_t oid,
	                    shared_ptr<ReplicationLsnState> lsn_state);

	uint64_t RemoteLsn() const {
		return lsn_state->remote_lsn.load();
	}
	void RaiseRemoteLsn(uint64_t lsn);
	void AssignRemoteLsn(uint64_t lsn) {
		lsn_state->remote_lsn.store(lsn);
	}
	const shared_ptr<ReplicationLsnState> &LsnState() const {
		return lsn_state;
	}
	void MarkRelationSynced(idx_t relation, uint64_t lsn);
	optional<uint64_t> RelationSyncedLsn(idx_t relation) const;

private:
	shared_ptr<ReplicationLsnState> lsn_state;
};

} // namespace duckdb
