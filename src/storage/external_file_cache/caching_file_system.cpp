#include "duckdb/storage/external_file_cache/caching_file_system.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/enums/destroy_buffer_upon.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache_util.hpp"

namespace duckdb {

namespace {

// Allocate an uncached read buffer to make sure it's de-allocated immediately, and its metadata is not stored in the
// eviction queue.
BufferHandle AllocateUncachedReadBuffer(BufferManager &buffer_manager, idx_t size) {
	auto buffer = buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, size);
	buffer.GetBlockHandle()->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	return buffer;
}

// IO operation is performed to stat the file.
CacheValidationInfo GetValidationInfo(FileSystem &file_system, FileHandle &handle) {
	auto stats = file_system.Stats(handle);
	CacheValidationInfo result;
	result.file_size = NumericCast<idx_t>(stats.file_size);
	result.last_modified = stats.last_modification_time;
	result.cache_valid_until = stats.cache_valid_until;
	result.version_tag = std::move(stats.version_tag);
	return result;
}

void TightenCacheDeadline(optional<timestamp_t> &cached, const optional<timestamp_t> &current) {
	if (current && (!cached || *current < *cached)) {
		cached = current;
	}
}

} // namespace

//===----------------------------------------------------------------------===//
// CachingFileSystem
//===----------------------------------------------------------------------===//

CachingFileSystem::CachingFileSystem(FileSystem &file_system_p, DatabaseInstance &db_p)
    : file_system(file_system_p), db(db_p) {
}

CachingFileSystem::~CachingFileSystem() {
}

CachingFileSystem CachingFileSystem::Get(ClientContext &context) {
	return CachingFileSystem(FileSystem::GetFileSystem(context), *context.db);
}

unique_ptr<CachingFileHandle> CachingFileSystem::OpenFile(const OpenFileInfo &path, const FileOpenFlags &flags,
                                                          optional_ptr<FileOpener> opener) {
	return make_uniq<CachingFileHandle>(QueryContext(), *this, path, flags, opener);
}

unique_ptr<CachingFileHandle> CachingFileSystem::OpenFile(QueryContext context, const OpenFileInfo &path,
                                                          const FileOpenFlags &flags, optional_ptr<FileOpener> opener) {
	return make_uniq<CachingFileHandle>(context, *this, path, flags, opener);
}

bool CachingFileSystem::ShouldCacheFile(const string &path) const {
	if (FileSystem::IsRemoteFile(path)) {
		return true;
	}
	// Local files are not cached: the OS page cache already serves repeated reads
	return Settings::Get<CacheLocalFilesSetting>(db);
}

//===----------------------------------------------------------------------===//
// CachingFileHandle
//===----------------------------------------------------------------------===//

CachingFileHandle::CachingFileHandle(QueryContext context, CachingFileSystem &caching_file_system_p,
                                     const OpenFileInfo &path_p, FileOpenFlags flags_p,
                                     optional_ptr<FileOpener> opener_p)
    : context(context), caching_file_system(caching_file_system_p), path(path_p), flags(std::move(flags_p)),
      opener(opener_p), validate(ExternalFileCacheUtil::GetCacheValidationMode(path_p, context.GetClientContext(),
                                                                               caching_file_system_p.db)),
      position(0) {
	if (!CacheAllowed() || Validate()) {
		// If caching is disabled, or if we must validate cache entries, we always have to open the file
		GetFileHandle();
		return;
	}
	BlockCacheFileInfo info;
	if (BlockCache::Get(caching_file_system.db).TryGetFile(path.path, info)) {
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		cached_info = std::move(info);
	} else {
		GetFileHandle();
	}
	if (StripForceFullDownloadIfPresent()) {
		Read(GetFileSize(), 0);
	}
}

CachingFileHandle::~CachingFileHandle() {
}

bool CachingFileHandle::StripForceFullDownloadIfPresent() {
	auto &extended_info_p = path.extended_info;
	if (!extended_info_p) {
		return false;
	}

	auto &extended_info = *extended_info_p;
	bool force_full_download;
	if (!extended_info.TryGetOption("force_full_download", force_full_download)) {
		return false;
	}

	//! We do have 'force_full_download' - strip it
	auto new_extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	*new_extended_info = extended_info;
	new_extended_info->options.erase("force_full_download");
	if (!new_extended_info->options.count("file_size")) {
		new_extended_info->options["file_size"] = Value::UBIGINT(GetFileSize());
	}
	path.extended_info = new_extended_info;
	return force_full_download;
}

bool CachingFileHandle::CacheAllowed() const {
	return caching_file_system.ShouldCacheFile(path.path) && BlockCache::Get(caching_file_system.db).IsEnabled();
}

shared_ptr<FileHandle> CachingFileHandle::GetFileHandle() {
	annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	if (file_handle) {
		return file_handle;
	}

	// The block cache reads blocks in parallel through one FileHandle. Request parallel access on it to avoid races in
	// implementations that require explicit opt-in for concurrent pread-style access (e.g., HTTPFS).
	auto internal_flags = flags | FileFlags::FILE_FLAGS_PARALLEL_ACCESS;
	internal_flags.SetCachingMode(CachingMode::NO_CACHING);
	file_handle = caching_file_system.file_system.OpenFile(path, internal_flags, opener);
	// Snapshot the metadata with a single Stats call, avoiding repeated metadata lookups (e.g., fstat)
	validation_info = GetValidationInfo(caching_file_system.file_system, *file_handle);
	return file_handle;
}

BlockCacheFileInfo CachingFileHandle::GetInfo() {
	{
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (cached_info) {
			return *cached_info;
		}
	}
	auto handle = GetFileHandle();
	BlockCacheFileInfo result;
	result.can_seek = handle->CanSeek();
	result.on_disk = handle->OnDiskFile();
	annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	result.validation = validation_info;
	return result;
}

shared_ptr<BlockCacheFile> CachingFileHandle::GetCachedFile() {
	if (!CacheAllowed()) {
		return nullptr;
	}
	shared_ptr<BlockCacheFile> file;
	{
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		file = cached_file;
	}
	if (!file) {
		auto info = GetInfo();
		auto &cache = BlockCache::Get(caching_file_system.db);
		if (info.validation.IsCacheReuseProhibited() || (Validate() && !info.validation.cache_valid_until &&
		                                                 !CacheValidation::HasValidationMetadata(info.validation))) {
			cache.ForgetFile(path.path);
			return nullptr;
		}
		shared_ptr<FileHandle> source;
		{
			annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
			source = file_handle;
		}
		file = cache.OpenFile(path.path, info, Validate(), std::move(source));
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (!cached_file) {
			cached_file = file;
		}
		file = cached_file;
	}
	const auto &cached = file->Info().validation;
	if (cached.IsCacheReuseProhibited() || cached.IsExpired()) {
		return nullptr;
	}
	return file;
}

Allocator &CachingFileHandle::GetBufferAllocator() const {
	return BufferManager::GetBufferManager(caching_file_system.db).GetBufferAllocator();
}

FileBufferHandleGroup CachingFileHandle::ReadUncached(idx_t nr_bytes, idx_t location) {
	auto buf = AllocateUncachedReadBuffer(BufferManager::GetBufferManager(caching_file_system.db), nr_bytes);
	ReadAndRecord(context, buf.GetDataMutable(), nr_bytes, location);
	const auto data = buf.Ptr();
	vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
	mem_handles.emplace_back(std::move(buf), data, nr_bytes);
	return FileBufferHandleGroup(std::move(mem_handles));
}

FileBufferHandleGroup CachingFileHandle::Read(const idx_t nr_bytes, const idx_t location) {
	if (nr_bytes == 0) {
		return FileBufferHandleGroup();
	}
	auto file = GetCachedFile();
	if (!file) {
		return ReadUncached(nr_bytes, location);
	}
	const idx_t file_size = file->GetSize();
	if (location >= file_size) {
		return FileBufferHandleGroup();
	}
	auto &cache = BlockCache::Get(caching_file_system.db);
	const idx_t end = MinValue(location + nr_bytes, file_size);
	vector<BlockCachePin> pins;
	if (!file->HasSource()) {
		pins = cache.TryPinRange(*file, location, end - location);
		if (pins.empty()) {
			file->SetSource(GetFileHandle());
		}
	}
	if (pins.empty()) {
		pins = cache.PinRange(file, location, end - location);
	}
	vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
	mem_handles.reserve(pins.size());
	idx_t block_start = location / BlockCache::BLOCK_SIZE * BlockCache::BLOCK_SIZE;
	for (auto &pin : pins) {
		const idx_t from = MaxValue(location, block_start) - block_start;
		const idx_t to = MinValue(end, block_start + pin.Size()) - block_start;
		const auto data = pin.Ptr() + from;
		mem_handles.emplace_back(std::move(pin), data, to - from);
		block_start += BlockCache::BLOCK_SIZE;
	}
	if (IsCacheReuseProhibited()) {
		cache.ForbidReuse(*file);
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		cached_file.reset();
	}
	return FileBufferHandleGroup(std::move(mem_handles));
}

FileBufferHandleGroup CachingFileHandle::Read(idx_t &nr_bytes) {
	// If we can't seek, we can't use the cache for these calls,
	// because we won't be able to seek over any parts we skipped by reading from the cache
	if (!CanSeek() || !GetCachedFile()) {
		auto buf = AllocateUncachedReadBuffer(BufferManager::GetBufferManager(caching_file_system.db), nr_bytes);
		auto handle = GetFileHandle();
		// Do positional read if handle can seek, otherwise have to fallback to sequential read.
		if (handle->CanSeek()) {
			const auto file_size = handle->GetFileSize();
			nr_bytes = position >= file_size ? 0 : MinValue(nr_bytes, file_size - position);
			handle->Read(context, buf.GetDataMutable(), nr_bytes, position);
		} else {
			nr_bytes = NumericCast<idx_t>(handle->Read(context, buf.GetDataMutable(), nr_bytes));
		}
		const auto data = buf.Ptr();
		vector<FileBufferHandleGroup::MemoryHandle> mem_handles;
		mem_handles.emplace_back(std::move(buf), data, nr_bytes);
		position += nr_bytes;
		return FileBufferHandleGroup(std::move(mem_handles));
	}

	const idx_t file_size = GetFileSize();
	if (position >= file_size) {
		nr_bytes = 0;
		return {};
	}

	nr_bytes = MinValue(nr_bytes, file_size - position);
	auto group = Read(nr_bytes, position);
	position += nr_bytes;
	return group;
}

string CachingFileHandle::GetPath() const {
	return path.path;
}

idx_t CachingFileHandle::GetFileSize() {
	return GetInfo().validation.file_size;
}

timestamp_t CachingFileHandle::GetLastModifiedTime() {
	return GetInfo().validation.last_modified;
}

string CachingFileHandle::GetVersionTag() {
	return GetInfo().validation.version_tag;
}

bool CachingFileHandle::Validate() const {
	if (!caching_file_system.ShouldCacheFile(path.path)) {
		// uncached files have no stale state to serve, so their metadata must always be read fresh
		return true;
	}
	switch (validate) {
	case CacheValidationMode::VALIDATE_ALL:
		return true;
	case CacheValidationMode::VALIDATE_REMOTE:
		return FileSystem::IsRemoteFile(path.path);
	case CacheValidationMode::NO_VALIDATION:
		return false;
	default:
		return true;
	}
}

bool CachingFileHandle::CanSeek() {
	return GetInfo().can_seek;
}

bool CachingFileHandle::IsRemoteFile() const {
	return FileSystem::IsRemoteFile(path.path);
}

bool CachingFileHandle::OnDiskFile() {
	return GetInfo().on_disk;
}

void ReadThroughputEstimator::AddSample(double seconds, idx_t bytes) {
	const double b = static_cast<double>(bytes);
	lock_guard<mutex> guard(lock);
	sample_count++;
	sum_bytes += b;
	sum_seconds += seconds;
	sum_bytes_sq += b * b;
	sum_bytes_seconds += b * seconds;
}

bool ReadThroughputEstimator::TryEstimate(NetworkThroughputEstimate &result) const {
	lock_guard<mutex> guard(lock);
	if (sample_count < 2) {
		return false;
	}
	const double n = static_cast<double>(sample_count);
	const double variance = sum_bytes_sq - sum_bytes * sum_bytes / n;
	if (!(variance > 0)) {
		// every read was the same size, so we cant really get the latency
		return false;
	}
	// least squares estimate of the line "total_time = intercept + slope * bytes".
	const double slope = (sum_bytes_seconds - sum_bytes * sum_seconds / n) / variance;
	const double intercept = (sum_seconds - slope * sum_bytes) / n;
	if (!(slope > 0) || !(intercept > 0)) {
		return false;
	}
	// intercept is the fixed start-up cost of a read, slope is seconds per byte so bandwidth is 1 / slope
	result.latency_seconds = intercept;
	result.bandwidth_bytes_per_s = 1.0 / slope;
	return true;
}

bool CachingFileHandle::TryGetNetworkThroughput(NetworkThroughputEstimate &result) {
	// Remote files measure throughput in their own file system; local files fit it from this handle's own reads.
	if (GetFileHandle()->TryGetNetworkThroughput(result)) {
		return true;
	}
	return throughput_estimator.TryEstimate(result);
}

void CachingFileHandle::ReadAndRecord(QueryContext context, data_ptr_t buffer, idx_t nr_bytes, idx_t location) {
	auto handle = GetFileHandle();
	const auto read_start = steady_clock::now();
	handle->Read(context, buffer, nr_bytes, location);
	auto cache_valid_until = handle->file_system.GetCacheValidUntil(*handle);
	if (cache_valid_until) {
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (file_handle == handle) {
			TightenCacheDeadline(validation_info.cache_valid_until, cache_valid_until);
		}
	}
	RecordReadThroughput(duration<double>(steady_clock::now() - read_start).count(), nr_bytes);
}

bool CachingFileHandle::IsCacheReuseProhibited() {
	shared_ptr<FileHandle> handle;
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (!file_handle) {
			return cached_info && cached_info->validation.IsCacheReuseProhibited();
		}
		handle = file_handle;
	}
	auto cache_valid_until = handle->file_system.GetCacheValidUntil(*handle);
	const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	TightenCacheDeadline(validation_info.cache_valid_until, cache_valid_until);
	return validation_info.IsCacheReuseProhibited();
}

void CachingFileHandle::RecordReadThroughput(double total_seconds, idx_t bytes) {
	if (IsRemoteFile() || !(total_seconds > 0)) {
		return;
	}
	throughput_estimator.AddSample(total_seconds, bytes);
}

idx_t CachingFileHandle::SeekPosition() {
	return position;
}

void CachingFileHandle::Seek(idx_t location) {
	position = location;
	shared_ptr<FileHandle> handle;
	{
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		handle = file_handle;
	}
	if (handle) {
		handle->Seek(location);
	}
}

} // namespace duckdb
