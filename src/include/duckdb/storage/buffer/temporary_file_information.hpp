//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer/temporary_file_information.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/memory_tag.hpp"

namespace duckdb {

struct MemoryInformation {
	MemoryTag tag;
	idx_t size;
	idx_t evicted_data;
	//! Blocks evicted, buffers written to temporary files and buffers read back from them since start
	idx_t evictions = 0;
	idx_t temporary_writes = 0;
	idx_t temporary_reads = 0;
};

struct TemporaryFileInformation {
	string path;
	idx_t size;
};

struct CachedFileInformation {
	string path;
	idx_t nr_bytes;
	idx_t location;
	bool loaded;
	bool spilled;
};

struct EvictionQueueInformation {
	idx_t queue_index;
	string queue_type;
	idx_t approximate_size;
	idx_t dead_nodes;
	idx_t total_insertions;
};

} // namespace duckdb
