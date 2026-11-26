//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/gzip_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/compressed_file_system.hpp"

namespace duckdb {

class GZipFileSystem : public CompressedFileSystem {
	// 256 KB
	static constexpr const idx_t BUFFER_SIZE = 1u << 18;

public:
	unique_ptr<FileHandle> OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle, bool write) override;

	std::string GetName() const override {
		return "GZipFileSystem";
	}

	FileCompressionType GetCompressionType() override {
		return FileCompressionType::GZIP;
	}

	bool CanHandleFile(const string &fpath) override {
		return IsFileCompressed(fpath, FileCompressionType::GZIP);
	}

	static bool CheckIsZip(const char *length, idx_t size);

	//! Consumes a byte stream as a gzip string, returning the decompressed string
	static string UncompressGZIPString(const string &in);
	static string UncompressGZIPString(const char *length, idx_t size);

	unique_ptr<StreamWrapper> CreateStream() override;
	idx_t InBufferSize() override;
	idx_t OutBufferSize() override;
	unique_ptr<PiecedFile> CreatePieces() override;
};

static constexpr const uint8_t GZIP_COMPRESSION_DEFLATE = 0x08;

static constexpr const uint8_t GZIP_HEADER_MINSIZE = 10;
static constexpr const uint8_t GZIP_FOOTER_SIZE = 8;

} // namespace duckdb
