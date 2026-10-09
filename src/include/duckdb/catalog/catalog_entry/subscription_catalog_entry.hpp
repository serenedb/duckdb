//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/subscription_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/shared_ptr.hpp"

namespace duckdb {

struct SubscriptionLsnState {
	explicit SubscriptionLsnState(uint64_t remote_lsn) : remote_lsn(remote_lsn) {
	}

	atomic<uint64_t> remote_lsn;
};

class SubscriptionCatalogEntry : public InCatalogEntry {
public:
	static constexpr const CatalogType Type = CatalogType::SUBSCRIPTION_ENTRY;
	static constexpr const char *Name = "subscription";

	SubscriptionCatalogEntry(Catalog &catalog, Identifier name, idx_t oid, shared_ptr<SubscriptionLsnState> lsn_state);

	uint64_t RemoteLsn() const {
		return lsn_state->remote_lsn.load();
	}
	void RaiseRemoteLsn(uint64_t lsn);
	const shared_ptr<SubscriptionLsnState> &LsnState() const {
		return lsn_state;
	}

private:
	shared_ptr<SubscriptionLsnState> lsn_state;
};

} // namespace duckdb
