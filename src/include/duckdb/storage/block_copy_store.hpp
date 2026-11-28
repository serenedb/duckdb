//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/block_copy_store.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/file_buffer.hpp"
#include "duckdb/common/list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"

namespace duckdb {

class DatabaseInstance;
class FileBuffer;
class QueryContext;
class TemporaryFileManager;

//! Copies of evicted blocks that have a name, a file each in a directory of the temporary directory. They outlive the
//! database: a later database that uses the same temporary directory reads them instead of fetching the bytes again.
//! A copy stays while a block holds it; the copies no block holds make room for new ones, least recently used first
class BlockCopyStore {
public:
	//! The directory of the copies in the temporary directory
	static constexpr const char *DIRECTORY = "external_file_cache";

	//! The copies in the temporary directory [temp_directory], whose disk space [files] counts. A copy is written to a
	//! file named [staging_prefix] and a number in the temporary directory before it takes its name
	BlockCopyStore(DatabaseInstance &db, const string &temp_directory, TemporaryFileManager &files,
	               string staging_prefix);

	//! Finds the copies an earlier database left; the oldest go while the copies take more than [limit] bytes or than
	//! the temporary directory has room for
	void Load(idx_t limit);
	//! Whether the copy [name] exists; true makes the caller a block that holds it
	bool Hold(const BlockCopyName &name);
	//! A block that held the copy [name] is gone; the copy stays
	void Release(const BlockCopyName &name);
	//! Writes [buffer] as the copy [name] for a block that holds it from then on. Copies no block holds go, least
	//! recently used first, while the copies would take more than [limit] bytes or than the temporary directory has
	//! room for; false when that is not enough
	bool TryWrite(QueryContext context, const BlockCopyName &name, FileBuffer &buffer, idx_t limit);
	//! The copy [name] in a buffer of [type] that allocates [alloc_size] bytes; null when it is missing or damaged,
	//! and then it is gone
	unique_ptr<FileBuffer> Read(QueryContext context, const BlockCopyName &name, FileBufferType type, idx_t alloc_size,
	                            unique_ptr<FileBuffer> reusable_buffer);
	//! Removes the copy [name]: its bytes are no longer those of the file they were read from
	void Remove(const BlockCopyName &name);
	//! The disk space the copies take
	idx_t Bytes() const;

private:
	struct Entry {
		idx_t size;
		//! The blocks that hold the copy
		idx_t holds;
		//! Identifies the write that creates the file until it is written
		idx_t write;
		//! The place in the copies no block holds
		list<BlockCopyName>::iterator unheld;
	};
	using Entries = unordered_map<BlockCopyName, Entry, BlockCopyNameHash>;

	string PathOf(const BlockCopyName &name) const;
	//! Makes the block that called a holder of [entry]
	void HoldLocked(Entries::iterator entry);
	//! Forgets [entry], whose file the caller removes
	void EraseLocked(Entries::iterator entry);
	//! Removes the copies no block holds, least recently used first, while [fits] is false. Files are removed under
	//! the lock, so a removal never takes the file of a later write of the same name
	template <class FITS>
	void MakeRoomLocked(FITS &&fits);

	DatabaseInstance &db;
	TemporaryFileManager &files;
	const string temp_directory;
	const string directory;
	const string staging_prefix;

	mutable mutex lock;
	Entries entries;
	//! The copies no block holds, least recently used first
	list<BlockCopyName> unheld;
	idx_t bytes = 0;
	idx_t writes = 0;
	bool has_directory = false;
};

} // namespace duckdb
