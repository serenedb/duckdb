//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/block_cache/block_cache_spill.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! Evicted blocks on local disk: one file of fixed slots, written with direct I/O where the filesystem allows it.
//! Copies of blocks that exist elsewhere are dropped when the slots run out; copies of unbacked blocks never are.
class BlockCacheSpill {
public:
	~BlockCacheSpill();

	//! Spills into [path], keeping at most [backed_limit] bytes of blocks that exist elsewhere
	void Configure(FileSystem &fs, const string &path, idx_t backed_limit);
	//! Whether a file is configured, which unbacked blocks need
	bool IsConfigured() const;

	//! Stores [data] as the copy of [key]; false when no slot can be had. [keep] copies are never dropped.
	bool Write(uint64_t key, data_ptr_t data, bool keep);
	//! Reads the copy of [key] into [data]; false when there is none
	bool Read(uint64_t key, data_ptr_t data);
	bool Contains(uint64_t key) const;
	vector<uint64_t> Keys() const;
	void Erase(uint64_t key);
	//! Drops the copies of every block of the file numbered [file]
	void EraseFile(uint32_t file);
	//! Drops every copy that may be dropped
	void EraseBacked();

private:
	struct Slot {
		uint32_t index;
		bool keep;
		//! The place of a copy that may be dropped in backed_order
		list<uint64_t>::iterator order;
	};
	using Slots = unordered_map<uint64_t, Slot>;

	uint32_t AcquireSlot() DUCKDB_REQUIRES(lock);
	void Release(Slots::iterator entry) DUCKDB_REQUIRES(lock);
	bool DropOldestBacked() DUCKDB_REQUIRES(lock);

	mutable annotated_mutex lock;
	unique_ptr<FileHandle> handle DUCKDB_GUARDED_BY(lock);
	idx_t backed_limit DUCKDB_GUARDED_BY(lock) = 0;
	vector<uint32_t> free_slots DUCKDB_GUARDED_BY(lock);
	Slots slots DUCKDB_GUARDED_BY(lock);
	//! Backed copies in the order they were written, dropped oldest first
	list<uint64_t> backed_order DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
