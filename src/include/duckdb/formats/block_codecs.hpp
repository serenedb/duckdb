//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/block_codecs.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/unique_ptr.hpp"

struct z_stream_s;

namespace duckdb {

enum class BlockCodec : uint8_t { GZIP };

class BlockDecompressor {
public:
	BlockDecompressor();
	~BlockDecompressor();
	BlockDecompressor(const BlockDecompressor &) = delete;
	BlockDecompressor &operator=(const BlockDecompressor &) = delete;

	bool TryDecompress(BlockCodec codec, const_data_ptr_t src, idx_t src_size, data_ptr_t dst, idx_t dst_size,
	                   string &error);
	bool TryDecompress(BlockCodec codec, const_data_ptr_t src, idx_t src_size, Allocator &allocator, idx_t padding,
	                   AllocatedData &out, idx_t &size, string &error);

private:
	class Output;

	bool Decompress(BlockCodec codec, const_data_ptr_t src, idx_t src_size, Output &out, string &error);
	bool Inflate(int window_bits, const_data_ptr_t src, idx_t src_size, Output &out, string &error);

private:
	unique_ptr<z_stream_s> zlib;
};

} // namespace duckdb
