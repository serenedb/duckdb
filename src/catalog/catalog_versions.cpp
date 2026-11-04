#include "duckdb/catalog/catalog_versions.hpp"

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
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

optional_ptr<CatalogEntry> CatalogVersions::GetLatestCommitted() const {
	return committed.load(std::memory_order_acquire);
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
	auto entry = GetVisibleOwner(oid, view);
	return entry && entry->oid == oid ? entry : nullptr;
}

void CatalogOidIndex::GetVisible(std::span<const idx_t> oids, const SnapshotView &view,
                                 std::span<optional_ptr<CatalogEntry>> entries) const {
	D_ASSERT(oids.size() == entries.size());
	absl::ReaderMutexLock guard(lock);
	for (idx_t i = 0; i < oids.size(); i++) {
		auto it = versions.find(oids[i]);
		auto entry = it == versions.end() ? nullptr : it->second->GetVisible(view);
		entries[i] = entry && entry->oid == oids[i] ? entry : nullptr;
	}
}

optional_ptr<CatalogEntry> CatalogOidIndex::GetVisibleOwner(idx_t oid, const SnapshotView &view) const {
	absl::ReaderMutexLock guard(lock);
	auto it = versions.find(oid);
	return it == versions.end() ? nullptr : it->second->GetVisible(view);
}

optional_ptr<CatalogEntry> CatalogOidIndex::GetCommitted(idx_t oid) const {
	absl::ReaderMutexLock guard(lock);
	auto it = versions.find(oid);
	auto entry = it == versions.end() ? nullptr : it->second->GetCommitted();
	return entry && entry->oid == oid ? entry : nullptr;
}

optional_ptr<CatalogEntry> CatalogOidIndex::GetCommittedDrop(idx_t oid) const {
	absl::ReaderMutexLock guard(lock);
	auto it = versions.find(oid);
	auto entry = it == versions.end() ? nullptr : it->second->GetLatestCommitted();
	return entry && entry->deleted && entry->oid == oid ? entry : nullptr;
}

void CatalogOidIndex::ScanVisible(const SnapshotView &view, const std::function<void(CatalogEntry &)> &callback) const {
	vector<reference<CatalogEntry>> entries;
	{
		absl::ReaderMutexLock guard(lock);
		entries.reserve(versions.size());
		for (auto &[oid, object] : versions) {
			auto entry = object->GetVisible(view);
			if (entry && entry->oid == oid) {
				entries.push_back(*entry);
			}
		}
	}
	for (auto &entry : entries) {
		callback(entry);
	}
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

void CatalogOidIndex::AddAliases(const CatalogEntry &version, const shared_ptr<CatalogVersions> &object_versions) {
	auto oids = version.GetSubObjectOids();
	if (oids.empty()) {
		return;
	}
	absl::MutexLock guard(lock);
	for (auto oid : oids) {
		if (versions.try_emplace(oid, object_versions).second) {
			aliases[version.oid].push_back(oid);
		}
	}
}

void CatalogOidIndex::Erase(unordered_map<idx_t, shared_ptr<CatalogVersions>>::iterator it) {
	auto owned = aliases.find(it->first);
	if (owned != aliases.end()) {
		for (auto alias : owned->second) {
			auto alias_it = versions.find(alias);
			if (alias_it != versions.end() && alias_it->second == it->second) {
				versions.erase(alias_it);
			}
		}
		aliases.erase(owned);
	}
	versions.erase(it);
}

void CatalogOidIndex::Install(CatalogEntry &version, const CatalogEntry &object) {
	if (!IsIndexed(object)) {
		return;
	}
	auto object_versions = GetOrCreate(object);
	object_versions->Install(version);
	AddAliases(version, object_versions);
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
		Erase(it);
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
		Erase(it);
	}
}

} // namespace duckdb
