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
#include "duckdb/common/limits.hpp"
#include "duckdb/common/list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/temporary_file_manager.hpp"

namespace duckdb {

//! Evicted blocks on local disk: one file of fixed slots, written with direct I/O where the filesystem allows it.
//! Copies of blocks that exist elsewhere are dropped when the slots run out; copies of unbacked blocks never are.
class BlockCacheSpill {
public:
	static constexpr uint32_t NO_SLOT = NumericLimits<uint32_t>::Maximum();

	//! The slot reserved for the copy of a block, which readers of the block wait for until it is written or cancelled
	struct Reservation {
		uint64_t key = 0;
		//! NO_SLOT when the copy existed already
		uint32_t slot = NO_SLOT;
	};

	~BlockCacheSpill();

	//! Spills into [path], keeping at most [backed_limit] bytes of blocks that exist elsewhere; an empty [path] keeps
	//! the file ConfigureDefault opened
	void Configure(FileSystem &fs, const string &path, idx_t backed_limit);
	//! Spills only blocks that exist nowhere else, unless a file is configured, into a file among [temporary_files],
	//! whose size counts against their limit
	void ConfigureDefault(FileSystem &fs, TemporaryFileManager &temporary_files);
	//! Whether a file is configured, which unbacked blocks need
	bool IsConfigured() const;

	//! Reserves a slot for the copy of [key], or finds the copy there is; false when no slot can be had. [keep] copies
	//! are never dropped.
	bool Reserve(uint64_t key, bool keep, Reservation &result);
	//! Writes [data] into the slot of [reservation], which publishes the copy; false when the write fails
	bool Write(const Reservation &reservation, data_ptr_t data);
	//! Gives up [reservation] without writing it
	void Cancel(const Reservation &reservation);
	//! Reads the copy of [key] into [data], waiting while it is reserved but not written; false when there is none
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
		//! Whether the slot holds the copy; until it does, readers of the copy wait
		bool written;
		//! The place of a copy that may be dropped in backed_order
		list<uint64_t>::iterator order;
	};
	using Slots = unordered_map<uint64_t, Slot>;

	void Open(FileSystem &fs, const string &path) DUCKDB_REQUIRES(lock);
	//! Closes and removes the file, which drops every copy
	void Close() DUCKDB_REQUIRES(lock);
	uint32_t AcquireSlot() DUCKDB_REQUIRES(lock);
	void Release(Slots::iterator entry) DUCKDB_REQUIRES(lock);
	bool DropOldestBacked() DUCKDB_REQUIRES(lock);

	mutable annotated_mutex lock;
	unique_ptr<FileHandle> handle DUCKDB_GUARDED_BY(lock);
	//! Whether ConfigureDefault opened the file
	bool is_default DUCKDB_GUARDED_BY(lock) = false;
	//! The temporary files whose limit the file counts against
	optional_ptr<TemporaryFileManager> temporary_files DUCKDB_GUARDED_BY(lock);
	//! The slots the file spans
	idx_t extent DUCKDB_GUARDED_BY(lock) = 0;
	idx_t backed_limit DUCKDB_GUARDED_BY(lock) = 0;
	vector<uint32_t> free_slots DUCKDB_GUARDED_BY(lock);
	Slots slots DUCKDB_GUARDED_BY(lock);
	//! Backed copies in the order they were written, dropped oldest first
	list<uint64_t> backed_order DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
