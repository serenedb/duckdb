#include "duckdb/storage/external_file_cache/caching_file_system.hpp"

#include "duckdb/common/chrono.hpp"
#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache_util.hpp"

namespace duckdb {

namespace {

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

FileBufferHandleGroup OnePin(ExternalFileCachePin pin) {
	vector<ExternalFileCachePin> pins;
	pins.push_back(std::move(pin));
	return FileBufferHandleGroup(std::move(pins));
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

bool CachingFileSystem::ShouldCacheFile(DatabaseInstance &db, const string &path) {
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
    : context(context), file_system(caching_file_system_p.file_system), db(caching_file_system_p.db), path(path_p),
      flags(std::move(flags_p)), opener(opener_p), validate(ExternalFileCacheUtil::GetCacheValidationMode(
                                                       path_p, context.GetClientContext(), caching_file_system_p.db)),
      position(0) {
	// If caching is disabled, or if we must validate cache entries, we always have to open the file
	if (!CacheAllowed() || Validate() || !ExternalFileCache::Get(db).TryGetFile(path.path, info)) {
		auto handle = GetFileHandle();
		// Snapshot the metadata with a single Stats call, avoiding repeated metadata lookups (e.g., fstat)
		info.validation = GetValidationInfo(file_system, *handle);
		info.can_seek = handle->CanSeek();
		info.on_disk = handle->OnDiskFile();
	}
	{
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		cache_valid_until = info.validation.cache_valid_until;
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
	return CachingFileSystem::ShouldCacheFile(db, path.path) && ExternalFileCache::Get(db).IsEnabled();
}

shared_ptr<FileHandle> CachingFileHandle::GetFileHandle() {
	annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
	if (file_handle) {
		return file_handle;
	}

	// The cache reads blocks in parallel through one FileHandle. Request parallel access on it to avoid races in
	// implementations that require explicit opt-in for concurrent pread-style access (e.g., HTTPFS).
	auto internal_flags = flags | FileFlags::FILE_FLAGS_PARALLEL_ACCESS;
	internal_flags.SetCachingMode(CachingMode::NO_CACHING);
	file_handle = file_system.OpenFile(path, internal_flags, opener);
	return file_handle;
}

shared_ptr<ExternalFileCacheFile> CachingFileHandle::GetCachedFile() {
	if (!CacheAllowed()) {
		return nullptr;
	}
	shared_ptr<ExternalFileCacheFile> file;
	optional<timestamp_t> deadline;
	{
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		file = cached_file;
		deadline = cache_valid_until;
	}
	if (!file) {
		auto current = info;
		current.validation.cache_valid_until = deadline;
		auto &cache = ExternalFileCache::Get(db);
		if (current.validation.IsCacheReuseProhibited() ||
		    (Validate() && !deadline && !CacheValidation::HasValidationMetadata(current.validation))) {
			cache.ForgetFile(path.path);
			return nullptr;
		}
		shared_ptr<FileHandle> source;
		{
			annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
			source = file_handle;
		}
		file = cache.OpenFile(path.path, current, Validate(), std::move(source));
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		if (!cached_file) {
			cached_file = file;
		}
		file = cached_file;
	}
	const auto &cached = file->Info().validation;
	if (cached.IsExpired()) {
		return nullptr;
	}
	return file;
}

Allocator &CachingFileHandle::GetBufferAllocator() const {
	return BufferManager::GetBufferManager(db).GetBufferAllocator();
}

FileBufferHandleGroup CachingFileHandle::ReadUncached(idx_t nr_bytes, idx_t location) {
	auto buf = ExternalFileCache::AllocateUncached(db, nr_bytes);
	auto handle = GetFileHandle();
	const auto read_start = steady_clock::now();
	handle->Read(context, buf.GetDataMutable(), nr_bytes, location);
	if (auto valid_until = handle->file_system.GetCacheValidUntil(*handle)) {
		const annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		TightenCacheDeadline(cache_valid_until, valid_until);
	}
	RecordReadThroughput(duration<double>(steady_clock::now() - read_start).count(), nr_bytes);
	return OnePin(std::move(buf));
}

FileBufferHandleGroup CachingFileHandle::Read(const idx_t nr_bytes, const idx_t location) {
	if (nr_bytes == 0) {
		return FileBufferHandleGroup();
	}
	auto file = GetCachedFile();
	if (!file) {
		return ReadUncached(nr_bytes, location);
	}
	if (location >= file->GetSize()) {
		return FileBufferHandleGroup();
	}
	auto &cache = ExternalFileCache::Get(db);
	vector<ExternalFileCachePin> pins;
	if (!file->HasSource()) {
		pins = cache.TryPinRange(*file, location, nr_bytes);
		if (pins.empty()) {
			file->SetSource(GetFileHandle());
		}
	}
	if (pins.empty()) {
		pins = cache.PinRange(*file, location, nr_bytes);
	}
	if (file->ReuseForbidden()) {
		cache.ForbidReuse(*file);
		annotated_lock_guard<annotated_mutex> guard(file_handle_mutex);
		cached_file.reset();
		cache_valid_until = timestamp_t::ninfinity();
	}
	return FileBufferHandleGroup(std::move(pins));
}

FileBufferHandleGroup CachingFileHandle::Read(idx_t &nr_bytes) {
	// If we can't seek, we can't use the cache for these calls,
	// because we won't be able to seek over any parts we skipped by reading from the cache
	if (!CanSeek() || !GetCachedFile()) {
		auto buf = ExternalFileCache::AllocateUncached(db, nr_bytes);
		auto file_handle = GetFileHandle();
		// Do positional read if handle can seek, otherwise have to fallback to sequential read.
		if (file_handle->CanSeek()) {
			const auto file_size = file_handle->GetFileSize();
			nr_bytes = position >= file_size ? 0 : MinValue(nr_bytes, file_size - position);
			file_handle->Read(context, buf.GetDataMutable(), nr_bytes, position);
		} else {
			nr_bytes = NumericCast<idx_t>(file_handle->Read(context, buf.GetDataMutable(), nr_bytes));
		}
		buf.Narrow(0, nr_bytes);
		position += nr_bytes;
		return OnePin(std::move(buf));
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
	return info.validation.file_size;
}

timestamp_t CachingFileHandle::GetLastModifiedTime() {
	return info.validation.last_modified;
}

string CachingFileHandle::GetVersionTag() {
	return info.validation.version_tag;
}

bool CachingFileHandle::Validate() const {
	if (!CachingFileSystem::ShouldCacheFile(db, path.path)) {
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
	return info.can_seek;
}

bool CachingFileHandle::IsRemoteFile() const {
	return FileSystem::IsRemoteFile(path.path);
}

bool CachingFileHandle::OnDiskFile() {
	return info.on_disk;
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
