#include "duckdb/catalog/catalog_entry/replication_lsn_entry.hpp"

namespace duckdb {

ReplicationLsnEntry::ReplicationLsnEntry(CatalogType type, Catalog &catalog, Identifier name, idx_t oid,
                                         shared_ptr<ReplicationLsnState> lsn_state_p)
    : InCatalogEntry(type, catalog, std::move(name), oid), lsn_state(std::move(lsn_state_p)) {
	D_ASSERT(lsn_state);
}

void ReplicationLsnEntry::RaiseRemoteLsn(uint64_t lsn) {
	auto current = lsn_state->remote_lsn.load();
	while (lsn > current && !lsn_state->remote_lsn.compare_exchange_weak(current, lsn)) {
	}
}

void ReplicationLsnEntry::MarkRelationSynced(idx_t relation, uint64_t lsn) {
	lock_guard<mutex> guard(lsn_state->relation_lock);
	lsn_state->relation_lsns[relation] = lsn;
}

optional<uint64_t> ReplicationLsnEntry::RelationSyncedLsn(idx_t relation) const {
	lock_guard<mutex> guard(lsn_state->relation_lock);
	auto entry = lsn_state->relation_lsns.find(relation);
	if (entry == lsn_state->relation_lsns.end()) {
		return nullopt;
	}
	return entry->second;
}

} // namespace duckdb
