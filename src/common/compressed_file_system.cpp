#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

static constexpr idx_t DIRECT_READ_BYTES = 1 << 16;

data_ptr_t CompressedPiece::Reserve(idx_t count) {
	if (capacity - size < count) {
		auto grown = MaxValue(size + count, 2 * capacity);
		auto bytes = make_unsafe_uniq_array_uninitialized<data_t>(grown);
		if (size > 0) {
			memcpy(bytes.get(), data.get(), size);
		}
		data = std::move(bytes);
		capacity = grown;
	}
	return data.get() + size;
}

PieceCompressor::~PieceCompressor() {
}

void PieceCompressor::Compress(const_data_ptr_t data, idx_t size, CompressedPiece &piece, bool last) {
	const std::span<const data_t> input(data, size);
	Compress(std::span<const std::span<const data_t>>(&input, 1), last, piece);
}

PiecedFile::~PiecedFile() {
}

string PiecedFile::Head() const {
	return string();
}

void PiecedFile::Append(const CompressedPiece &) {
}

string PiecedFile::Tail() const {
	return string();
}

StreamWrapper::~StreamWrapper() {
}

void StreamWrapper::FinalizeRead(StreamData &) {
}

void StreamWrapper::Write(CompressedFile &file, StreamData &, data_ptr_t, int64_t) {
	throw NotImplementedException("Writing is not supported for compressed file \"%s\"", file.path);
}

void StreamWrapper::AbortWrite() {
	Close();
}

bool CompressedFileSystem::CanHandleFile(const string &fpath) {
	return false;
}

unique_ptr<PiecedFile> CompressedFileSystem::CreatePieces() {
	return nullptr;
}

bool CompressedFileSystem::FindPieces(const_data_ptr_t, idx_t, vector<PieceSpan> &) {
	return false;
}

unique_ptr<PieceDecompressor> CompressedFileSystem::CreateDecompressor() {
	return nullptr;
}

PieceDecompressor::~PieceDecompressor() {
}

unique_ptr<PiecedFile> CreatePiecedFile(FileSystem &fs, const FileCompressionType &compression, const string &path) {
	auto compressed = fs.GetCompressedFileSystem(compression, path);
	return compressed ? compressed->CreatePieces() : nullptr;
}

CompressedFile::CompressedFile(CompressedFileSystem &fs, unique_ptr<FileHandle> child_handle_p, const string &path) try
    : FileHandle(fs, path, child_handle_p->GetFlags()), compressed_fs(fs), child_handle(std::move(child_handle_p)) {
	// The real on-disk I/O happens on the (compressed) child handle; attribute the bytes there instead of
	// double-counting the uncompressed bytes that pass through this wrapper handle.
	track_io = false;

} catch (...) {
	auto error = std::current_exception();
	if (child_handle_p) {
		try {
			child_handle_p->AbortWrite();
		} catch (...) { // NOLINT
		}
	}
	std::rethrow_exception(error);
}

CompressedFile::~CompressedFile() {
	try {
		if (write && !initialized) {
			AbortCompressedWrite();
		} else {
			Close();
		}
	} catch (std::exception &ex) {
		if (child_handle) {
			// FIXME: Make any log context available here.
			ErrorData data(ex);
			try {
				const auto logger = child_handle->logger;
				if (logger) {
					DUCKDB_LOG_ERROR(logger, "CompressedFile::~CompressedFile()\t\t" + data.Message());
				}
			} catch (...) { // NOLINT
			}
		}
	} catch (...) { // NOLINT
	}
}

void CompressedFile::Initialize(QueryContext context, bool write) {
	Clear();
	initialized = false;

	this->context = context;
	this->write = write;
	compressed_bytes_read = 0;
	compressed_bytes_consumed.store(0, std::memory_order_relaxed);
	if (write) {
		pieced = compressed_fs.CreatePieces();
	}
	if (pieced) {
		compressor = pieced->CreateCompressor();
		auto head = pieced->Head();
		if (!head.empty()) {
			child_handle->Write(context, head.data(), head.size());
		}
		initialized = true;
		return;
	}
	stream_data.in_buf_size = compressed_fs.InBufferSize();
	stream_data.out_buf_size = compressed_fs.OutBufferSize();
	stream_data.in_buff = make_unsafe_uniq_array_uninitialized<data_t>(stream_data.in_buf_size);
	stream_data.in_buff_start = stream_data.in_buff.get();
	stream_data.in_buff_end = stream_data.in_buff.get();
	stream_data.out_buff = make_unsafe_uniq_array_uninitialized<data_t>(stream_data.out_buf_size);
	stream_data.out_buff_start = stream_data.out_buff.get();
	stream_data.out_buff_end = stream_data.out_buff.get();
	stream_data.out_buff_limit = stream_data.out_buff.get() + stream_data.out_buf_size;

	stream_wrapper = compressed_fs.CreateStream();
	stream_wrapper->Initialize(context, *this, write);
	initialized = true;
}

idx_t CompressedFile::GetProgress() {
	return compressed_bytes_consumed.load(std::memory_order_relaxed);
}

bool CompressedFile::IsPipe() {
	return child_handle->IsPipe();
}

int64_t CompressedFile::ReadData(void *buffer, int64_t remaining) {
	idx_t total_read = 0;
	while (true) {
		// first check if there are input bytes available in the output buffers
		if (stream_data.out_buff_start != stream_data.out_buff_end) {
			// there is! copy it into the output buffer
			auto available =
			    MinValue<idx_t>(UnsafeNumericCast<idx_t>(remaining),
			                    UnsafeNumericCast<idx_t>(stream_data.out_buff_end - stream_data.out_buff_start));
			memcpy(static_cast<data_ptr_t>(buffer) + total_read, stream_data.out_buff_start, available);

			// increment the total read variables as required
			stream_data.out_buff_start += available;
			total_read += available;
			remaining = UnsafeNumericCast<int64_t>(UnsafeNumericCast<idx_t>(remaining) - available);
			if (remaining == 0) {
				// done! read enough
				return UnsafeNumericCast<int64_t>(total_read);
			}
		}
		if (!stream_wrapper) {
			return UnsafeNumericCast<int64_t>(total_read);
		}
		// ran out of buffer: read more data from the child stream
		stream_data.out_buff_start = stream_data.out_buff.get();
		stream_data.out_buff_end = stream_data.out_buff.get();
		stream_data.out_buff_limit = stream_data.out_buff.get() + stream_data.out_buf_size;
		D_ASSERT(stream_data.in_buff_start <= stream_data.in_buff_end);
		D_ASSERT(stream_data.in_buff_end <= stream_data.in_buff_start + stream_data.in_buf_size);

		// read more input if none available
		if (stream_data.in_buff_start == stream_data.in_buff_end) {
			// empty input buffer: refill from the start
			stream_data.in_buff_start = stream_data.in_buff.get();
			stream_data.in_buff_end = stream_data.in_buff_start;
			auto sz = child_handle->Read(context, stream_data.in_buff.get(), stream_data.in_buf_size);
			if (sz <= 0) {
				stream_wrapper->FinalizeRead(stream_data);
				stream_wrapper.reset();
				compressed_bytes_consumed.store(compressed_bytes_read, std::memory_order_relaxed);
				break;
			} else {
				stream_data.in_buff_end = stream_data.in_buff_start + sz;
				compressed_bytes_read += UnsafeNumericCast<idx_t>(sz);
			}
		}

		const bool direct = UnsafeNumericCast<idx_t>(remaining) >= DIRECT_READ_BYTES;
		if (direct) {
			stream_data.out_buff_start = static_cast<data_ptr_t>(buffer) + total_read;
			stream_data.out_buff_end = stream_data.out_buff_start;
			stream_data.out_buff_limit =
			    stream_data.out_buff_start +
			    MinValue<idx_t>(UnsafeNumericCast<idx_t>(remaining), NumericLimits<uint32_t>::Maximum());
		}
		auto finished = stream_wrapper->Read(stream_data);
		if (direct) {
			const auto produced = UnsafeNumericCast<idx_t>(stream_data.out_buff_end - stream_data.out_buff_start);
			total_read += produced;
			remaining = UnsafeNumericCast<int64_t>(UnsafeNumericCast<idx_t>(remaining) - produced);
			stream_data.out_buff_start = stream_data.out_buff.get();
			stream_data.out_buff_end = stream_data.out_buff.get();
			stream_data.out_buff_limit = stream_data.out_buff.get() + stream_data.out_buf_size;
		}
		// the input that remains in the buffer has not been consumed by the decompressor yet
		auto unconsumed = static_cast<idx_t>(stream_data.in_buff_end - stream_data.in_buff_start);
		compressed_bytes_consumed.store(compressed_bytes_read - MinValue(unconsumed, compressed_bytes_read),
		                                std::memory_order_relaxed);
		if (finished) {
			stream_wrapper.reset();
		}
		if (remaining == 0) {
			return UnsafeNumericCast<int64_t>(total_read);
		}
	}
	return UnsafeNumericCast<int64_t>(total_read);
}

int64_t CompressedFile::WriteData(data_ptr_t buffer, int64_t nr_bytes) {
	if (!pieced) {
		stream_wrapper->Write(*this, stream_data, buffer, nr_bytes);
		return nr_bytes;
	}
	const auto total = UnsafeNumericCast<idx_t>(nr_bytes);
	idx_t offset = 0;
	while (offset < total) {
		if (pending_size == 0 && total - offset >= PIECE_BYTES) {
			WritePiece(buffer + offset, PIECE_BYTES, false);
			offset += PIECE_BYTES;
			continue;
		}
		if (!pending) {
			pending = make_unsafe_uniq_array_uninitialized<data_t>(PIECE_BYTES);
		}
		const auto count = MinValue(PIECE_BYTES - pending_size, total - offset);
		memcpy(pending.get() + pending_size, buffer + offset, count);
		pending_size += count;
		offset += count;
		if (pending_size == PIECE_BYTES) {
			WritePiece(pending.get(), pending_size, false);
			pending_size = 0;
		}
	}
	return nr_bytes;
}

void CompressedFile::WritePiece(const_data_ptr_t data, idx_t size, bool last) {
	compressor->Compress(data, size, piece, last);
	child_handle->Write(context, piece.data.get(), piece.size);
	pieced->Append(piece);
}

// Clear does most of the heavy lifting of a close, but leaves the child_handle intact. Specifically it is separated out
// to support upstream Reset() calls from the FileSystem, which should tear down / reset ephemeral state but leave
// persistent state (child_handle) intact.
void CompressedFile::Clear() {
	if (pieced) {
		if (pending_size > 0) {
			WritePiece(pending.get(), pending_size, true);
			pending_size = 0;
		}
		auto tail = pieced->Tail();
		pieced.reset();
		compressor.reset();
		pending.reset();
		if (!tail.empty()) {
			child_handle->Write(context, tail.data(), tail.size());
		}
	}
	if (stream_wrapper) {
		stream_wrapper->Close();
		stream_wrapper.reset();
	}
	ResetStreamData();
}

void CompressedFile::ResetStreamData() {
	stream_data.in_buff.reset();
	stream_data.out_buff.reset();
	stream_data.out_buff_start = nullptr;
	stream_data.out_buff_end = nullptr;
	stream_data.out_buff_limit = nullptr;
	stream_data.in_buff_start = nullptr;
	stream_data.in_buff_end = nullptr;
	stream_data.in_buf_size = 0;
	stream_data.out_buf_size = 0;
}

void CompressedFile::Close() {
	try {
		Clear();
	} catch (...) {
		auto error = std::current_exception();
		try {
			AbortCompressedWrite();
		} catch (...) { // NOLINT
		}
		std::rethrow_exception(error);
	}

	auto child = std::move(child_handle);
	if (child) {
		child->Close();
	}
}

void CompressedFile::AbortCompressedWrite() {
	initialized = false;
	pieced.reset();
	compressor.reset();
	pending.reset();
	pending_size = 0;
	std::exception_ptr error;
	auto wrapper = std::move(stream_wrapper);
	if (wrapper) {
		try {
			wrapper->AbortWrite();
		} catch (...) {
			error = std::current_exception();
		}
	}
	ResetStreamData();
	auto child = std::move(child_handle);
	if (child) {
		try {
			child->AbortWrite();
		} catch (...) {
			if (!error) {
				error = std::current_exception();
			}
		}
	}
	if (error) {
		std::rethrow_exception(error);
	}
}

int64_t CompressedFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return compressed_file.ReadData(buffer, nr_bytes);
}

int64_t CompressedFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return compressed_file.WriteData(data_ptr_cast(buffer), nr_bytes);
}

void CompressedFileSystem::Reset(FileHandle &handle) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	compressed_file.child_handle->Reset();
	// Preserve the query context across a reset so re-reads (e.g. the scan after the CSV sniffer) stay attributed.
	compressed_file.Initialize(compressed_file.context, compressed_file.write);
}

int64_t CompressedFileSystem::GetFileSize(FileHandle &handle) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return NumericCast<int64_t>(compressed_file.child_handle->GetFileSize());
}

bool CompressedFileSystem::OnDiskFile(FileHandle &handle) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return compressed_file.child_handle->OnDiskFile();
}

bool CompressedFileSystem::CanSeek() {
	return false;
}

void CompressedFileSystem::AbortFileWrite(FileHandle &handle) {
	handle.Cast<CompressedFile>().AbortCompressedWrite();
}

} // namespace duckdb
