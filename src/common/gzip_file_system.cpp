#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include "miniz_wrapper.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/logging/logger.hpp"

namespace duckdb {

namespace {

struct MiniZStreamWrapper : public StreamWrapper {
	~MiniZStreamWrapper() override;

	CompressedFile *file = nullptr;
	unique_ptr<z_stream> mz_stream_ptr;
	bool started = false;
	bool in_member = false;
	bool member_ended = false;

public:
	void Initialize(QueryContext context, CompressedFile &file, bool write) override;

	bool Read(StreamData &stream_data) override;
	void FinalizeRead(StreamData &stream_data) override;

	void Close() override;
};

MiniZStreamWrapper::~MiniZStreamWrapper() {
	// avoid closing if destroyed during stack unwinding
	if (Exception::UncaughtException()) {
		return;
	}
	try {
		MiniZStreamWrapper::Close();
	} catch (std::exception &ex) {
		if (file && file->child_handle) {
			// FIXME: Make any log context available here.
			ErrorData data(ex);
			try {
				const auto logger = file->child_handle->logger;
				if (logger) {
					DUCKDB_LOG_ERROR(logger, "MiniZStreamWrapper::~MiniZStreamWrapper()\t\t" + data.Message())
				}
			} catch (...) { // NOLINT
			}
		}
	} catch (...) { // NOLINT
	}
}

void MiniZStreamWrapper::Initialize(QueryContext, CompressedFile &file, bool write) {
	D_ASSERT(mz_stream_ptr == nullptr);
	D_ASSERT(!write);
	this->file = &file;
	mz_stream_ptr = make_uniq<z_stream>();
	memset(mz_stream_ptr.get(), 0, sizeof(z_stream));
	if (inflateInit2(mz_stream_ptr.get(), MAX_WBITS + 16) != Z_OK) {
		throw InternalException("Failed to initialize miniz");
	}
}

bool MiniZStreamWrapper::Read(StreamData &sd) {
	if (sd.in_buff_start == sd.in_buff_end) {
		return false;
	}
	auto &stream = *mz_stream_ptr;
	if (member_ended) {
		if (inflateReset(&stream) != Z_OK) {
			throw InternalException("Failed to initialize miniz");
		}
		member_ended = false;
	}
	started = true;
	stream.next_in = sd.in_buff_start;
	D_ASSERT(sd.in_buff_end - sd.in_buff_start < NumericLimits<int32_t>::Maximum());
	stream.avail_in = static_cast<uint32_t>(sd.in_buff_end - sd.in_buff_start);
	stream.next_out = data_ptr_cast(sd.out_buff_end);
	stream.avail_out = static_cast<uint32_t>((sd.out_buff.get() + sd.out_buf_size) - sd.out_buff_end);
	const auto ret = inflate(&stream, Z_NO_FLUSH);
	if (ret != Z_OK && ret != Z_STREAM_END) {
		const string reason = stream.msg ? stream.msg : zError(ret);
		if (reason == "incorrect header check") {
			throw IOException("Input is not a GZIP stream: %s", file->path);
		}
		throw IOException("Failed to decode gzip stream %s: %s", file->path, reason);
	}
	in_member = ret == Z_OK;
	member_ended = ret == Z_STREAM_END;
	sd.in_buff_start = (data_ptr_t)stream.next_in; // NOLINT
	sd.in_buff_end = sd.in_buff_start + stream.avail_in;
	sd.out_buff_end = data_ptr_cast(stream.next_out);
	D_ASSERT(sd.out_buff_end + stream.avail_out == sd.out_buff.get() + sd.out_buf_size);
	return false;
}

void MiniZStreamWrapper::FinalizeRead(StreamData &) {
	if (!started) {
		throw IOException("Input is not a GZIP stream: %s", file->path);
	}
	if (in_member) {
		throw IOException("Unexpected end of GZIP stream: %s", file->path);
	}
}

void MiniZStreamWrapper::Close() {
	if (!mz_stream_ptr) {
		return;
	}
	inflateEnd(mz_stream_ptr.get());
	mz_stream_ptr = nullptr;
	file = nullptr;
}

class GZipPieceCompressor : public PieceCompressor {
public:
	GZipPieceCompressor() {
		memset(&stream, 0, sizeof(stream));
		if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, MEMORY_LEVEL, Z_DEFAULT_STRATEGY) !=
		    Z_OK) {
			throw InternalException("Failed to initialize the GZIP compressor");
		}
	}
	~GZipPieceCompressor() override {
		deflateEnd(&stream);
	}

	void Compress(std::span<const std::span<const data_t>> inputs, bool last, CompressedPiece &piece) override {
		if (deflateReset(&stream) != Z_OK) {
			throw InternalException("Failed to reset the GZIP compressor");
		}
		idx_t total = 0;
		auto crc = crc32_z(0, nullptr, 0);
		for (auto &input : inputs) {
			total += input.size();
			crc = crc32_z(crc, input.data(), input.size());
		}
		piece.size = 0;
		piece.Reserve(deflateBound(&stream, total) + FLUSH_BYTES);
		const auto flush = last ? Z_FINISH : Z_SYNC_FLUSH;
		for (idx_t i = 0; i < inputs.size(); i++) {
			Deflate(inputs[i], i + 1 == inputs.size() ? flush : Z_NO_FLUSH, piece);
		}
		if (inputs.empty()) {
			Deflate({}, flush, piece);
		}
		piece.input_size = total;
		piece.input_crc = UnsafeNumericCast<uint32_t>(crc);
		piece.last = last;
	}

private:
	static constexpr int MEMORY_LEVEL = 8;
	static constexpr idx_t FLUSH_BYTES = 16;

	void Deflate(std::span<const data_t> input, int flush, CompressedPiece &piece) {
		auto data = input.data();
		auto remaining = input.size();
		while (true) {
			const auto step = MinValue<idx_t>(remaining, NumericLimits<uInt>::Maximum());
			stream.next_in = data;
			stream.avail_in = UnsafeNumericCast<uInt>(step);
			const auto last = step == remaining;
			do {
				const auto room = MinValue<idx_t>(piece.capacity - piece.size, NumericLimits<uInt>::Maximum());
				stream.next_out = piece.data.get() + piece.size;
				stream.avail_out = UnsafeNumericCast<uInt>(room);
				const auto result = deflate(&stream, last ? flush : Z_NO_FLUSH);
				if (result != Z_OK && result != Z_BUF_ERROR && result != Z_STREAM_END) {
					throw InternalException("Failed to compress GZIP block");
				}
				piece.size += room - stream.avail_out;
				if (stream.avail_out == 0) {
					piece.Reserve(MaxValue<idx_t>(stream.avail_in, FLUSH_BYTES));
				}
			} while (stream.avail_in > 0 || stream.avail_out == 0);
			data += step;
			remaining -= step;
			if (last) {
				return;
			}
		}
	}

	z_stream stream;
};

class GZipPiecedFile : public PiecedFile {
public:
	unique_ptr<PieceCompressor> CreateCompressor() const override {
		return make_uniq<GZipPieceCompressor>();
	}
	string Head() const override {
		unsigned char header[GZIP_HEADER_MINSIZE];
		MiniZStream::InitializeGZIPHeader(header);
		return string(const_char_ptr_cast(header), GZIP_HEADER_MINSIZE);
	}
	void Append(const CompressedPiece &piece) override {
		crc = crc32_combine(crc, piece.input_crc, UnsafeNumericCast<z_off64_t>(piece.input_size));
		size += piece.input_size;
		ended = piece.last;
	}
	string Tail() const override {
		unsigned char footer[GZIP_FOOTER_SIZE];
		MiniZStream::InitializeGZIPFooter(footer, crc, size);
		string tail = ended ? string() : string(const_char_ptr_cast(FINAL_BLOCK), sizeof(FINAL_BLOCK));
		return tail + string(const_char_ptr_cast(footer), GZIP_FOOTER_SIZE);
	}

private:
	static constexpr unsigned char FINAL_BLOCK[] = {0x03, 0x00};

	uLong crc = 0;
	idx_t size = 0;
	bool ended = false;
};

struct GZipFileSystemHolder {
	GZipFileSystem gzip_fs;
};

class GZipFile : private GZipFileSystemHolder, public CompressedFile {
public:
	GZipFile(QueryContext context, unique_ptr<FileHandle> child_handle_p, const string &path, bool write)
	    : CompressedFile(gzip_fs, std::move(child_handle_p), path) {
		Initialize(context, write);
	}
	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType::GZIP;
	}
};

} // namespace

void GZipFileSystem::VerifyGZIPHeader(uint8_t gzip_hdr[], idx_t read_count, optional_ptr<CompressedFile> source_file) {
	// include the filename in the error message if known
	string file_info = source_file ? ": " + source_file->path : "";

	// check for incorrectly formatted files
	if (read_count != GZIP_HEADER_MINSIZE) {
		throw IOException("Input is not a GZIP stream" + file_info);
	}
	if (gzip_hdr[0] != 0x1F || gzip_hdr[1] != 0x8B) { // magic header
		throw IOException("Input is not a GZIP stream" + file_info);
	}
	if (gzip_hdr[2] != GZIP_COMPRESSION_DEFLATE) { // compression method
		throw IOException("Unsupported GZIP compression method" + file_info);
	}
	if (gzip_hdr[3] & GZIP_FLAG_UNSUPPORTED) {
		throw IOException("Unsupported GZIP archive" + file_info);
	}
}

bool GZipFileSystem::CheckIsZip(const char *data, duckdb::idx_t size) {
	if (size < GZIP_HEADER_MINSIZE) {
		return false;
	}

	auto data_ptr = reinterpret_cast<const uint8_t *>(data);
	if (data_ptr[0] != 0x1F || data_ptr[1] != 0x8B) {
		return false;
	}

	if (data_ptr[2] != GZIP_COMPRESSION_DEFLATE) {
		return false;
	}

	return true;
}

string GZipFileSystem::UncompressGZIPString(const string &in) {
	return UncompressGZIPString(in.data(), in.size());
}

string GZipFileSystem::UncompressGZIPString(const char *data, idx_t size) {
	// decompress file
	auto body_ptr = data;

	auto mz_stream_ptr = make_uniq<z_stream>();
	memset(mz_stream_ptr.get(), 0, sizeof(z_stream));

	uint8_t gzip_hdr[GZIP_HEADER_MINSIZE];

	// check for incorrectly formatted files

	// TODO this is mostly the same as gzip_file_system.cpp
	if (size < GZIP_HEADER_MINSIZE) {
		throw IOException("Input is not a GZIP stream");
	}
	memcpy(gzip_hdr, body_ptr, GZIP_HEADER_MINSIZE);
	body_ptr += GZIP_HEADER_MINSIZE;
	GZipFileSystem::VerifyGZIPHeader(gzip_hdr, GZIP_HEADER_MINSIZE, nullptr);

	if (gzip_hdr[3] & GZIP_FLAG_EXTRA) {
		throw IOException("Extra field in a GZIP stream unsupported");
	}

	if (gzip_hdr[3] & GZIP_FLAG_NAME) {
		auto end_ptr = data + size;
		while (body_ptr < end_ptr) {
			if (*body_ptr++ == '\0') {
				break;
			}
		}
	}

	// stream is now set to beginning of payload data
	auto status = inflateInit2(mz_stream_ptr.get(), -MAX_WBITS);
	if (status != Z_OK) {
		throw InternalException("Failed to initialize miniz");
	}

	auto bytes_remaining = size - NumericCast<idx_t>(body_ptr - data);
	mz_stream_ptr->next_in = const_uchar_ptr_cast(body_ptr);
	mz_stream_ptr->avail_in = NumericCast<unsigned int>(bytes_remaining);

	string decompressed;

	while (status == Z_OK) {
		unsigned char decompress_buffer[BUFSIZ];
		mz_stream_ptr->next_out = decompress_buffer;
		mz_stream_ptr->avail_out = sizeof(decompress_buffer);
		status = inflate(mz_stream_ptr.get(), Z_NO_FLUSH);
		if (status != Z_STREAM_END && status != Z_OK) {
			inflateEnd(mz_stream_ptr.get());
			throw IOException("Failed to uncompress");
		}
		decompressed.append(char_ptr_cast(decompress_buffer), mz_stream_ptr->total_out - decompressed.size());
	}
	inflateEnd(mz_stream_ptr.get());

	if (decompressed.empty()) {
		throw IOException("Failed to uncompress");
	}
	return decompressed;
}

unique_ptr<FileHandle> GZipFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                          bool write) {
	try {
		auto path = handle->path;
		return make_uniq<GZipFile>(context, std::move(handle), path, write);
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

unique_ptr<StreamWrapper> GZipFileSystem::CreateStream() {
	return make_uniq<MiniZStreamWrapper>();
}

unique_ptr<PiecedFile> GZipFileSystem::CreatePieces() {
	return make_uniq<GZipPiecedFile>();
}

idx_t GZipFileSystem::InBufferSize() {
	return BUFFER_SIZE;
}

idx_t GZipFileSystem::OutBufferSize() {
	return BUFFER_SIZE;
}

} // namespace duckdb
