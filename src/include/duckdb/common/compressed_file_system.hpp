//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/compressed_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/common/serializer/async_write_queue.hpp"

#include <span>

namespace duckdb {
class CompressedFile;

class CompressedPiece : public AsyncWritePayload {
public:
	data_ptr_t Ptr() override {
		return data.get();
	}
	idx_t Size() const override {
		return size;
	}
	DUCKDB_API data_ptr_t Reserve(idx_t count);

	unsafe_unique_array<data_t> data;
	idx_t capacity = 0;
	idx_t size = 0;
	idx_t input_size = 0;
	uint32_t input_crc = 0;
	bool last = false;
};

class PieceCompressor {
public:
	DUCKDB_API virtual ~PieceCompressor();
	DUCKDB_API virtual void Compress(std::span<const std::span<const data_t>> inputs, bool last,
	                                 CompressedPiece &piece) = 0;
	DUCKDB_API void Compress(const_data_ptr_t data, idx_t size, CompressedPiece &piece, bool last = false);
};

class PiecedFile {
public:
	DUCKDB_API virtual ~PiecedFile();
	DUCKDB_API virtual unique_ptr<PieceCompressor> CreateCompressor() const = 0;
	DUCKDB_API virtual string Head() const;
	DUCKDB_API virtual void Append(const CompressedPiece &piece);
	DUCKDB_API virtual string Tail() const;
};

struct PieceSpan {
	idx_t offset = 0;
	idx_t size = 0;
	idx_t output_offset = 0;
	idx_t output_size = 0;
};

class PieceDecompressor {
public:
	DUCKDB_API virtual ~PieceDecompressor();
	DUCKDB_API virtual void DecompressAll(const_data_ptr_t piece, const PieceSpan &span, data_ptr_t out) = 0;
	DUCKDB_API virtual void Begin(const_data_ptr_t piece, const PieceSpan &span) = 0;
	DUCKDB_API virtual void Continue(data_ptr_t out, idx_t count) = 0;
};

struct StreamData {
	// various buffers & pointers
	bool write = false;
	bool refresh = false;
	unsafe_unique_array<data_t> in_buff;
	unsafe_unique_array<data_t> out_buff;
	data_ptr_t out_buff_start = nullptr;
	data_ptr_t out_buff_end = nullptr;
	data_ptr_t in_buff_start = nullptr;
	data_ptr_t in_buff_end = nullptr;

	idx_t in_buf_size = 0;
	idx_t out_buf_size = 0;
};

struct StreamWrapper {
	DUCKDB_API virtual ~StreamWrapper();

	DUCKDB_API virtual void Initialize(QueryContext context, CompressedFile &file, bool write) = 0;
	DUCKDB_API virtual bool Read(StreamData &stream_data) = 0;
	DUCKDB_API virtual void FinalizeRead(StreamData &stream_data);
	DUCKDB_API virtual void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer, int64_t nr_bytes);
	DUCKDB_API virtual void Close() = 0;
	DUCKDB_API virtual void AbortWrite();
};

class CompressedFileSystem : public FileSystem {
public:
	DUCKDB_API int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	DUCKDB_API int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	DUCKDB_API void Reset(FileHandle &handle) override;

	DUCKDB_API int64_t GetFileSize(FileHandle &handle) override;

	DUCKDB_API bool OnDiskFile(FileHandle &handle) override;
	DUCKDB_API bool CanSeek() override;
	DUCKDB_API void AbortFileWrite(FileHandle &handle) override;

	//! The compression scheme provided by this filesystem, e.g. "gzip"
	DUCKDB_API virtual FileCompressionType GetCompressionType() = 0;
	//! Whether this filesystem can decompress the given file - used to auto-detect compression from the file name.
	//! Filesystems that do not override this do not participate in compression auto-detection.
	DUCKDB_API bool CanHandleFile(const string &fpath) override;

	DUCKDB_API virtual unique_ptr<StreamWrapper> CreateStream() = 0;
	DUCKDB_API virtual idx_t InBufferSize() = 0;
	DUCKDB_API virtual idx_t OutBufferSize() = 0;
	DUCKDB_API virtual unique_ptr<PiecedFile> CreatePieces();
	DUCKDB_API virtual bool FindPieces(const_data_ptr_t data, idx_t size, vector<PieceSpan> &pieces);
	DUCKDB_API virtual unique_ptr<PieceDecompressor> CreateDecompressor();
};

DUCKDB_API unique_ptr<PiecedFile> CreatePiecedFile(FileSystem &fs, const FileCompressionType &compression,
                                                   const string &path);

class CompressedFile : public FileHandle {
public:
	DUCKDB_API CompressedFile(CompressedFileSystem &fs, unique_ptr<FileHandle> child_handle_p, const string &path);
	DUCKDB_API ~CompressedFile() override;

	DUCKDB_API idx_t GetProgress() override;
	DUCKDB_API bool IsPipe() override;

	CompressedFileSystem &compressed_fs;
	unique_ptr<FileHandle> child_handle;
	//! Whether the file is opened for reading or for writing
	bool write = false;
	StreamData stream_data;
	//! The query context, used to attribute the (compressed) on-disk I/O of the child handle to the query
	QueryContext context;

public:
	DUCKDB_API void Initialize(QueryContext context, bool write);
	DUCKDB_API int64_t ReadData(void *buffer, int64_t nr_bytes);
	DUCKDB_API int64_t WriteData(data_ptr_t buffer, int64_t nr_bytes);
	DUCKDB_API void Close() override;
	DUCKDB_API void AbortCompressedWrite();

	static constexpr idx_t PIECE_BYTES = idx_t(1) << 20;

private:
	void Clear(); // for Initialize re-use to support FS.Reset()
	void ResetStreamData();
	void WritePiece(const_data_ptr_t data, idx_t size, bool last);

	//! The number of compressed bytes read from the child handle
	idx_t compressed_bytes_read = 0;
	//! The number of compressed bytes consumed by the decompressor (for progress)
	atomic<idx_t> compressed_bytes_consumed {0};
	bool initialized = false;
	unique_ptr<StreamWrapper> stream_wrapper;
	unique_ptr<PiecedFile> pieced;
	unique_ptr<PieceCompressor> compressor;
	unsafe_unique_array<data_t> pending;
	idx_t pending_size = 0;
	CompressedPiece piece;
};

} // namespace duckdb
