#include "duckdb/catalog/catalog_entry/subscription_catalog_entry.hpp"

namespace duckdb {

SubscriptionCatalogEntry::SubscriptionCatalogEntry(Catalog &catalog, Identifier name, idx_t oid,
                                                   shared_ptr<ReplicationLsnState> lsn_state)
    : ReplicationLsnEntry(CatalogType::SUBSCRIPTION_ENTRY, catalog, std::move(name), oid, std::move(lsn_state)) {
}

} // namespace duckdb
