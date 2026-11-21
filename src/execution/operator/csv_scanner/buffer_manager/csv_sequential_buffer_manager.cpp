#include "duckdb/execution/operator/csv_scanner/csv_sequential_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"

namespace duckdb {

CSVSequentialBufferManager::CSVSequentialBufferManager(ClientContext &context_p, const CSVReaderOptions &options,
                                                       const OpenFileInfo &file_p, bool per_file_single_threaded_p,
                                                       unique_ptr<CSVFileHandle> file_handle_p)
    : CSVBufferManager(context_p, options, file_p, per_file_single_threaded_p, std::move(file_handle_p)) {
	is_pipe = file_handle->IsPipe();
	Initialize();
}

void CSVSequentialBufferManager::Initialize() {
	if (cached_buffers.empty()) {
		last_buffer = nullptr;
	}
}

shared_ptr<CSVBufferHandle> CSVSequentialBufferManager::GetBuffer(const idx_t pos) {
	{
		lock_guard<mutex> parallel_lock(main_mutex);
		if (pos < cached_buffers.size() && cached_buffers[pos]) {
			shared_ptr<CSVBufferHandle> pinned;
			if (cached_buffers[pos]->TryPin(pinned)) {
				UnpinPrevious(pos);
				return pinned;
			}
		}
	}
	lock_guard<mutex> read_guard(read_lock);
	while (true) {
		shared_ptr<CSVBuffer> previous;
		{
			lock_guard<mutex> parallel_lock(main_mutex);
			if (pos == 0 && done && cached_buffers.empty()) {
				if (is_pipe) {
					throw InvalidInputException("Recursive CTEs are not allowed when using piped csv files");
				}
				done = false;
				file_handle->Reset();
				Initialize();
			}
			if (pos < cached_buffers.size()) {
				UnpinPrevious(pos);
				return cached_buffers[pos]->Pin(*file_handle, has_seeked);
			}
			if (done) {
				return nullptr;
			}
			previous = last_buffer;
		}
		shared_ptr<CSVBuffer> next;
		if (!previous) {
			next = make_shared_ptr<CSVBuffer>(context, buffer_size, *file_handle, global_csv_pos);
		} else if (!previous->IsCSVFileLastBuffer()) {
			next = previous->Next(*file_handle, buffer_size, has_seeked);
		}
		lock_guard<mutex> parallel_lock(main_mutex);
		if (next) {
			last_buffer = next;
			cached_buffers.emplace_back(std::move(next));
		} else {
			previous->last_buffer = true;
			MarkDone(*previous);
		}
	}
}

idx_t CSVSequentialBufferManager::ReadInOrder(char *data, idx_t count, idx_t offset) {
	idx_t read = 0;
	while (read < count) {
		const idx_t position = offset + read;
		shared_ptr<CSVBufferHandle> pinned;
		{
			lock_guard<mutex> parallel_lock(main_mutex);
			const idx_t index = position / buffer_size;
			if (index >= cached_buffers.size() || !cached_buffers[index]) {
				break;
			}
			pinned = cached_buffers[index]->Pin(*file_handle, has_seeked);
		}
		const idx_t within = position % buffer_size;
		if (within >= pinned->actual_size) {
			break;
		}
		const idx_t piece = MinValue(count - read, pinned->actual_size - within);
		memcpy(data + read, pinned->Ptr() + within, piece);
		read += piece;
	}
	if (read == count) {
		return read;
	}
	lock_guard<mutex> read_guard(read_lock);
	{
		lock_guard<mutex> parallel_lock(main_mutex);
		if (last_buffer && last_buffer->IsCSVFileLastBuffer()) {
			MarkDone(*last_buffer);
		}
		if (done) {
			return read;
		}
		if (!direct_position.IsValid()) {
			direct_position = last_buffer ? last_buffer->GlobalEnd() : global_csv_pos;
		}
	}
	D_ASSERT(offset + read == direct_position.GetIndex());
	idx_t position = direct_position.GetIndex();
	while (read < count) {
		const idx_t got = file_handle->Read(data + read, count - read);
		if (got == 0) {
			lock_guard<mutex> parallel_lock(main_mutex);
			done = true;
			known_end = position;
			break;
		}
		read += got;
		position += got;
	}
	direct_position = position;
	return read;
}

void CSVSequentialBufferManager::MarkDone(const CSVBuffer &last) {
	done = true;
	known_end = last.GlobalEnd();
}

optional_idx CSVSequentialBufferManager::KnownEnd() {
	lock_guard<mutex> parallel_lock(main_mutex);
	return known_end;
}

void CSVSequentialBufferManager::UnpinPrevious(const idx_t pos) {
	if (pos != 0 && (sniffing || file_handle->CanSeek() || per_file_single_threaded)) {
		// We don't need to unpin the buffers here if we are not sniffing since we
		// control it per-thread on the scan
		if (cached_buffers[pos - 1]) {
			cached_buffers[pos - 1]->Unpin();
		}
	}
}

CSVBufferResidency CSVSequentialBufferManager::GetBufferResidency(const idx_t pos,
                                                                  shared_ptr<CSVBufferHandle> &handle) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (pos < cached_buffers.size()) {
		D_ASSERT(cached_buffers[pos]);
		if (!cached_buffers[pos]->IsInMemory()) {
			return CSVBufferResidency::NEEDS_LOAD;
		}
		UnpinPrevious(pos);
		handle = cached_buffers[pos]->Pin(*file_handle, has_seeked);
		return CSVBufferResidency::IN_MEMORY;
	}
	if (!cached_buffers.empty() && cached_buffers.back() && cached_buffers.back()->last_buffer) {
		MarkDone(*cached_buffers.back());
	}
	if (done) {
		return CSVBufferResidency::END_OF_FILE;
	}
	return CSVBufferResidency::NEEDS_LOAD;
}

void CSVSequentialBufferManager::ResetBuffer(const idx_t buffer_idx) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (buffer_idx >= cached_buffers.size()) {
		// Nothing to reset
		return;
	}
	D_ASSERT(cached_buffers[buffer_idx]);
	if (buffer_idx == 0 && cached_buffers.size() > 1) {
		cached_buffers[buffer_idx].reset();
		idx_t cur_buffer = buffer_idx + 1;
		while (reset_when_possible.find(cur_buffer) != reset_when_possible.end()) {
			cached_buffers[cur_buffer].reset();
			reset_when_possible.erase(cur_buffer);
			cur_buffer++;
		}
		return;
	}
	// We only reset if previous one was also already reset
	if (buffer_idx == 0 || cached_buffers[buffer_idx - 1]) {
		reset_when_possible.insert(buffer_idx);
		return;
	}
	if (cached_buffers[buffer_idx]->last_buffer) {
		// We clear the whole shebang
		cached_buffers.clear();
		reset_when_possible.clear();
		return;
	}
	cached_buffers[buffer_idx].reset();
	idx_t cur_buffer = buffer_idx + 1;
	while (reset_when_possible.find(cur_buffer) != reset_when_possible.end()) {
		cached_buffers[cur_buffer].reset();
		reset_when_possible.erase(cur_buffer);
		cur_buffer++;
	}
}

bool CSVSequentialBufferManager::Done() const {
	return done;
}

void CSVSequentialBufferManager::ResetBufferManager() {
	if (!file_handle->IsPipe()) {
		// If this is not a pipe we reset the buffer manager and restart it when doing the actual scan
		cached_buffers.clear();
		reset_when_possible.clear();
		file_handle->Reset();
		last_buffer = nullptr;
		done = false;
		known_end = optional_idx();
		global_csv_pos = 0;
		Initialize();
	}
}

} // namespace duckdb
