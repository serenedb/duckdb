//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint/string_checkpoint_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/function/compression_function.hpp"

namespace duckdb {
struct UncompressedStringSegmentState;

class OverflowStringWriter {
public:
	virtual ~OverflowStringWriter() {
	}

	virtual void WriteString(UncompressedStringSegmentState &state, string_t string, block_id_t &result_block,
	                         int32_t &result_offset) = 0;
	virtual void Flush() = 0;
};

class OverflowStringReader {
public:
	virtual ~OverflowStringReader() = default;
	virtual string_t ReadString(Vector &result, block_id_t block, int32_t offset) = 0;
};

//! Append-only byte sink addressed by file position; lets a compression
//! function stream column data past the segment into an external file instead
//! of block-manager pages (analogous to OverflowStringWriter).
class ColumnStreamWriter {
public:
	virtual ~ColumnStreamWriter() = default;
	virtual idx_t Position() const = 0;
	virtual void Append(const_data_ptr_t data, idx_t size) = 0;
};

class ColumnStreamReader {
public:
	virtual ~ColumnStreamReader() = default;
	//! Returns a stable pointer to [position, position + size) if the source
	//! supports zero-copy access (e.g. a memory mapping), nullptr otherwise.
	virtual const_data_ptr_t TryReadStable(idx_t position, idx_t size) = 0;
	virtual void Read(idx_t position, data_ptr_t target, idx_t size) = 0;
};

struct StringBlock {
	shared_ptr<BlockHandle> block;
	idx_t offset;
	idx_t size;
	unique_ptr<StringBlock> next;
};

struct UncompressedStringSegmentState : public CompressedSegmentState {
	~UncompressedStringSegmentState() override;

	//! The string block holding strings that do not fit in the main block
	//! FIXME: this should be replaced by a heap that also allows freeing of unused strings
	unique_ptr<StringBlock> head;
	//! Map of block id to string block
	unordered_map<block_id_t, reference<StringBlock>> overflow_blocks;
	//! Overflow string writer (if any), if not set overflow strings will be written to memory blocks
	optional_ptr<OverflowStringWriter> overflow_writer;
	//! Holds the overflow writer when this state owns it (WriteOverflowStringsToDisk)
	unique_ptr<OverflowStringWriter> owned_overflow_writer;
	optional_ptr<OverflowStringReader> overflow_reader;
	//! Position-addressed reader for column data streamed to an external file
	//! (if any); its presence selects the streamed layout on scan
	optional_ptr<ColumnStreamReader> stream_reader;
	//! The block manager with which to write
	optional_ptr<BlockManager> block_manager;
	//! The set of overflow blocks written to disk (if any)
	vector<block_id_t> on_disk_blocks;

public:
	shared_ptr<BlockHandle> GetHandle(BlockManager &manager, block_id_t block_id);

	void RegisterBlock(BlockManager &manager, block_id_t block_id);

	string GetSegmentInfo() const override;

	void InsertOverflowBlock(block_id_t block_id, reference<StringBlock> block);
	reference<StringBlock> FindOverflowBlock(block_id_t block_id);

private:
	mutex block_lock;
	unordered_map<block_id_t, shared_ptr<BlockHandle>> handles;

	StorageLock overflow_blocks_lock;
};

} // namespace duckdb
