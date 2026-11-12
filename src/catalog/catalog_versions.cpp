#include "duckdb/catalog/catalog_versions.hpp"

namespace duckdb {

optional_ptr<CatalogEntry> CatalogVersions::GetVisible(const SnapshotView &view) const {
	CatalogEntry *entry = nullptr;
	if (pending_transaction.load(std::memory_order_acquire) == view.transaction_id) {
		entry = pending.load(std::memory_order_acquire);
	}
	if (!entry) {
		entry = committed.load(std::memory_order_acquire);
	}
	while (entry && !view.Sees(entry->timestamp.load(std::memory_order_acquire))) {
		entry = entry->previous_version.load(std::memory_order_acquire);
	}
	if (!entry || entry->deleted) {
		return nullptr;
	}
	return entry;
}

optional_ptr<CatalogEntry> CatalogVersions::GetCommitted() const {
	auto entry = committed.load(std::memory_order_acquire);
	if (!entry || entry->deleted) {
		return nullptr;
	}
	return entry;
}

bool CatalogVersions::HasPending() const {
	return pending_transaction.load(std::memory_order_acquire) != MAX_TRANSACTION_ID;
}

void CatalogVersions::Install(CatalogEntry &version) {
	auto timestamp = version.timestamp.load(std::memory_order_acquire);
	if (IsCommitted(timestamp)) {
		version.previous_version.store(committed.load(std::memory_order_relaxed), std::memory_order_relaxed);
		committed.store(&version, std::memory_order_release);
		return;
	}
	auto own_pending = pending_transaction.load(std::memory_order_relaxed) == timestamp;
	auto previous = own_pending ? pending.load(std::memory_order_relaxed) : committed.load(std::memory_order_relaxed);
	version.previous_version.store(previous, std::memory_order_relaxed);
	pending.store(&version, std::memory_order_release);
	pending_transaction.store(timestamp, std::memory_order_release);
}

void CatalogVersions::Commit(CatalogEntry &version) {
	committed.store(&version, std::memory_order_release);
	if (pending.load(std::memory_order_relaxed) == &version) {
		pending_transaction.store(MAX_TRANSACTION_ID, std::memory_order_release);
		pending.store(nullptr, std::memory_order_release);
	}
}

void CatalogVersions::Rollback(CatalogEntry &version) {
	if (pending.load(std::memory_order_relaxed) != &version) {
		return;
	}
	auto previous = version.previous_version.load(std::memory_order_relaxed);
	if (previous &&
	    previous->timestamp.load(std::memory_order_relaxed) == version.timestamp.load(std::memory_order_relaxed)) {
		pending.store(previous, std::memory_order_release);
		return;
	}
	pending_transaction.store(MAX_TRANSACTION_ID, std::memory_order_release);
	pending.store(nullptr, std::memory_order_release);
}

void CatalogVersions::Forget(CatalogEntry &version) {
	auto expected = &version;
	committed.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

} // namespace duckdb
