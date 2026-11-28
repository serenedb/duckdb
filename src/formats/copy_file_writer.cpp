#include "duckdb/formats/copy_file_writer.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstring>

namespace duckdb {

namespace {

class OwnedBytes final : public AsyncWritePayload {
public:
	OwnedBytes(const_data_ptr_t data_p, idx_t size_p)
	    : data(make_unsafe_uniq_array_uninitialized<data_t>(size_p)), size(size_p) {
		memcpy(data.get(), data_p, size);
	}

	data_ptr_t Ptr() override {
		return data.get();
	}
	idx_t Size() const override {
		return size;
	}

private:
	unsafe_unique_array<data_t> data;
	idx_t size;
};

unique_ptr<AsyncFileWriter> OpenCopyFile(ClientContext &context, FileSystem &fs, const string &path,
                                         const FileCompressionType &compression) {
	auto flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW | compression;
	if (!FileSystem::IsRemoteFile(path) && !fs.FileExists(path) && !fs.IsPipe(path)) {
		flags |= FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
	}
	return make_uniq<AsyncFileWriter>(QueryContext(context), fs, path, flags, true);
}

} // namespace

CopyFileWriter::CopyFileWriter(ClientContext &context, const string &path, const FileCompressionType &compression,
                               string separator_p)
    : CopyFileWriter(context, FileSystem::GetFileSystem(context), path, compression, std::move(separator_p)) {
}

CopyFileWriter::CopyFileWriter(ClientContext &context, FileSystem &fs, const string &path,
                               const FileCompressionType &compression, string separator_p)
    : pieced(CreatePiecedFile(fs, compression, path)),
      stream_compressed(!pieced && compression != FileCompressionType::UNCOMPRESSED),
      separator(std::move(separator_p)) {
	file = OpenCopyFile(context, fs, path, pieced ? FileCompressionType::UNCOMPRESSED : compression);
	if (pieced) {
		compressor = pieced->CreateCompressor();
		const auto head = pieced->Head();
		file->WriteData(const_data_ptr_cast(head.data()), head.size());
		written = head.size();
	}
}

CopyFileWriter::~CopyFileWriter() = default;

void CopyFileWriter::WriteFrame(const_data_ptr_t data, idx_t size) {
	unique_lock<mutex> guard(lock);
	EnqueueBytes(data, size);
	Drain(guard);
}

unique_ptr<CompressedPiece> CopyFileWriter::Compress(std::span<const std::span<const data_t>> rows,
                                                     unique_ptr<PieceCompressor> &rows_compressor) const {
	if (!rows_compressor) {
		rows_compressor = pieced->CreateCompressor();
	}
	auto piece = make_uniq<CompressedPiece>();
	rows_compressor->Compress(rows, false, *piece);
	return piece;
}

void CopyFileWriter::WriteRows(unique_ptr<CompressedPiece> rows) {
	unique_lock<mutex> guard(lock);
	Separate();
	if (rows->input_size != 0) {
		EnqueuePiece(std::move(rows));
	}
	Drain(guard);
}

void CopyFileWriter::WriteRows(vector<unique_ptr<AsyncWritePayload>> rows) {
	unique_lock<mutex> guard(lock);
	Separate();
	for (auto &row : rows) {
		Enqueue(std::move(row));
	}
	Drain(guard);
}

void CopyFileWriter::WriteRows(const_data_ptr_t data, idx_t size) {
	unique_lock<mutex> guard(lock);
	Separate();
	if (pieced || writing || error) {
		EnqueueBytes(data, size);
		Drain(guard);
		return;
	}
	written += size;
	Write(guard, data, size);
}

idx_t CopyFileWriter::FileSize() {
	unique_lock<mutex> guard(lock);
	if (!stream_compressed) {
		return written;
	}
	const auto idle = [&]() {
		return !writing;
	};
	lock.Await(absl::Condition(&idle));
	Drain(guard);
	return file->GetPhysicalFileSize();
}

idx_t CopyFileWriter::Close() {
	unique_lock<mutex> guard(lock);
	const auto idle = [&]() {
		return !writing;
	};
	lock.Await(absl::Condition(&idle));
	if (pieced) {
		const auto tail = pieced->Tail();
		Enqueue(make_uniq<OwnedBytes>(const_data_ptr_cast(tail.data()), tail.size()));
	}
	Drain(guard);
	file->Close();
	return written;
}

void CopyFileWriter::Separate() {
	if (!std::exchange(wrote_rows, true) || separator.empty()) {
		return;
	}
	if (!pieced) {
		Enqueue(make_uniq<OwnedBytes>(const_data_ptr_cast(separator.data()), separator.size()));
		return;
	}
	if (!separator_piece) {
		separator_piece = make_uniq<CompressedPiece>();
		compressor->Compress(const_data_ptr_cast(separator.data()), separator.size(), *separator_piece);
	}
	pieced->Append(*separator_piece);
	Enqueue(make_uniq<OwnedBytes>(separator_piece->data.get(), separator_piece->size));
}

void CopyFileWriter::Enqueue(unique_ptr<AsyncWritePayload> payload) {
	const auto size = payload->Size();
	if (size == 0) {
		return;
	}
	queued.push_back(std::move(payload));
	queued_bytes += size;
	written += size;
}

void CopyFileWriter::EnqueueBytes(const_data_ptr_t data, idx_t size) {
	if (size == 0) {
		return;
	}
	if (!pieced) {
		Enqueue(make_uniq<OwnedBytes>(data, size));
		return;
	}
	auto piece = make_uniq<CompressedPiece>();
	compressor->Compress(data, size, *piece);
	EnqueuePiece(std::move(piece));
}

void CopyFileWriter::EnqueuePiece(unique_ptr<CompressedPiece> piece) {
	pieced->Append(*piece);
	Enqueue(unique_ptr<AsyncWritePayload>(std::move(piece)));
}

void CopyFileWriter::Drain(unique_lock<mutex> &guard) {
	if (error) {
		std::rethrow_exception(error);
	}
	if (writing) {
		const auto has_room = [&]() {
			return queued_bytes < MAX_QUEUED_BYTES || error;
		};
		lock.Await(absl::Condition(&has_room));
		if (error) {
			std::rethrow_exception(error);
		}
		return;
	}
	Write(guard, nullptr, 0);
}

void CopyFileWriter::Write(unique_lock<mutex> &guard, const_data_ptr_t data, idx_t size) {
	writing = true;
	while (!queued.empty() || size != 0) {
		auto batch = std::move(queued);
		queued.clear();
		queued_bytes = 0;
		guard.unlock();
		try {
			for (auto &payload : batch) {
				file->WriteData(std::move(payload));
			}
			if (size != 0) {
				file->WriteData(data, size);
				size = 0;
			}
			file->ApplyBackpressure();
		} catch (...) {
			guard.lock();
			writing = false;
			error = std::current_exception();
			throw;
		}
		guard.lock();
	}
	writing = false;
}

} // namespace duckdb
