#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_executor.hpp"

namespace duckdb {

namespace {

constexpr idx_t LOAD_PIECE = 1 << 20;
constexpr idx_t LOAD_ALIGNMENT = 1 << 12;

class LoadPieceTask : public BaseExecutorTask {
public:
	LoadPieceTask(TaskExecutor &executor, CSVFileHandle &file_handle, data_ptr_t data, idx_t size, idx_t position)
	    : BaseExecutorTask(executor), file_handle(file_handle), data(data), size(size), position(position) {
	}

	void ExecuteTask() override {
		file_handle.ReadAt(data, size, position);
	}

private:
	CSVFileHandle &file_handle;
	const data_ptr_t data;
	const idx_t size;
	const idx_t position;
};

} // namespace

CSVBuffer::CSVBuffer(ClientContext &context, idx_t buffer_size_p, CSVFileHandle &file_handle,
                     const idx_t &global_csv_current_position)
    : context(context), requested_size(buffer_size_p), is_pipe(file_handle.IsPipe()) {
	global_csv_start = global_csv_current_position;
	Load(file_handle, nullptr);
}

CSVBuffer::CSVBuffer(CSVFileHandle &file_handle, ClientContext &context, idx_t buffer_size,
                     idx_t global_csv_current_position, idx_t buffer_idx_p, shared_ptr<CSVBuffer> recycled)
    : context(context), requested_size(buffer_size), global_csv_start(global_csv_current_position),
      is_pipe(file_handle.IsPipe()), buffer_idx(buffer_idx_p) {
	Load(file_handle, std::move(recycled));
}

CSVBuffer::CSVBuffer(ClientContext &context, idx_t requested_size_p, idx_t actual_size, idx_t global_csv_start_p,
                     idx_t buffer_idx_p, bool last_buffer_p)
    : last_buffer(last_buffer_p), context(context), actual_buffer_size(actual_size), requested_size(requested_size_p),
      global_csv_start(global_csv_start_p), is_pipe(false), random_access(true), buffer_idx(buffer_idx_p) {
}

void CSVBuffer::Load(CSVFileHandle &file_handle, shared_ptr<CSVBuffer> recycled) {
	if (recycled && recycled->requested_size >= requested_size && recycled->Reusable()) {
		handle = std::move(recycled->handle);
		block = std::move(recycled->block);
	} else {
		AllocateBuffer(requested_size);
	}
	auto buffer = char_ptr_cast(handle.GetDataMutable());
	actual_buffer_size = file_handle.Read(buffer, requested_size);
	while (actual_buffer_size < requested_size && !file_handle.FinishedReading()) {
		// We keep reading until this block is full
		actual_buffer_size += file_handle.Read(&buffer[actual_buffer_size], requested_size - actual_buffer_size);
	}
	last_buffer = file_handle.FinishedReading();
}

shared_ptr<CSVBuffer> CSVBuffer::Next(CSVFileHandle &file_handle, idx_t buffer_size, bool &has_seeked,
                                      shared_ptr<CSVBuffer> recycled) const {
	if (has_seeked) {
		// This means that at some point a reload was done, and we are currently on the incorrect position in our file
		// handle
		file_handle.Seek(global_csv_start + actual_buffer_size);
		has_seeked = false;
	}
	auto next_csv_buffer = make_shared_ptr<CSVBuffer>(
	    file_handle, context, buffer_size, global_csv_start + actual_buffer_size, buffer_idx + 1, std::move(recycled));
	if (next_csv_buffer->GetBufferSize() == 0) {
		// We are done reading
		return nullptr;
	}
	return next_csv_buffer;
}

void CSVBuffer::AllocateBuffer(idx_t buffer_size) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	bool can_destroy = !is_pipe;
	handle = buffer_manager.Allocate(MemoryTag::CSV_READER, MaxValue<idx_t>(buffer_manager.GetBlockSize(), buffer_size),
	                                 can_destroy);
	block = handle.GetBlockHandle();
}

idx_t CSVBuffer::GetBufferSize() const {
	return actual_buffer_size;
}

void CSVBuffer::Reload(CSVFileHandle &file_handle) {
	AllocateBuffer(actual_buffer_size);
	// If we can seek, we seek and return the correct pointers
	file_handle.Seek(global_csv_start);
	file_handle.Read(handle.GetDataMutable(), actual_buffer_size);
}

void CSVBuffer::LoadRandomAccess(CSVFileHandle &file_handle, bool parallel) {
	D_ASSERT(random_access);
	AllocateBuffer(actual_buffer_size);
	const auto data = handle.GetDataMutable();
	const idx_t pieces =
	    parallel ? MinValue<idx_t>(TaskScheduler::QueryThreads(context), actual_buffer_size / LOAD_PIECE) : 1;
	if (pieces <= 1) {
		if (actual_buffer_size > 0) {
			file_handle.ReadAt(data, actual_buffer_size, global_csv_start);
		}
		return;
	}
	const idx_t piece = AlignValue<idx_t, LOAD_ALIGNMENT>((actual_buffer_size + pieces - 1) / pieces);
	TaskExecutor executor(context);
	for (idx_t begin = 0; begin < actual_buffer_size; begin += piece) {
		executor.ScheduleTask(make_uniq<LoadPieceTask>(executor, file_handle, data + begin,
		                                               MinValue(piece, actual_buffer_size - begin),
		                                               global_csv_start + begin));
	}
	executor.WorkOnTasks();
}

shared_ptr<CSVBufferHandle> CSVBuffer::Pin(CSVFileHandle &file_handle, bool &has_seeked, bool parallel) {
	lock_guard<mutex> guard(load_lock);
	if (!IsInMemory()) {
		// We have to reload it from disk
		block = nullptr;
		if (random_access) {
			LoadRandomAccess(file_handle, parallel);
		} else {
			Reload(file_handle);
			has_seeked = true;
		}
	}
	return CreatePin();
}

bool CSVBuffer::TryPin(shared_ptr<CSVBufferHandle> &pinned) {
	unique_lock<mutex> guard(load_lock);
	if (!guard.owns_lock() || !IsInMemory()) {
		return false;
	}
	pinned = CreatePin();
	return true;
}

shared_ptr<CSVBufferHandle> CSVBuffer::CreatePin() {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto pinned = make_shared_ptr<CSVBufferHandle>(buffer_manager.Pin(block), actual_buffer_size, requested_size,
	                                               last_buffer, buffer_idx);
	if (random_access) {
		// Random-access buffers can always be re-read
		Unpin();
	}
	return pinned;
}

void CSVBuffer::Unpin() {
	if (handle.IsValid()) {
		handle.Destroy();
	}
}

bool CSVBuffer::IsCSVFileLastBuffer() const {
	return last_buffer;
}

} // namespace duckdb
