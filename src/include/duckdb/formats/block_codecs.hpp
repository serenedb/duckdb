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

#include <climits>
#include <span>

struct z_stream_s;
struct ZSTD_DCtx_s;
struct ZSTD_CCtx_s;
struct LZ4F_dctx_s;
struct LZ4F_cctx_s;

namespace duckdb {

enum class BlockCodec : uint8_t { DEFLATE, GZIP, SNAPPY, ZSTD, LZ4, LZ4_FRAME, BROTLI, LZ4_HADOOP };

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
	bool Snappy(const_data_ptr_t src, idx_t src_size, Output &out, string &error);
	bool Zstd(const_data_ptr_t src, idx_t src_size, Output &out, string &error);
	bool Lz4(const_data_ptr_t src, idx_t src_size, Output &out, string &error);
	bool Lz4Hadoop(const_data_ptr_t src, idx_t src_size, Output &out, string &error);
	bool Lz4Frame(const_data_ptr_t src, idx_t src_size, Output &out, string &error);
	bool Brotli(const_data_ptr_t src, idx_t src_size, Output &out, string &error);

private:
	unique_ptr<z_stream_s> zlib;
	ZSTD_DCtx_s *zstd = nullptr;
	LZ4F_dctx_s *lz4f = nullptr;
};

class BlockCompressor {
public:
	static constexpr int DEFAULT_LEVEL = INT_MIN;

	explicit BlockCompressor(BlockCodec codec, int level = DEFAULT_LEVEL);
	~BlockCompressor();
	BlockCompressor(const BlockCompressor &) = delete;
	BlockCompressor &operator=(const BlockCompressor &) = delete;

	idx_t MaxCompressedSize(idx_t size);
	idx_t Compress(const_data_ptr_t src, idx_t size, data_ptr_t dst, idx_t capacity);
	idx_t Compress(std::span<const std::span<const data_t>> inputs, bool finish, data_ptr_t dst, idx_t capacity);

private:
	z_stream_s &Zlib();
	void Deflate(std::span<const data_t> input, int flush, data_ptr_t dst, idx_t capacity, idx_t &written);
	idx_t CompressOne(std::span<const data_t> input, data_ptr_t dst, idx_t capacity);

private:
	const BlockCodec codec;
	const int level;
	unique_ptr<z_stream_s> zlib;
	ZSTD_CCtx_s *zstd = nullptr;
	LZ4F_cctx_s *lz4f = nullptr;
};

} // namespace duckdb
