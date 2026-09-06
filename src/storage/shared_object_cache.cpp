#include "duckdb/storage/shared_object_cache.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/buffer/buffer_pool_reservation.hpp"

#include "absl/hash/hash.h"

namespace duckdb {

//! `type` views the static string literal behind T::ObjectType(), so a stored key never copies it.
struct SharedObjectCache::FullKey {
	explicit FullKey(const LookupKey &lookup) : type(lookup.first), key(lookup.second) {
	}

	std::string_view type;
	string key;
};

struct SharedObjectCache::KeyHash {
	using is_transparent = void;

	size_t operator()(const FullKey &k) const noexcept {
		return absl::HashOf(k.type, std::string_view {k.key});
	}
	size_t operator()(const LookupKey &k) const noexcept {
		return absl::HashOf(k.first, k.second);
	}
};

struct SharedObjectCache::KeyEq {
	using is_transparent = void;

	bool operator()(const FullKey &a, const FullKey &b) const noexcept {
		return a.type == b.type && a.key == b.key;
	}
	bool operator()(const FullKey &a, const LookupKey &b) const noexcept {
		return a.type == b.first && a.key == b.second;
	}
	bool operator()(const LookupKey &a, const FullKey &b) const noexcept {
		return b.type == a.first && b.key == a.second;
	}
};

struct SharedObjectCache::Slot {
	weak_ptr<ObjectCacheEntry> value;
	idx_t reserved = 0;
	//! True while one thread runs `build` for this slot; other threads Await on it.
	bool building = false;
	//! Threads blocked in Await on this slot; an erase would invalidate their slot pointer.
	idx_t waiters = 0;
};

struct SharedObjectCache::Registry {
	explicit Registry(shared_ptr<BufferPool> buffer_pool_p) : buffer_pool(std::move(buffer_pool_p)) {
	}

	mutex lock;
	unordered_map<FullKey, Slot, KeyHash, KeyEq> slots;
	shared_ptr<BufferPool> buffer_pool;
};

//! Deleter of an interned entry: destroys the payload, releases its buffer pool reservation, and drops the
//! registry slot unless it is still in use by a rebuild or its waiters.
struct SharedObjectCache::Deleter {
	shared_ptr<Registry> registry;
	FullKey key;
	shared_ptr<TempBufferPoolReservation> reservation;

	void operator()(ObjectCacheEntry *entry) {
		delete entry;
		reservation.reset();
		const absl::MutexLock guard(&registry->lock);
		auto it = registry->slots.find(key);
		if (it != registry->slots.end() && it->second.value.expired() && !it->second.building &&
		    it->second.waiters == 0) {
			registry->slots.erase(it);
		}
	}
};

SharedObjectCache::SharedObjectCache(shared_ptr<BufferPool> buffer_pool)
    : registry(make_shared_ptr<Registry>(std::move(buffer_pool))) {
}

SharedObjectCache::~SharedObjectCache() = default;

//! Wait until the key is either live (return it) or unclaimed (mark it building, return nullptr). A waiter's
//! slot pointer stays valid across Await: the map is node-based, and a slot with building == true or waiters > 0
//! is never erased.
shared_ptr<ObjectCacheEntry> SharedObjectCache::ClaimSlot(const LookupKey &lookup) {
	const absl::MutexLock guard(&registry->lock);
	auto *slot = &registry->slots.try_emplace(FullKey {lookup}).first->second;
	while (true) {
		if (auto live = slot->value.lock()) {
			return live;
		}
		if (!slot->building) {
			slot->building = true;
			return nullptr;
		}
		slot->waiters++;
		registry->lock.Await(absl::Condition(+[](Slot *waited) { return !waited->building; }, slot));
		slot->waiters--;
	}
}

shared_ptr<ObjectCacheEntry>
SharedObjectCache::GetOrBuildInternal(std::string_view type, std::string_view key,
                                      const std::function<unique_ptr<ObjectCacheEntry>()> &build) {
	const LookupKey lookup {type, key};
	{
		const absl::ReaderMutexLock guard(&registry->lock);
		auto it = registry->slots.find(lookup);
		if (it != registry->slots.end()) {
			if (auto live = it->second.value.lock()) {
				return live;
			}
		}
	}
	if (auto live = ClaimSlot(lookup)) {
		return live;
	}

	try {
		auto built = build();
		if (!built) {
			throw InternalException("SharedObjectCache: build returned no entry for key \"%s\"", string(key));
		}
		const auto estimated_memory = built->GetEstimatedCacheMemory();
		const idx_t size = estimated_memory.IsValid() ? estimated_memory.GetIndex() : 0;
		// Fully construct the deleter before releasing `built`: a throw here leaves the entry owned.
		Deleter deleter {registry, FullKey {lookup},
		                 make_shared_ptr<TempBufferPoolReservation>(MemoryTag::OBJECT_CACHE, *registry->buffer_pool,
		                                                            size)};
		shared_ptr<ObjectCacheEntry> value(built.release(), std::move(deleter));
		const absl::MutexLock guard(&registry->lock);
		auto it = registry->slots.find(lookup);
		D_ASSERT(it != registry->slots.end());
		it->second.value = value;
		it->second.reserved = size;
		it->second.building = false;
		return value;
	} catch (...) {
		// Hand the key over to the next waiter, or drop the slot if nobody wants it.
		const absl::MutexLock guard(&registry->lock);
		auto it = registry->slots.find(lookup);
		D_ASSERT(it != registry->slots.end());
		it->second.building = false;
		if (it->second.waiters == 0 && it->second.value.expired()) {
			registry->slots.erase(it);
		}
		throw;
	}
}

shared_ptr<ObjectCacheEntry> SharedObjectCache::GetInternal(std::string_view type, std::string_view key) {
	const absl::ReaderMutexLock guard(&registry->lock);
	auto it = registry->slots.find(LookupKey {type, key});
	if (it == registry->slots.end()) {
		return nullptr;
	}
	return it->second.value.lock();
}

idx_t SharedObjectCache::GetEntryCount() const {
	const absl::ReaderMutexLock guard(&registry->lock);
	idx_t count = 0;
	for (auto &slot : registry->slots) {
		count += slot.second.value.expired() ? 0 : 1;
	}
	return count;
}

idx_t SharedObjectCache::GetMemoryUsage() const {
	const absl::ReaderMutexLock guard(&registry->lock);
	idx_t reserved = 0;
	for (auto &slot : registry->slots) {
		if (!slot.second.value.expired()) {
			reserved += slot.second.reserved;
		}
	}
	return reserved;
}

} // namespace duckdb
