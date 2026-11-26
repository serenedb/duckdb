#include "zstd_file_system.hpp"

#include <exception>
#include <utility>

#include "zstd.h"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/logging/logger.hpp"

namespace duckdb {

namespace {

struct ZstdStreamWrapper : public StreamWrapper {
	~ZstdStreamWrapper() override;

	CompressedFile *file = nullptr;
	duckdb_zstd::ZSTD_DStream *zstd_stream_ptr = nullptr;

public:
	void Initialize(QueryContext context, CompressedFile &file, bool write) override;
	bool Read(StreamData &stream_data) override;

	void Close() override;
};

ZstdStreamWrapper::~ZstdStreamWrapper() {
	if (Exception::UncaughtException()) {
		return;
	}
	try {
		Close();
	} catch (std::exception &ex) {
		if (file && file->child_handle) {
			// FIXME: Make any log context available here.
			ErrorData data(ex);
			try {
				const auto logger = file->child_handle->logger;
				if (logger) {
					DUCKDB_LOG_ERROR(logger, "ZstdStreamWrapper::~ZstdStreamWrapper()\t\t" + data.Message());
				}
			} catch (...) { // NOLINT
			}
		}
	} catch (...) { // NOLINT
	}
}

void ZstdStreamWrapper::Initialize(QueryContext context, CompressedFile &file, bool write) {
	D_ASSERT(!zstd_stream_ptr);
	D_ASSERT(!write);
	this->file = &file;
	zstd_stream_ptr = duckdb_zstd::ZSTD_createDStream();
}

bool ZstdStreamWrapper::Read(StreamData &sd) {
	duckdb_zstd::ZSTD_inBuffer in_buffer;
	duckdb_zstd::ZSTD_outBuffer out_buffer;

	in_buffer.src = sd.in_buff_start;
	in_buffer.size = sd.in_buff_end - sd.in_buff_start;
	in_buffer.pos = 0;

	out_buffer.dst = sd.out_buff_start;
	out_buffer.size = sd.out_buf_size;
	out_buffer.pos = 0;

	auto res = duckdb_zstd::ZSTD_decompressStream(zstd_stream_ptr, &out_buffer, &in_buffer);
	if (duckdb_zstd::ZSTD_isError(res)) {
		throw IOException(duckdb_zstd::ZSTD_getErrorName(res));
	}

	sd.in_buff_start = (data_ptr_t)in_buffer.src + in_buffer.pos;  // NOLINT
	sd.in_buff_end = (data_ptr_t)in_buffer.src + in_buffer.size;   // NOLINT
	sd.out_buff_end = (data_ptr_t)out_buffer.dst + out_buffer.pos; // NOLINT
	return false;
}

void ZstdStreamWrapper::Close() {
	if (zstd_stream_ptr) {
		duckdb_zstd::ZSTD_freeDStream(zstd_stream_ptr);
	}
	zstd_stream_ptr = nullptr;
}

void CheckZstd(size_t result) {
	if (duckdb_zstd::ZSTD_isError(result)) {
		throw IOException(duckdb_zstd::ZSTD_getErrorName(result));
	}
}

class ZstdPieceCompressor : public PieceCompressor {
public:
	ZstdPieceCompressor() : context(duckdb_zstd::ZSTD_createCCtx()) {
		if (!context) {
			throw InternalException("Failed to create a ZSTD compression context");
		}
	}
	~ZstdPieceCompressor() override {
		duckdb_zstd::ZSTD_freeCCtx(context);
	}

	void Compress(std::span<const std::span<const data_t>> inputs, bool last, CompressedPiece &piece) override {
		idx_t total = 0;
		for (auto &input : inputs) {
			total += input.size();
		}
		piece.size = 0;
		piece.Reserve(duckdb_zstd::ZSTD_compressBound(total));
		piece.input_size = total;
		piece.last = last;
		if (inputs.size() == 1) {
			const auto written = duckdb_zstd::ZSTD_compress2(context, piece.data.get(), piece.capacity,
			                                                 inputs[0].data(), inputs[0].size());
			CheckZstd(written);
			piece.size = written;
			return;
		}
		CheckZstd(duckdb_zstd::ZSTD_CCtx_reset(context, duckdb_zstd::ZSTD_reset_session_only));
		CheckZstd(duckdb_zstd::ZSTD_CCtx_setPledgedSrcSize(context, total));
		for (idx_t i = 0; i < inputs.size(); i++) {
			Stream(inputs[i], i + 1 == inputs.size() ? duckdb_zstd::ZSTD_e_end : duckdb_zstd::ZSTD_e_continue, piece);
		}
		if (inputs.empty()) {
			Stream({}, duckdb_zstd::ZSTD_e_end, piece);
		}
	}

private:
	void Stream(std::span<const data_t> input, duckdb_zstd::ZSTD_EndDirective mode, CompressedPiece &piece) {
		duckdb_zstd::ZSTD_inBuffer in_buffer {input.data(), input.size(), 0};
		while (true) {
			duckdb_zstd::ZSTD_outBuffer out_buffer {piece.data.get() + piece.size, piece.capacity - piece.size, 0};
			const auto remaining = duckdb_zstd::ZSTD_compressStream2(context, &out_buffer, &in_buffer, mode);
			CheckZstd(remaining);
			piece.size += out_buffer.pos;
			const auto done = mode == duckdb_zstd::ZSTD_e_end ? remaining == 0 : in_buffer.pos == in_buffer.size;
			if (done) {
				return;
			}
			piece.Reserve(MaxValue<idx_t>(remaining, duckdb_zstd::ZSTD_CStreamOutSize()));
		}
	}

	duckdb_zstd::ZSTD_CCtx *context;
};

class ZstdPiecedFile : public PiecedFile {
public:
	unique_ptr<PieceCompressor> CreateCompressor() const override {
		return make_uniq<ZstdPieceCompressor>();
	}
};

struct ZStdFileSystemHolder {
	ZStdFileSystem zstd_fs;
};

class ZStdFile : private ZStdFileSystemHolder, public CompressedFile {
public:
	ZStdFile(QueryContext context, unique_ptr<FileHandle> child_handle_p, const string &path, bool write)
	    : CompressedFile(zstd_fs, std::move(child_handle_p), path) {
		Initialize(context, write);
	}

	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType::ZSTD;
	}
};

} // namespace

unique_ptr<FileHandle> ZStdFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                          bool write) {
	try {
		auto path = handle->path;
		return make_uniq<ZStdFile>(context, std::move(handle), path, write);
	} catch (...) {
		auto error = std::current_exception();
		if (handle) {
			try {
				handle->AbortWrite();
			} catch (...) { // NOLINT
			}
		}
		std::rethrow_exception(error);
	}
}

unique_ptr<StreamWrapper> ZStdFileSystem::CreateStream() {
	return make_uniq<ZstdStreamWrapper>();
}

unique_ptr<PiecedFile> ZStdFileSystem::CreatePieces() {
	return make_uniq<ZstdPiecedFile>();
}

idx_t ZStdFileSystem::InBufferSize() {
	return duckdb_zstd::ZSTD_DStreamInSize();
}

idx_t ZStdFileSystem::OutBufferSize() {
	return duckdb_zstd::ZSTD_DStreamOutSize();
}

int64_t ZStdFileSystem::DefaultCompressionLevel() {
	return duckdb_zstd::ZSTD_defaultCLevel();
}

int64_t ZStdFileSystem::MinimumCompressionLevel() {
	return duckdb_zstd::ZSTD_minCLevel();
}

int64_t ZStdFileSystem::MaximumCompressionLevel() {
	return duckdb_zstd::ZSTD_maxCLevel();
}

} // namespace duckdb
