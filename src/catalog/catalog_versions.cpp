#include "duckdb/catalog/catalog_versions.hpp"

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/schema_info.hpp"

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

bool CatalogVersions::IsEmpty() const {
	return !committed.load(std::memory_order_acquire) && !pending.load(std::memory_order_acquire);
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

void CatalogVersions::RevertCommit(CatalogEntry &version) {
	if (committed.load(std::memory_order_relaxed) == &version) {
		committed.store(version.previous_version.load(std::memory_order_relaxed), std::memory_order_release);
	}
	if (!pending.load(std::memory_order_relaxed)) {
		pending.store(&version, std::memory_order_release);
		pending_transaction.store(version.timestamp.load(std::memory_order_relaxed), std::memory_order_release);
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

void CatalogVersions::Unlink(CatalogEntry &version) {
	if (committed.load(std::memory_order_relaxed) == &version) {
		committed.store(version.previous_version.load(std::memory_order_relaxed), std::memory_order_release);
	}
	auto entry = pending.load(std::memory_order_relaxed);
	if (!entry) {
		entry = committed.load(std::memory_order_relaxed);
	}
	while (entry) {
		auto previous = entry->previous_version.load(std::memory_order_relaxed);
		if (previous == &version) {
			entry->previous_version.store(version.previous_version.load(std::memory_order_relaxed),
			                              std::memory_order_release);
			return;
		}
		entry = previous;
	}
}

bool CatalogOidIndex::IsIndexed(const CatalogEntry &entry) {
	switch (entry.type) {
	case CatalogType::INVALID:
	case CatalogType::DELETED_ENTRY:
	case CatalogType::RENAMED_ENTRY:
	case CatalogType::DEPENDENCY_ENTRY:
		return false;
	case CatalogType::SCHEMA_ENTRY:
		return true;
	default:
		return !entry.internal;
	}
}

shared_ptr<CatalogVersions> CatalogOidIndex::Find(idx_t oid) const {
	absl::ReaderMutexLock guard(lock);
	auto it = versions.find(oid);
	if (it == versions.end()) {
		return nullptr;
	}
	return it->second;
}

optional_ptr<CatalogEntry> CatalogOidIndex::GetVisible(idx_t oid, const SnapshotView &view) const {
	auto object = Find(oid);
	return object ? object->GetVisible(view) : nullptr;
}

optional_ptr<CatalogEntry> CatalogOidIndex::GetCommitted(idx_t oid) const {
	auto object = Find(oid);
	return object ? object->GetCommitted() : nullptr;
}

shared_ptr<CatalogVersions> CatalogOidIndex::GetOrCreate(const CatalogEntry &object) {
	absl::MutexLock guard(lock);
	auto &slot = versions[object.oid];
	if (!slot) {
		if (object.type == CatalogType::SCHEMA_ENTRY) {
			slot = object.Cast<SchemaCatalogEntry>().GetSchemaInfo()->versions;
		} else {
			slot = make_shared_ptr<CatalogVersions>();
		}
	}
	return slot;
}

void CatalogOidIndex::Install(CatalogEntry &version, const CatalogEntry &object) {
	if (!IsIndexed(object)) {
		return;
	}
	GetOrCreate(object)->Install(version);
}

void CatalogOidIndex::Commit(CatalogEntry &version, const CatalogEntry &object) {
	if (!IsIndexed(object)) {
		return;
	}
	auto object_versions = Find(object.oid);
	if (object_versions) {
		object_versions->Commit(version);
	}
}

void CatalogOidIndex::RevertCommit(CatalogEntry &version, const CatalogEntry &object) {
	if (!IsIndexed(object)) {
		return;
	}
	auto object_versions = Find(object.oid);
	if (object_versions) {
		object_versions->RevertCommit(version);
	}
}

void CatalogOidIndex::Rollback(CatalogEntry &version, const CatalogEntry &object) {
	if (!IsIndexed(object)) {
		return;
	}
	absl::MutexLock guard(lock);
	auto it = versions.find(object.oid);
	if (it == versions.end()) {
		return;
	}
	it->second->Rollback(version);
	if (it->second->IsEmpty()) {
		versions.erase(it);
	}
}

void CatalogOidIndex::Unlink(CatalogEntry &version) {
	if (!IsIndexed(version) && version.type != CatalogType::DELETED_ENTRY) {
		return;
	}
	absl::MutexLock guard(lock);
	auto it = versions.find(version.oid);
	if (it == versions.end()) {
		return;
	}
	it->second->Unlink(version);
	if (it->second->IsEmpty()) {
		versions.erase(it);
	}
}

} // namespace duckdb
