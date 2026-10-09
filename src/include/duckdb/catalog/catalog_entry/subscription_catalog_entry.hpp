//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/subscription_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/replication_lsn_entry.hpp"

namespace duckdb {

class SubscriptionCatalogEntry : public ReplicationLsnEntry {
public:
	static constexpr const CatalogType Type = CatalogType::SUBSCRIPTION_ENTRY;
	static constexpr const char *Name = "subscription";

	SubscriptionCatalogEntry(Catalog &catalog, Identifier name, idx_t oid, shared_ptr<ReplicationLsnState> lsn_state);
};

} // namespace duckdb
