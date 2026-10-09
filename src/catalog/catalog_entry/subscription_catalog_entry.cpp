#include "duckdb/catalog/catalog_entry/subscription_catalog_entry.hpp"

namespace duckdb {

SubscriptionCatalogEntry::SubscriptionCatalogEntry(Catalog &catalog, Identifier name, idx_t oid,
                                                   shared_ptr<SubscriptionLsnState> lsn_state_p)
    : InCatalogEntry(CatalogType::SUBSCRIPTION_ENTRY, catalog, std::move(name), oid),
      lsn_state(std::move(lsn_state_p)) {
	D_ASSERT(lsn_state);
}

void SubscriptionCatalogEntry::RaiseRemoteLsn(uint64_t lsn) {
	auto current = lsn_state->remote_lsn.load();
	while (lsn > current && !lsn_state->remote_lsn.compare_exchange_weak(current, lsn)) {
	}
}

} // namespace duckdb
