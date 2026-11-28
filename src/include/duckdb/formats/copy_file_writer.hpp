//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/copy_file_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/serializer/async_file_writer.hpp"

#include <exception>
#include <span>

namespace duckdb {

class CopyFileWriter {
public:
	static constexpr idx_t MAX_QUEUED_BYTES = idx_t(32) << 20;

	CopyFileWriter(ClientContext &context, const string &path, const FileCompressionType &compression,
	               string separator);
	CopyFileWriter(ClientContext &context, FileSystem &fs, const string &path, const FileCompressionType &compression,
	               string separator);
	~CopyFileWriter();

	bool Pieced() const {
		return pieced != nullptr;
	}
	void WriteFrame(const_data_ptr_t data, idx_t size);
	unique_ptr<CompressedPiece> Compress(std::span<const std::span<const data_t>> rows,
	                                     unique_ptr<PieceCompressor> &compressor) const;
	void WriteRows(unique_ptr<CompressedPiece> rows);
	void WriteRows(vector<unique_ptr<AsyncWritePayload>> rows);
	void WriteRows(const_data_ptr_t data, idx_t size);
	idx_t FileSize();
	idx_t Close();

private:
	void Separate();
	void Enqueue(unique_ptr<AsyncWritePayload> payload);
	void EnqueueBytes(const_data_ptr_t data, idx_t size);
	void EnqueuePiece(unique_ptr<CompressedPiece> piece);
	void Drain(unique_lock<mutex> &guard);
	void Write(unique_lock<mutex> &guard, const_data_ptr_t data, idx_t size);

	mutex lock;
	const unique_ptr<PiecedFile> pieced;
	const bool stream_compressed;
	unique_ptr<AsyncFileWriter> file;
	unique_ptr<PieceCompressor> compressor;
	unique_ptr<CompressedPiece> separator_piece;
	const string separator;
	vector<unique_ptr<AsyncWritePayload>> queued;
	idx_t queued_bytes = 0;
	idx_t written = 0;
	bool writing = false;
	bool wrote_rows = false;
	std::exception_ptr error;
};

} // namespace duckdb
