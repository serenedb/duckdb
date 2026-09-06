//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/shared_object_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace duckdb {

class BufferPool;

//! SharedObjectCache interns heavy immutable ObjectCacheEntry objects by (type, key) at database-instance scope.
//! Unlike ObjectCache it owns nothing: it keeps only weak references, so an entry lives exactly as long as some
//! holder references it, every concurrent holder of a key gets the same object, and entries are never evicted or
//! replaced. Each entry's GetEstimatedCacheMemory() is reserved in the buffer pool (counted toward memory_limit,
//! shown in duckdb_memory()) for as long as the entry lives — an invalid estimate means unaccounted here — and the
//! last holder releases it. Builds are single-flight: exactly one concurrent builder per key, other callers wait
//! for its result; a throwing build hands the key over to the next waiter.
class SharedObjectCache {
public:
	explicit SharedObjectCache(shared_ptr<BufferPool> buffer_pool);
	~SharedObjectCache();

	//! Return the live entry interned under T's (type, key), building it if absent. `build` returns unique_ptr<T>
	//! and runs without holding any cache lock (building can be slow).
	template <class T, class BUILD>
	shared_ptr<T> GetOrBuild(std::string_view key, BUILD &&build) {
		static_assert(std::is_same<decltype(T::ObjectType()), std::string_view>::value,
		              "ObjectType() must return a view of static storage");
		auto entry = GetOrBuildInternal(T::ObjectType(), key, [&]() -> unique_ptr<ObjectCacheEntry> {
			auto built = std::forward<BUILD>(build)();
			return unique_ptr<ObjectCacheEntry>(built.release());
		});
		return shared_ptr_cast<ObjectCacheEntry, T>(std::move(entry));
	}

	//! Return the live entry interned under T's (type, key), or nullptr.
	template <class T>
	shared_ptr<T> Get(std::string_view key) {
		static_assert(std::is_same<decltype(T::ObjectType()), std::string_view>::value,
		              "ObjectType() must return a view of static storage");
		auto entry = GetInternal(T::ObjectType(), key);
		if (!entry) {
			return nullptr;
		}
		return shared_ptr_cast<ObjectCacheEntry, T>(std::move(entry));
	}

	//! Number of live entries.
	idx_t GetEntryCount() const;
	//! Total bytes currently reserved in the buffer pool by live entries.
	idx_t GetMemoryUsage() const;

private:
	struct FullKey;
	struct KeyHash;
	struct KeyEq;
	using LookupKey = std::pair<std::string_view, std::string_view>;
	struct Slot;
	struct Registry;
	struct Deleter;

	shared_ptr<ObjectCacheEntry> ClaimSlot(const LookupKey &lookup);
	shared_ptr<ObjectCacheEntry> GetOrBuildInternal(std::string_view type, std::string_view key,
	                                                const std::function<unique_ptr<ObjectCacheEntry>()> &build);
	shared_ptr<ObjectCacheEntry> GetInternal(std::string_view type, std::string_view key);

private:
	//! Held via shared_ptr by the cache and by every entry's deleter, so an entry released after the cache's
	//! destruction still finds a live registry and buffer pool.
	shared_ptr<Registry> registry;
};

} // namespace duckdb
