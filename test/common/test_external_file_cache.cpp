#include "catch.hpp"
#include "caching_test_utils.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/memory_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/storage/external_file_cache/memory_reader.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/external_file_cache/cache_validation.hpp"
#include "duckdb/storage/external_file_cache/input_file_reader.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"
#include "duckdb/storage/external_file_cache/read_ahead_window.hpp"

#include <absl/cleanup/cleanup.h>
#include <absl/crc/crc32c.h>
#include <absl/time/clock.h>
#include <chrono>
#include <future>

namespace duckdb {

namespace {

using EFCTestFileGuard = CachingTestFileGuard;
using EFCTrackingFileSystem = SimpleTrackingFileSystem;
using EFCNoMetadataFileSystem = NoValidationMetadataFileSystem;

class CachePolicyFileSystem : public SimpleTrackingFileSystem {
public:
	FileMetadata Stats(FileHandle &handle) override {
		stats_count++;
		auto metadata = SimpleTrackingFileSystem::Stats(handle);
		metadata.version_tag = version_tag;
		metadata.cache_valid_until = cache_valid_until;
		return metadata;
	}

	optional<timestamp_t> GetCacheValidUntil(FileHandle &handle) override {
		return cache_valid_until;
	}

	string version_tag = "v1";
	timestamp_t cache_valid_until = timestamp_t::infinity();
	idx_t stats_count = 0;
};

//! File system whose positional reads can be held back, to control the timing of in-flight block fetches.
class BlockingCachePolicyFileSystem : public CachePolicyFileSystem {
public:
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			read_count++;
			auto released = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
				return !block_reads;
			};
			lock.Await(absl::Condition(&released));
		}
		CachePolicyFileSystem::Read(handle, buffer, nr_bytes, location);
	}
	void ReadScattered(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location) override {
		FileSystem::ReadScattered(handle, targets, location);
	}

	void BlockReads() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		block_reads = true;
	}

	void WaitForReadCount(idx_t count) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto reached = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
			return read_count >= count;
		};
		lock.Await(absl::Condition(&reached));
	}

	void ReleaseReads() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		block_reads = false;
	}

	idx_t GetReadCount() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return read_count;
	}

private:
	annotated_mutex lock;
	bool block_reads DUCKDB_GUARDED_BY(lock) = false;
	idx_t read_count DUCKDB_GUARDED_BY(lock) = 0;
};

//! Records the ranges of positional reads.
class ReadRecordingFileSystem : public SimpleTrackingFileSystem {
public:
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			reads.emplace_back(location, NumericCast<idx_t>(nr_bytes));
		}
		SimpleTrackingFileSystem::Read(handle, buffer, nr_bytes, location);
	}
	void ReadScattered(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location) override {
		FileSystem::ReadScattered(handle, targets, location);
	}

	//! Recorded reads sorted by location, clears them
	vector<pair<idx_t, idx_t>> TakeReads() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto result = std::move(reads);
		reads.clear();
		std::sort(result.begin(), result.end());
		return result;
	}

private:
	annotated_mutex lock;
	vector<pair<idx_t, idx_t>> reads DUCKDB_GUARDED_BY(lock);
};

constexpr int64_t SHORT_MAX_AGE_MICROS = 100 * 1000LL;

OpenFileInfo MakeTestOpenFileInfo(const string &path) {
	OpenFileInfo info(path);
	info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	info.extended_info->options["validate_external_file_cache"] = Value::BOOLEAN(false);
	return info;
}

OpenFileInfo MakeValidatingOpenFileInfo(const string &path) {
	OpenFileInfo info(path);
	info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	info.extended_info->options["validate_external_file_cache"] = Value::BOOLEAN(true);
	return info;
}

string MakeTestContent(idx_t size) {
	string content(size, '\0');
	for (idx_t i = 0; i < size; i++) {
		content[i] = static_cast<char>('A' + (i % 26));
	}
	return content;
}

string ReadFull(CachingFileHandle &handle, idx_t size, idx_t offset = 0) {
	auto group = handle.Read(size, offset);
	string result(size, '\0');
	group.CopyTo(reinterpret_cast<data_ptr_t>(&result[0]), size);
	return result;
}

string ReadSequential(CachingFileHandle &handle, idx_t size) {
	auto read_size = size;
	auto group = handle.Read(read_size);
	string result(read_size, '\0');
	group.CopyTo(reinterpret_cast<data_ptr_t>(&result[0]), read_size);
	return result;
}

void WriteTestContent(const string &path, const string &content) {
	auto local_fs = FileSystem::CreateLocal();
	auto handle = local_fs->OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
	handle->Write(QueryContext(), const_cast<char *>(content.data()), content.size(), 0);
	handle->Sync();
}

idx_t CountCachedBlocks(ExternalFileCache &cache) {
	return cache.GetCachedFileInformation().size();
}

idx_t TotalCachedBytes(ExternalFileCache &cache) {
	idx_t total = 0;
	for (auto &info : cache.GetCachedFileInformation()) {
		total += info.nr_bytes;
	}
	return total;
}

void WaitPast(timestamp_t deadline) {
	while (Timestamp::GetCurrentTimestamp() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

} // namespace

TEST_CASE("A file that fits in one request is fetched whole on the first read", "[external_file_cache]") {
	auto recording_fs = make_uniq<ReadRecordingFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const idx_t FILE_SIZE = 3 * BLOCK_SIZE + 50;
	auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_small_file.bin", content);

	CachingFileSystem cfs(*recording_fs, db_instance);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	auto &cache = db_instance.GetExternalFileCache();

	REQUIRE(ReadFull(*handle, 100, 2 * BLOCK_SIZE + 800) == content.substr(2 * BLOCK_SIZE + 800, 100));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{0, FILE_SIZE}});
	REQUIRE(CountCachedBlocks(cache) == 4);
	REQUIRE(TotalCachedBytes(cache) == FILE_SIZE);

	// Any later read of the file is served from the cache
	REQUIRE(ReadFull(*handle, 500, 100) == content.substr(100, 500));
	REQUIRE(ReadFull(*handle, 10, FILE_SIZE - 10) == content.substr(FILE_SIZE - 10));
	REQUIRE(recording_fs->TakeReads().empty());
}

TEST_CASE("Reads of a file that takes several requests fetch the cache blocks around them", "[external_file_cache]") {
	auto recording_fs = make_uniq<ReadRecordingFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetExternalFileCache();
	Connection con(db);

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_local_max_block_size = " + to_string(BLOCK_SIZE)));
	const idx_t FILE_SIZE = 3 * BLOCK_SIZE + 50;
	auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_cache_sized.bin", content);

	CachingFileSystem cfs(*recording_fs, db_instance);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);

	REQUIRE(ReadFull(*handle, 100, 0) == content.substr(0, 100));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{0, BLOCK_SIZE}});

	for (idx_t offset = 100; offset + 1000 <= BLOCK_SIZE; offset += 1000) {
		REQUIRE(ReadFull(*handle, 1000, offset) == content.substr(offset, 1000));
	}
	REQUIRE(recording_fs->TakeReads().empty());

	REQUIRE(ReadFull(*handle, 100, BLOCK_SIZE) == content.substr(BLOCK_SIZE, 100));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{BLOCK_SIZE, BLOCK_SIZE}});

	REQUIRE(ReadFull(*handle, 100, 2 * BLOCK_SIZE + 100) == content.substr(2 * BLOCK_SIZE + 100, 100));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{2 * BLOCK_SIZE, BLOCK_SIZE}});

	REQUIRE(ReadFull(*handle, 10, FILE_SIZE - 10) == content.substr(FILE_SIZE - 10, 10));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{3 * BLOCK_SIZE, 50}});
	REQUIRE(CountCachedBlocks(cache) == 4);
}

TEST_CASE("Reads past the end of the file fetch only the bytes in it", "[external_file_cache]") {
	auto recording_fs = make_uniq<ReadRecordingFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetExternalFileCache();
	Connection con(db);

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_local_max_block_size = " + to_string(BLOCK_SIZE)));
	const idx_t FILE_SIZE = 2 * BLOCK_SIZE + 50;
	auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_past_end.bin", content);

	CachingFileSystem cfs(*recording_fs, db_instance);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);

	auto group = handle->Read(100, FILE_SIZE - 10);
	REQUIRE(group.GetHandles().size() == 1);
	REQUIRE(group.GetHandles()[0].Size() == 10);
	string tail(10, '\0');
	group.CopyTo(reinterpret_cast<data_ptr_t>(&tail[0]), 10);
	REQUIRE(tail == content.substr(FILE_SIZE - 10));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{2 * BLOCK_SIZE, 50}});

	REQUIRE(handle->Read(10, FILE_SIZE).GetHandles().empty());
	REQUIRE(recording_fs->TakeReads().empty());
	REQUIRE(CountCachedBlocks(cache) == 1);
}

TEST_CASE("Disabled external file cache does not cache blocks", "[external_file_cache]") {
	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetExternalFileCache();
	Connection con(db);

	const idx_t FILE_SIZE = 16384;
	const auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_disabled.bin", content);

	CachingFileSystem cfs(*tracking_fs, db_instance);

	// Disable the cache.
	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=false"));
	REQUIRE(cache.GetCachedFileInformation().empty());

	// Open and fully read the file with caching disabled.
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, FILE_SIZE) == content);
	}

	// With caching disabled, no block should be cached.
	REQUIRE(cache.GetCachedFileInformation().empty());

	// When cache enabled, opening and reading the file does cache its block.
	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=true"));
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, FILE_SIZE) == content);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);
}

TEST_CASE("Sequential read preserves its position when the cache is disabled", "[external_file_cache]") {
	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetExternalFileCache();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string first_block(block_size, 'A');
	const string second_block(block_size, 'B');
	EFCTestFileGuard test_file("test_efc_disabled_sequential_position.bin", first_block + second_block);

	CachingFileSystem cfs(*tracking_fs, *db.instance);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadSequential(*handle, block_size) == first_block);
	REQUIRE(CountCachedBlocks(cache) == 2);

	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=false"));
	REQUIRE(ReadSequential(*handle, block_size) == second_block);
}

TEST_CASE("Concurrent SET and Read do not corrupt data or cache state", "[external_file_cache]") {
	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;

	constexpr idx_t FILE_SIZE = 2 * ExternalFileCache::DEFAULT_BLOCK_SIZE + 137; // odd tail to stress boundaries
	const auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_set_vs_read.bin", content);

	CachingFileSystem cfs(*tracking_fs, db_instance);

	constexpr idx_t READER_COUNT = 4;
	constexpr idx_t SETTER_COUNT = 2;

	atomic<bool> stop {false};
	atomic<idx_t> mismatches {0};

	struct ReadFixture {
		idx_t off;
		idx_t len;
	};
	const array<ReadFixture, 5> FIXTURES = {{
	    {0, 4096},
	    {123, 8000},
	    {ExternalFileCache::DEFAULT_BLOCK_SIZE - 8500, 17000},
	    {FILE_SIZE - 1024, 1024},
	    {0, FILE_SIZE},
	}};

	vector<std::thread> threads;
	threads.reserve(READER_COUNT + SETTER_COUNT);

	for (idx_t r = 0; r < READER_COUNT; r++) {
		threads.emplace_back([&, r]() {
			auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
			idx_t i = r;
			while (!stop.load()) {
				const auto &fx = FIXTURES[i % FIXTURES.size()];
				if (ReadFull(*handle, fx.len, fx.off) != content.substr(fx.off, fx.len)) {
					mismatches.fetch_add(1);
				}
				i++;
			}
		});
	}

	for (idx_t s = 0; s < SETTER_COUNT; s++) {
		threads.emplace_back([&, s]() {
			Connection con(db);
			idx_t i = s;
			while (!stop.load()) {
				con.Query(StringUtil::Format("SET enable_external_file_cache=%s", i % 2 == 0 ? "false" : "true"));
				i++;
			}
		});
	}

	// Run the set and read for a while.
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	stop.store(true);
	for (auto &t : threads) {
		t.join();
	}
	REQUIRE(mismatches.load() == 0);

	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=true"));
	auto &cache = db_instance.GetExternalFileCache();
	idx_t total_cached_bytes = 0;
	for (auto &info : cache.GetCachedFileInformation()) {
		REQUIRE(info.location + info.nr_bytes <= FILE_SIZE);
		total_cached_bytes += info.nr_bytes;
	}

	REQUIRE(total_cached_bytes <= FILE_SIZE);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadFull(*handle, FILE_SIZE) == content);
}

TEST_CASE("Disabling external file cache drops its blocks", "[external_file_cache]") {
	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	CachingFileSystem cfs(*tracking_fs, db_instance);
	auto &cache = db_instance.GetExternalFileCache();
	Connection con(db);

	const auto content = MakeTestContent(ExternalFileCache::DEFAULT_BLOCK_SIZE);
	EFCTestFileGuard test_file("test_efc_disable_drops_blocks.bin", content);

	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, content.size()) == content);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);

	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=false"));
	REQUIRE(CountCachedBlocks(cache) == 0);

	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=true"));
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadFull(*handle, content.size()) == content);
	REQUIRE(CountCachedBlocks(cache) == 1);
}

TEST_CASE("File with freshness deadline but no validators is cached and reused", "[external_file_cache]") {
	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetExternalFileCache();

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(BLOCK_SIZE, 'A');
	const string content_b(BLOCK_SIZE, 'B'); // same size as content_a
	EFCTestFileGuard test_file("test_efc_freshness_reuse.bin", content_a);

	CachingFileSystem cfs(*fresh_fs, db_instance);

	// First read populates the cache.
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_a);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);
	ExternalFileCacheFileInfo info;
	REQUIRE(cache.TryGetFile(test_file.GetPath(), info));
	const auto original_valid_until = info.validation.cache_valid_until;

	// Overwrite with same-size content. Within the freshness deadline the cached content is still served,
	// because the file provides no validators to detect the change.
	WriteTestContent(test_file.GetPath(), content_b);
	fresh_fs->max_age_micros *= 2;
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_a);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);
	REQUIRE(cache.TryGetFile(test_file.GetPath(), info));
	REQUIRE(info.validation.cache_valid_until == original_valid_until);
}

TEST_CASE("NO_VALIDATION retains and enforces the initial freshness deadline", "[external_file_cache]") {
	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetExternalFileCache();

	const auto max_age_micros = fresh_fs->max_age_micros;
	fresh_fs->max_age_micros = SHORT_MAX_AGE_MICROS;

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(BLOCK_SIZE, 'A');
	const string content_b(BLOCK_SIZE, 'B');
	const string content_c(BLOCK_SIZE, 'C');
	EFCTestFileGuard test_file("test_efc_no_validation_freshness.bin", content_a);

	CachingFileSystem cfs(*fresh_fs, db_instance);
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_a);
	}

	const auto initial_deadline = timestamp_t(Timestamp::GetCurrentTimestamp().value + fresh_fs->max_age_micros);
	WaitPast(initial_deadline);
	ExternalFileCacheFileInfo info;
	REQUIRE_FALSE(cache.TryGetFile(test_file.GetPath(), info));

	fresh_fs->max_age_micros = max_age_micros;
	WriteTestContent(test_file.GetPath(), content_b);
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_b);
	}

	// Validate cache metadata.
	REQUIRE(cache.TryGetFile(test_file.GetPath(), info));
	REQUIRE(info.validation.cache_valid_until != nullopt);
	REQUIRE(*info.validation.cache_valid_until > initial_deadline);

	// The refreshed metadata allows the current file contents to be cached again.
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_b);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);

	// NO_VALIDATION reuses the refreshed cache while its new deadline is fresh.
	WriteTestContent(test_file.GetPath(), content_c);
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_b);
	}
}

TEST_CASE("File with freshness deadline is invalidated when the file size changes", "[external_file_cache]") {
	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetExternalFileCache();

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(BLOCK_SIZE, 'A');
	const string content_b(BLOCK_SIZE * 2, 'B');
	EFCTestFileGuard test_file("test_efc_freshness_size_change.bin", content_a);

	CachingFileSystem cfs(*fresh_fs, db_instance);

	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_a);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);

	// Overwrite with larger content: the size mismatch invalidates the cache despite the freshness deadline.
	WriteTestContent(test_file.GetPath(), content_b);
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(handle->GetFileSize() == content_b.size());
		REQUIRE(ReadFull(*handle, content_b.size()) == content_b);
	}
	REQUIRE(TotalCachedBytes(cache) == content_b.size());
}

TEST_CASE("Long-lived handle does not use cache after the freshness deadline", "[external_file_cache]") {
	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;

	fresh_fs->max_age_micros = SHORT_MAX_AGE_MICROS;

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(BLOCK_SIZE, 'A');
	const string content_b(BLOCK_SIZE, 'B');
	EFCTestFileGuard test_file("test_efc_freshness_long_lived_handle.bin", content_a);

	CachingFileSystem cfs(*fresh_fs, db_instance);
	auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_a);

	WaitPast(timestamp_t(Timestamp::GetCurrentTimestamp().value + fresh_fs->max_age_micros));
	WriteTestContent(test_file.GetPath(), content_b);
	REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_b);
}

TEST_CASE("Long-lived handle with validators stops using cache after the freshness deadline", "[external_file_cache]") {
	auto validating_fs = make_uniq<CachePolicyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(block_size, 'A');
	const string content_b(block_size, 'B');
	EFCTestFileGuard test_file("test_efc_validator_freshness_long_lived_handle.bin", content_a);

	CachingFileSystem cfs(*validating_fs, *db.instance);
	validating_fs->cache_valid_until = timestamp_t(Timestamp::GetCurrentTimestamp().value + SHORT_MAX_AGE_MICROS);
	auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadFull(*handle, block_size) == content_a);
	REQUIRE(validating_fs->stats_count == 1);

	WaitPast(validating_fs->cache_valid_until);
	WriteTestContent(test_file.GetPath(), content_b);

	REQUIRE(ReadFull(*handle, block_size) == content_b);
	REQUIRE(validating_fs->stats_count == 1);
}

TEST_CASE("An earlier freshness deadline in the response to a fetch ends the reuse of the cached file sooner",
          "[external_file_cache]") {
	auto policy_fs = make_uniq<CachePolicyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetExternalFileCache();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const idx_t size = 40 * block_size;
	const string content_a(size, 'A');
	const string content_b(size, 'B');
	EFCTestFileGuard test_file("test_efc_earlier_deadline.bin", content_a);
	CachingFileSystem cfs(*policy_fs, *db.instance);

	const auto late = timestamp_t(Timestamp::GetCurrentTimestamp().value + 3600LL * 1000 * 1000);
	policy_fs->cache_valid_until = late;
	auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadFull(*handle, block_size) == content_a.substr(0, block_size));
	ExternalFileCacheFileInfo info;
	REQUIRE(cache.TryGetFile(test_file.GetPath(), info));
	REQUIRE(info.validation.cache_valid_until == late);

	const auto early = timestamp_t(Timestamp::GetCurrentTimestamp().value + SHORT_MAX_AGE_MICROS);
	policy_fs->cache_valid_until = early;
	REQUIRE(ReadFull(*handle, block_size, size - block_size) == content_a.substr(size - block_size));
	REQUIRE(cache.TryGetFile(test_file.GetPath(), info));
	REQUIRE(info.validation.cache_valid_until == early);

	policy_fs->cache_valid_until = late;
	REQUIRE(ReadFull(*handle, block_size, block_size) == content_a.substr(block_size, block_size));
	REQUIRE(cache.TryGetFile(test_file.GetPath(), info));
	REQUIRE(info.validation.cache_valid_until == early);

	WaitPast(early);
	WriteTestContent(test_file.GetPath(), content_b);
	REQUIRE(ReadFull(*handle, block_size) == content_b.substr(0, block_size));
	REQUIRE_FALSE(cache.TryGetFile(test_file.GetPath(), info));
}

TEST_CASE("File marked as not cacheable does not retain cached blocks", "[external_file_cache]") {
	auto policy_fs = make_uniq<CachePolicyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetExternalFileCache();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(block_size, 'A');
	const string content_b(block_size, 'B');
	EFCTestFileGuard test_file("test_efc_no_store.bin", content_a);
	CachingFileSystem cfs(*policy_fs, *db.instance);

	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, block_size) == content_a);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);

	WriteTestContent(test_file.GetPath(), content_b);
	policy_fs->cache_valid_until = timestamp_t::ninfinity();
	policy_fs->version_tag = "v2";
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, block_size) == content_b);
	}
	REQUIRE(CountCachedBlocks(cache) == 0);
}

TEST_CASE("Explicit cache reuse prohibition is honored under NO_VALIDATION", "[external_file_cache]") {
	auto policy_fs = make_uniq<CachePolicyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetExternalFileCache();
	policy_fs->cache_valid_until = timestamp_t::ninfinity();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(block_size, 'A');
	const string content_b(block_size, 'B');
	EFCTestFileGuard test_file("test_efc_no_reuse_no_validation.bin", content_a);
	CachingFileSystem cfs(*policy_fs, *db.instance);

	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, block_size) == content_a);
	}
	REQUIRE(CountCachedBlocks(cache) == 0);

	WriteTestContent(test_file.GetPath(), content_b);
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, block_size) == content_b);
	}
	REQUIRE(CountCachedBlocks(cache) == 0);
}

TEST_CASE("Expired freshness deadline is not served from cache", "[external_file_cache]") {
	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;

	fresh_fs->max_age_micros = -1000000; // deadline is always in the past

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(BLOCK_SIZE, 'A');
	const string content_b(BLOCK_SIZE, 'B'); // same size as content_a
	EFCTestFileGuard test_file("test_efc_freshness_expired.bin", content_a);

	CachingFileSystem cfs(*fresh_fs, db_instance);

	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_a);
	}

	// Overwrite with same-size content: the expired deadline prevents serving stale cached data.
	WriteTestContent(test_file.GetPath(), content_b);
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_b);
	}
}

TEST_CASE("Waiter on a loading block refetches when the response prohibits sharing", "[external_file_cache]") {
	auto policy_fs = make_uniq<BlockingCachePolicyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetExternalFileCache();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content(block_size, 'A');
	EFCTestFileGuard test_file("test_efc_loading_waiter.bin", content);
	CachingFileSystem cfs(*policy_fs, *db.instance);

	// Open both handles while the policy still allows sharing.
	auto handle_a = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	auto handle_b = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);

	// Content responses prohibit reuse, and hang until released.
	policy_fs->cache_valid_until = timestamp_t::ninfinity();
	policy_fs->BlockReads();

	string result_a;
	string result_b;
	std::thread reader_a([&]() { result_a = ReadFull(*handle_a, block_size); });
	// Once reader A's fetch is in flight, the block is LOADING, so reader B waits on it.
	policy_fs->WaitForReadCount(1);

	annotated_mutex reader_b_lock;
	bool reader_b_is_started = false;
	std::thread reader_b([&]() {
		{
			annotated_lock_guard<annotated_mutex> guard(reader_b_lock);
			reader_b_is_started = true;
		}
		result_b = ReadFull(*handle_b, block_size);
	});
	{
		annotated_lock_guard<annotated_mutex> guard(reader_b_lock);
		reader_b_lock.Await(absl::Condition(&reader_b_is_started));
	}
	policy_fs->ReleaseReads();

	reader_a.join();
	reader_b.join();
	REQUIRE(result_a == content);
	REQUIRE(result_b == content);
	// Reader B must not consume reader A's response: each reader issues its own request.
	REQUIRE(policy_fs->GetReadCount() == 2);
	REQUIRE(CountCachedBlocks(cache) == 0);
}

TEST_CASE("Content response can prohibit cache reuse", "[external_file_cache]") {
	auto policy_fs = make_uniq<CachePolicyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetExternalFileCache();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(block_size, 'A');
	const string content_b(block_size, 'B');
	EFCTestFileGuard test_file("test_efc_content_policy.bin", content_a);
	CachingFileSystem cfs(*policy_fs, *db.instance);

	auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	// Cache validation expiration is found after a successful open.
	policy_fs->cache_valid_until = timestamp_t::ninfinity();
	REQUIRE(ReadFull(*handle, block_size) == content_a);
	REQUIRE(CountCachedBlocks(cache) == 0);

	WriteTestContent(test_file.GetPath(), content_b);
	REQUIRE(ReadFull(*handle, block_size) == content_b);
	REQUIRE(CountCachedBlocks(cache) == 0);
}

TEST_CASE("No-metadata file is not cached and always returns fresh content", "[external_file_cache]") {
	auto no_meta_fs = make_uniq<EFCNoMetadataFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetExternalFileCache();

	const idx_t BLOCK_SIZE = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(BLOCK_SIZE, 'A');
	const string content_b(BLOCK_SIZE * 2, 'B');
	EFCTestFileGuard test_file("test_efc_no_metadata.bin", content_a);

	CachingFileSystem cfs(*no_meta_fs, db_instance);

	// First read: data is fetched from source.  No blocks should be stored in the cache.
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(handle->GetFileSize() == content_a.size());
		REQUIRE(ReadFull(*handle, BLOCK_SIZE) == content_a);
	}
	REQUIRE(CountCachedBlocks(cache) == 0);

	// Overwrite the file with larger content.
	WriteTestContent(test_file.GetPath(), content_b);

	// Second read: file size and content must reflect the new version, not the cached one.
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(handle->GetFileSize() == content_b.size());
		REQUIRE(ReadFull(*handle, content_b.size()) == content_b);
	}
	REQUIRE(CountCachedBlocks(cache) == 0);
}

TEST_CASE("Dropping cache blocks preserves existing readers", "[external_file_cache]") {
	auto policy_fs = make_uniq<CachePolicyFileSystem>();
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetExternalFileCache();

	const idx_t block_size = ExternalFileCache::DEFAULT_BLOCK_SIZE;
	const string content_a(block_size, 'A');
	const string content_b(block_size, 'B');
	EFCTestFileGuard test_file("test_efc_drop_blocks.bin", content_a);
	CachingFileSystem cfs(*policy_fs, *db.instance);

	auto reader = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	auto group = reader->Read(block_size, 0);

	WriteTestContent(test_file.GetPath(), content_b);
	policy_fs->version_tag = "v2";

	// Future readers receive fresh blocks
	{
		auto handle = cfs.OpenFile(MakeValidatingOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, block_size) == content_b);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);

	// Existing readers retain the blocks they hold
	string held(block_size, '\0');
	group.CopyTo(reinterpret_cast<data_ptr_t>(&held[0]), block_size);
	REQUIRE(held == content_a);
}

namespace {

constexpr idx_t BLOCK = ExternalFileCache::DEFAULT_BLOCK_SIZE;
constexpr idx_t PIECE_SIZE = 4ULL * 1024ULL * 1024ULL;

constexpr idx_t BlockCountOf(idx_t size) {
	return (size + BLOCK - 1) / BLOCK;
}

constexpr idx_t PIECE_BLOCKS = BlockCountOf(PIECE_SIZE);
constexpr idx_t REQUEST_BLOCKS = BlockCountOf(ExternalFileCache::MAX_REQUEST);

uint8_t ExpectedByte(idx_t offset) {
	return static_cast<uint8_t>((offset * 31 + offset / BLOCK * 7) & 0xFF);
}

class CountingSourceFileSystem : public LocalFileSystem {
public:
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		absl::Duration delay;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			reads.emplace_back(location, NumericCast<idx_t>(nr_bytes));
			auto released = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
				return !hold || released_reads.contains(location);
			};
			lock.Await(absl::Condition(&released));
			if (failures > 0) {
				failures--;
				throw IOException("injected read failure");
			}
			delay = latency + absl::Seconds(bandwidth > 0 ? double(nr_bytes) / bandwidth : 0);
		}
		absl::SleepFor(delay);
		LocalFileSystem::Read(handle, buffer, nr_bytes, location);
	}
	void ReadScattered(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location) override {
		FileSystem::ReadScattered(handle, targets, location);
	}
	void ReadStreamed(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location,
	                  FileReadProgress &progress) override {
		bool streamed;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			streamed = streams;
			if (streamed) {
				reads.emplace_back(location, FileReadTarget::TotalSize(targets));
			}
		}
		if (!streamed) {
			FileSystem::ReadStreamed(handle, targets, location, progress);
			return;
		}
		for (idx_t i = 0; i < targets.size(); i++) {
			{
				annotated_lock_guard<annotated_mutex> guard(lock);
				auto let_in = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
					return arrived > i;
				};
				lock.Await(absl::Condition(&let_in));
				if (failures > 0) {
					failures--;
					throw IOException("injected read failure");
				}
			}
			if (targets[i].data) {
				LocalFileSystem::Read(handle, targets[i].data, NumericCast<int64_t>(targets[i].size), location);
			}
			location += targets[i].size;
			progress.Filled(i + 1);
		}
	}

	//! Makes a request arrive target by target, as the body of a response does: its first [count] targets are let
	//! in, the next one waits
	void Arrive(idx_t count) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		streams = true;
		arrived = count;
	}

	//! Makes every read take [latency_p] plus the time its bytes take at [bandwidth_p] bytes per second, as one request
	//! to an object store does
	void Throttle(absl::Duration latency_p, double bandwidth_p) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		latency = latency_p;
		bandwidth = bandwidth_p;
	}

	idx_t ReadCount() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return reads.size();
	}

	void AwaitReads(idx_t count) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto arrived = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
			return reads.size() >= count;
		};
		lock.Await(absl::Condition(&arrived));
	}

	//! Whether [count] reads arrive within [timeout]
	bool AwaitReadsFor(idx_t count, absl::Duration timeout) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto arrived = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
			return reads.size() >= count;
		};
		return lock.AwaitWithTimeout(absl::Condition(&arrived), timeout);
	}

	//! Lets the read at [location] go while the others are held
	void ReleaseRead(idx_t location) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		released_reads.insert(location);
	}

	vector<pair<idx_t, idx_t>> TakeReads() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return std::move(reads);
	}

	void Fail(idx_t count) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		failures = count;
	}

	void Hold(bool value) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		hold = value;
	}

	optional<timestamp_t> GetCacheValidUntil(FileHandle &handle) override {
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (no_store) {
			return timestamp_t::ninfinity();
		}
		return optional<timestamp_t>();
	}

	void NoStore(bool value) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		no_store = value;
	}

	annotated_mutex lock;
	vector<pair<idx_t, idx_t>> reads DUCKDB_GUARDED_BY(lock);
	idx_t failures DUCKDB_GUARDED_BY(lock) = 0;
	bool hold DUCKDB_GUARDED_BY(lock) = false;
	unordered_set<idx_t> released_reads DUCKDB_GUARDED_BY(lock);
	bool no_store DUCKDB_GUARDED_BY(lock) = false;
	absl::Duration latency DUCKDB_GUARDED_BY(lock) = absl::ZeroDuration();
	double bandwidth DUCKDB_GUARDED_BY(lock) = 0;
	bool streams DUCKDB_GUARDED_BY(lock) = false;
	idx_t arrived DUCKDB_GUARDED_BY(lock) = 0;
};

class TestFile {
public:
	TestFile(const string &name, idx_t size) : path(TestCreatePath(name)) {
		LocalFileSystem fs;
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		vector<uint8_t> data(size);
		for (idx_t i = 0; i < size; i++) {
			data[i] = ExpectedByte(i);
		}
		handle->Write(QueryContext(), data.data(), size, 0);
		handle->Sync();
	}
	~TestFile() {
		LocalFileSystem fs;
		fs.TryRemoveFile(path);
	}

	shared_ptr<InputFile> Open(ExternalFileCache &cache, CountingSourceFileSystem &fs, const string &version = "v1",
	                           bool on_disk = false) const {
		shared_ptr<FileHandle> handle =
		    fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_PARALLEL_ACCESS);
		ExternalFileCacheFileInfo info;
		info.validation.file_size = handle->GetFileSize();
		info.validation.version_tag = version;
		info.on_disk = on_disk;
		return cache.OpenFile(path, info, true, std::move(handle));
	}

	const string path;
};

//! Appends [blocks] blocks to the unbacked [file], block b filled with the byte fill(b)
template <class FILL>
void AppendBlocks(const shared_ptr<InputFile> &file, idx_t blocks, FILL &&fill) {
	ExternalFileCacheAppender appender(file);
	vector<data_t> bytes(BLOCK);
	for (idx_t block = 0; block < blocks; block++) {
		memset(bytes.data(), int(fill(block)), BLOCK);
		appender.Append(bytes.data(), BLOCK);
	}
	appender.Finish();
}

//! Whether the [size] bytes at [data] are the bytes of the file from [offset] on
bool HoldsExpectedBytesAt(const_data_ptr_t data, idx_t size, idx_t offset) {
	for (idx_t i = 0; i < size; i++) {
		if (data[i] != ExpectedByte(offset + i)) {
			return false;
		}
	}
	return true;
}

//! Whether [pin] holds the bytes of the file from [offset] on
bool HoldsExpectedBytesAt(const FilePin &pin, idx_t offset) {
	return HoldsExpectedBytesAt(pin.Ptr(), pin.Size(), offset);
}

//! The value the bytes of the file hold at [offset]
template <class T>
T ExpectedValue(idx_t offset) {
	data_t bytes[sizeof(T)];
	for (idx_t i = 0; i < sizeof(T); i++) {
		bytes[i] = ExpectedByte(offset + i);
	}
	return Load<T>(bytes);
}

bool HoldsExpectedBytes(const FilePin &pin, idx_t block) {
	return HoldsExpectedBytesAt(pin, block * BLOCK);
}

//! Starts fetching every missing block of [offset, offset + size)
void PrefetchNeeded(ExternalFileCache &cache, InputFile &file, idx_t offset, idx_t size) {
	FileRange range {offset, size};
	cache.Prefetch(file, std::span(&range, 1), ExternalFileCachePrefetch::NEED);
}

//! A flag one task sets and another awaits
class Signal {
public:
	void Set() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		set = true;
	}
	//! Whether the flag is set within [timeout]
	bool AwaitFor(absl::Duration timeout) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto is_set = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
			return set;
		};
		return lock.AwaitWithTimeout(absl::Condition(&is_set), timeout);
	}

private:
	annotated_mutex lock;
	bool set DUCKDB_GUARDED_BY(lock) = false;
};

//! The files in [directory] whose names end in [suffix]
idx_t CountFiles(const string &directory, const string &suffix) {
	LocalFileSystem local;
	if (!local.DirectoryExists(directory)) {
		return 0;
	}
	idx_t count = 0;
	local.ListFiles(directory, [&](const string &name, bool is_directory) {
		if (!is_directory && StringUtil::EndsWith(name, suffix)) {
			count++;
		}
	});
	return count;
}

//! Gives the buffer pool of [config] room for [blocks] cache blocks, evicting to [temporary_directory], or nowhere
//! when it is empty
void LimitMemory(DBConfig &config, idx_t blocks, const string &temporary_directory) {
	config.options.maximum_memory = blocks * BLOCK;
	config.options.temporary_directory = temporary_directory;
	config.options.use_temporary_directory = !temporary_directory.empty();
}

} // namespace

TEST_CASE("External file cache pins hold the bytes of their block", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_pins.bin", 5 * BLOCK + 1000);
	auto cached = file.Open(cache, fs);

	REQUIRE(BlockCountOf(cached->GetSize()) == 6);
	for (idx_t block = 0; block < 6; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(pin.Size() == (block == 5 ? 1000 : BLOCK));
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 6);

	for (idx_t block = 0; block < 6; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 6);
}

TEST_CASE("External file cache reads of no bytes take nothing", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_no_bytes.bin", 3 * BLOCK);
	auto cached = file.Open(cache, fs);
	REQUIRE(cache.PinRange(*cached, BLOCK, 0).empty());
	const auto none = cached->PinCopy(BLOCK, 0);
	REQUIRE(!none.IsValid());
	REQUIRE(none.Size() == 0);
	REQUIRE_THROWS_AS(cached->PinCopy(2 * BLOCK, BLOCK + 1), DataCorruptionException);
	REQUIRE(fs.ReadCount() == 0);

	auto memory = cache.CreateUnbackedFile("memory://no_bytes/file");
	{
		ExternalFileCacheAppender appender(memory);
		appender.Commit(0);
		REQUIRE(appender.Size() == 0);
	}
	AppendBlocks(memory, 2, [](idx_t block) { return block + 1; });
	REQUIRE(cache.Pin(*memory, 1).Ptr()[0] == 2);
}

TEST_CASE("External file cache pins a byte range in one piece", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_pin_bytes.bin", 5 * BLOCK + 1000);
	auto cached = file.Open(cache, fs);

	const auto holds = [](const FilePin &pin, idx_t offset) {
		for (idx_t i = 0; i < pin.Size(); i++) {
			if (pin.Ptr()[i] != ExpectedByte(offset + i)) {
				return false;
			}
		}
		return true;
	};
	auto inside = cache.PinBytes(*cached, BLOCK + 100, 1000);
	REQUIRE(inside.Size() == 1000);
	REQUIRE(holds(inside, BLOCK + 100));
	auto block = cache.Pin(*cached, 1);
	REQUIRE(inside.Ptr() == block.Ptr() + 100);

	auto across = cache.PinBytes(*cached, 2 * BLOCK - 16, 2 * BLOCK + 32);
	REQUIRE(across.Size() == 2 * BLOCK + 32);
	REQUIRE(holds(across, 2 * BLOCK - 16));

	auto tail = cache.PinBytes(*cached, 5 * BLOCK + 900, 100);
	REQUIRE(tail.Size() == 100);
	REQUIRE(holds(tail, 5 * BLOCK + 900));
}

TEST_CASE("External file cache keeps one copy of bytes that lie in several blocks for all their readers",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_copies");
	DBConfig config;
	LimitMemory(config, 16, directory);
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_spill_size = 0"));
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_copies.bin", 40 * BLOCK);
	auto cached = file.Open(cache, fs);
	constexpr idx_t SHORT = BLOCK - 100;
	constexpr idx_t LONG = 4 * BLOCK + 8;
	constexpr idx_t LONG_SIZE = 2 * BLOCK;

	// less than a block comes through the blocks its neighbours share, in one request
	const_data_ptr_t copied;
	{
		auto first = cached->PinCopy(SHORT, 300);
		auto second = cached->PinCopy(SHORT, 300);
		REQUIRE(first.Size() == 300);
		REQUIRE(HoldsExpectedBytesAt(first, SHORT));
		REQUIRE(first.Ptr() == second.Ptr());
		copied = first.Ptr();
	}
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{0, 2 * BLOCK}});
	REQUIRE(cached->PinCopy(SHORT, 300).Ptr() == copied);

	// a pin of bytes read again and again is in place in one block, the shared copy in several, and its own copy
	// when the bytes are few
	{
		const InputFileReader reader(*cached);
		auto inside = reader.PinShared(BLOCK + 100, 8192);
		REQUIRE(inside.Ptr() == cache.Pin(*cached, 1).Ptr() + 100);
		auto across = reader.PinShared(2 * BLOCK - 4096, 8192);
		REQUIRE(HoldsExpectedBytesAt(across, 2 * BLOCK - 4096));
		REQUIRE(across.Ptr() == cached->PinCopy(2 * BLOCK - 4096, 8192).Ptr());
		auto few = reader.PinShared(2 * BLOCK - 100, 300);
		REQUIRE(HoldsExpectedBytesAt(few, 2 * BLOCK - 100));
		REQUIRE(few.Ptr() != reader.PinShared(2 * BLOCK - 100, 300).Ptr());
	}
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{2 * BLOCK, BLOCK}});

	// of the blocks a copy came from, the ones it covers leave first, and nothing asks for them again while the copy
	// is there
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	const auto limit = buffers.GetMaxMemory();
	{
		auto kept = cached->PinCopy(LONG, LONG_SIZE);
		REQUIRE(HoldsExpectedBytesAt(kept, LONG));
		REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{4 * BLOCK, 3 * BLOCK}});
		buffers.SetMemoryLimit(buffers.GetUsedMemory() - BLOCK);
		buffers.SetMemoryLimit(limit);
	}
	REQUIRE(!cache.IsCached(*cached, 5 * BLOCK, BLOCK));
	REQUIRE(cache.IsCached(*cached, 4 * BLOCK, BLOCK));
	REQUIRE(cache.IsHeld(*cached, LONG, LONG_SIZE));
	REQUIRE(cached->IsResident(LONG, LONG_SIZE));
	REQUIRE(cached->Warm(LONG, LONG_SIZE) == LONG_SIZE);
	REQUIRE(!cached->Hint(LONG, LONG_SIZE));
	cached->Prefetch(LONG, LONG_SIZE);
	FileRange ranges[] = {{39 * BLOCK, BLOCK}, {LONG, LONG_SIZE}};
	cached->Prefetch(ranges);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 39), 39));
	REQUIRE(HoldsExpectedBytesAt(cached->PinCopy(LONG, LONG_SIZE), LONG));
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{39 * BLOCK, BLOCK}});
	// neither do hinted ranges, also when the bytes next to the copy are hinted with it
	FileRange hinted[] = {{LONG, LONG_SIZE}, {LONG + LONG_SIZE, 8 * BLOCK - LONG - LONG_SIZE}};
	cached->Hint(hinted);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 7), 7));
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{7 * BLOCK, BLOCK}});

	// a copy no one pins leaves memory before any block of the file does, and is made again from the blocks
	buffers.SetMemoryLimit(buffers.GetUsedMemory() - BLOCK);
	buffers.SetMemoryLimit(limit);
	REQUIRE(!cache.IsHeld(*cached, LONG, LONG_SIZE));
	REQUIRE(cache.IsCached(*cached, 4 * BLOCK, BLOCK));
	REQUIRE(cache.IsCached(*cached, 39 * BLOCK, BLOCK));
	REQUIRE(HoldsExpectedBytesAt(cached->PinCopy(LONG, LONG_SIZE), LONG));
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{5 * BLOCK, BLOCK}});

	// switching the cache off drops the copies with the blocks; a pin taken before keeps its bytes
	auto held = cached->PinCopy(LONG, LONG_SIZE);
	cache.SetEnabled(false);
	cache.SetEnabled(true);
	REQUIRE(HoldsExpectedBytesAt(held, LONG));
	REQUIRE(!cache.IsHeld(*cached, LONG, LONG_SIZE));
	{
		auto again = cached->PinCopy(LONG, LONG_SIZE);
		REQUIRE(HoldsExpectedBytesAt(again, LONG));
		REQUIRE(again.Ptr() != held.Ptr());
		REQUIRE(again.Ptr() == cached->PinCopy(LONG, LONG_SIZE).Ptr());
	}
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{4 * BLOCK, 3 * BLOCK}});

	// readers that ask for the same bytes at once, before their copy is made, get the one copy from one request
	constexpr idx_t TOGETHER = 20 * BLOCK + 8;
	{
		constexpr idx_t READERS = 8;
		vector<FilePin> pins(READERS);
		vector<std::thread> threads;
		threads.reserve(READERS);
		for (idx_t i = 0; i < READERS; i++) {
			threads.emplace_back([&, i]() { pins[i] = cached->PinCopy(TOGETHER, LONG_SIZE); });
		}
		for (auto &thread : threads) {
			thread.join();
		}
		for (auto &pin : pins) {
			REQUIRE(HoldsExpectedBytesAt(pin, TOGETHER));
			REQUIRE(pin.Ptr() == pins[0].Ptr());
		}
	}
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{20 * BLOCK, 3 * BLOCK}});

	// a response that forbids reuse leaves no copy behind: each reader's copy is its own
	constexpr idx_t FORBIDDEN = 10 * BLOCK + 8;
	fs.NoStore(true);
	{
		auto first = cached->PinCopy(FORBIDDEN, LONG_SIZE);
		auto second = cached->PinCopy(FORBIDDEN, LONG_SIZE);
		REQUIRE(HoldsExpectedBytesAt(first, FORBIDDEN));
		REQUIRE(HoldsExpectedBytesAt(second, FORBIDDEN));
		REQUIRE(first.Ptr() != second.Ptr());
	}
	REQUIRE(cached->ReuseForbidden());
	REQUIRE(!cache.IsHeld(*cached, FORBIDDEN, LONG_SIZE));
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{10 * BLOCK, 3 * BLOCK}, {10 * BLOCK, 3 * BLOCK}});
}

TEST_CASE("External file cache pins share their bytes and keep them after the cache drops their block",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_shared_pins.bin", 8 * BLOCK);
	auto cached = file.Open(cache, fs);

	auto pin = cache.Pin(*cached, 0);
	auto shared = pin.Share(100, 1000);
	REQUIRE(shared.IsValid());
	REQUIRE(shared.Ptr() == pin.Ptr() + 100);
	REQUIRE(shared.Size() == 1000);
	pin.Reset();
	REQUIRE(cache.IsCached(*cached, 0, BLOCK));
	auto copy = cache.PinBytes(*cached, BLOCK - 10, 20);
	auto copy_shared = copy.Share(5, 10);
	REQUIRE(copy_shared.Ptr() == copy.Ptr() + 5);
	cache.ForgetFile(file.path);
	REQUIRE_FALSE(cache.IsCached(*cached, 0, BLOCK));
	for (idx_t i = 0; i < shared.Size(); i++) {
		REQUIRE(shared.Ptr()[i] == ExpectedByte(100 + i));
	}
	copy.Reset();
	for (idx_t i = 0; i < copy_shared.Size(); i++) {
		REQUIRE(copy_shared.Ptr()[i] == ExpectedByte(BLOCK - 5 + i));
	}
}

TEST_CASE("External file cache reopens of an unchanged file share its blocks", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_reopen.bin", 3 * BLOCK);
	{
		auto cached = file.Open(cache, fs);
		auto pin = cache.Pin(*cached, 1);
	}
	auto again = file.Open(cache, fs);
	auto pin = cache.Pin(*again, 1);
	REQUIRE(HoldsExpectedBytes(pin, 1));
	REQUIRE(fs.ReadCount() == 1);

	auto changed = file.Open(cache, fs, "v2");
	auto changed_pin = cache.Pin(*changed, 1);
	REQUIRE(fs.ReadCount() == 2);
}

TEST_CASE("External file cache fetches a missing block once for concurrent readers", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_once.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	atomic<idx_t> correct {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t i = 0; i < 16; i++) {
		executor.ScheduleTask(
		    [&]() {
			    auto pin = cache.Pin(*cached, 2);
			    if (HoldsExpectedBytes(pin, 2)) {
				    correct++;
			    }
		    },
		    "ExternalFileCacheTestTask");
	}
	fs.AwaitReads(1);
	fs.Hold(false);
	executor.WorkOnTasks();
	REQUIRE(correct == 16);
	REQUIRE(fs.ReadCount() == 1);
}

TEST_CASE("External file cache starts the queued fetch a reader waits for while every worker is busy",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 2;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_fetch_itself.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);

	auto &scheduler = TaskScheduler::GetScheduler(*db.instance);
	mutex lock;
	bool release = false;
	atomic<bool> occupied {false};
	TaskExecutor busy(scheduler, scheduler.CreateAccount(SchedulerClass::FOREGROUND));
	busy.ScheduleTask(
	    [&]() {
		    occupied = true;
		    lock.Lock();
		    lock.Await(absl::Condition(&release));
		    lock.Unlock();
	    },
	    "ExternalFileCacheOccupyWorker");
	for (idx_t i = 0; i < 3000 && !occupied.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(occupied.load());

	cached->ReadAhead(0, BLOCK, 0);
	atomic<bool> pinned {false};
	bool correct = false;
	std::thread reader([&]() {
		auto pin = cache.Pin(*cached, 0);
		correct = HoldsExpectedBytes(pin, 0);
		pinned = true;
	});
	for (idx_t i = 0; i < 1000 && !pinned.load(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	const bool in_time = pinned.load();
	{
		lock_guard<mutex> guard(lock);
		release = true;
	}
	reader.join();
	busy.WorkOnTasks();
	REQUIRE(in_time);
	REQUIRE(correct);
}

TEST_CASE("External file cache prefetch merges runs across small gaps and caps each request", "[external_file_cache]") {
	constexpr idx_t RANGE = REQUEST_BLOCKS + 2 * PIECE_BLOCKS + PIECE_BLOCKS / 4;
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_prefetch.bin", 2 * RANGE * BLOCK);
	auto cached = file.Open(cache, fs);

	for (idx_t block : {10, 11, 30}) {
		cache.Pin(*cached, block);
	}
	fs.TakeReads();

	cache.Prefetch(*cached, 0, RANGE * BLOCK);
	for (idx_t block = 0; block < RANGE; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	auto reads = fs.TakeReads();
	idx_t covered = 0;
	for (auto &read : reads) {
		REQUIRE(read.second <= PIECE_SIZE);
		REQUIRE(read.first % BLOCK == 0);
		covered += read.second;
	}
	REQUIRE(reads.size() == REQUEST_BLOCKS / PIECE_BLOCKS + 3);
	REQUIRE(covered == RANGE * BLOCK);
}

TEST_CASE("External file cache prefetch of several ranges reads the ones within the merge gap of each other at once",
          "[external_file_cache]") {
	constexpr idx_t GAP_BLOCKS = ExternalFileCache::MERGE_GAP / BLOCK;
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_prefetch_ranges.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	FileRange ranges[] = {{40 * BLOCK, BLOCK}, {(1 + GAP_BLOCKS) * BLOCK, BLOCK}, {BLOCK + 5, 10}};
	cache.Prefetch(*cached, ranges, ExternalFileCachePrefetch::NEED);
	for (idx_t block = 1; block <= 1 + GAP_BLOCKS; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	auto pin = cache.Pin(*cached, 40);
	REQUIRE(HoldsExpectedBytes(pin, 40));
	auto reads = fs.TakeReads();
	std::sort(reads.begin(), reads.end());
	REQUIRE(reads == vector<pair<idx_t, idx_t>> {{BLOCK, (1 + GAP_BLOCKS) * BLOCK}, {40 * BLOCK, BLOCK}});
}

TEST_CASE("External file cache prefetch of several ranges keeps apart the ones a request would not cover together",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_remote_max_block_size = 262144"));
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_prefetch_apart.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	FileRange ranges[] = {
	    {40 * BLOCK, BLOCK}, {5 * BLOCK, BLOCK}, {BLOCK + 5, 10}, {11 * BLOCK, 7}, {10 * BLOCK, BLOCK}};
	cache.Prefetch(*cached, ranges, ExternalFileCachePrefetch::NEED);
	for (const idx_t block : {1, 5, 10, 11, 40}) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	auto reads = fs.TakeReads();
	std::sort(reads.begin(), reads.end());
	REQUIRE(reads == vector<pair<idx_t, idx_t>> {
	                     {BLOCK, BLOCK}, {5 * BLOCK, BLOCK}, {10 * BLOCK, 2 * BLOCK}, {40 * BLOCK, BLOCK}});
}

TEST_CASE("Ranges hinted together are asked for in their order, the ones closer than a page as one, within a budget",
          "[external_file_cache]") {
	constexpr idx_t PAGE = PageResidency::PAGE_SIZE;
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_hinted_ranges.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	FileRange ranges[] = {{2 * BLOCK - 100, 50},
	                      {2 * BLOCK + PAGE - 100, 200},
	                      {10 * BLOCK, 10},
	                      {20 * BLOCK, BLOCK},
	                      {30 * BLOCK, BLOCK}};
	// the first two are one hint of a page and 200 bytes; the budget ends inside the fourth
	REQUIRE(cached->Hint(ranges, PAGE + 200 + 10 + 5) == 0);
	for (const idx_t block : {1, 2, 10, 20}) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE_FALSE(cache.IsCached(*cached, 30 * BLOCK, BLOCK));
	auto reads = fs.TakeReads();
	std::sort(reads.begin(), reads.end());
	REQUIRE(reads == vector<pair<idx_t, idx_t>> {{BLOCK, 2 * BLOCK}, {10 * BLOCK, BLOCK}, {20 * BLOCK, BLOCK}});
	// what a budget has left comes back
	FileRange again[] = {{2 * BLOCK - 100, 50}};
	REQUIRE(cached->Hint(again, 1000) == 950);

	// the cache makes the requests of a batch: ranges a request covers together share it
	FileRange near[] = {{40 * BLOCK, 10}, {42 * BLOCK, 10}, {50 * BLOCK, 10}};
	cached->Hint(near);
	for (const idx_t block : {40, 42, 50}) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	reads = fs.TakeReads();
	std::sort(reads.begin(), reads.end());
	REQUIRE(reads == vector<pair<idx_t, idx_t>> {{40 * BLOCK, 3 * BLOCK}, {50 * BLOCK, BLOCK}});
}

TEST_CASE("External file cache fetches blocks it holds again between two runs only when that takes fewer requests",
          "[external_file_cache]") {
	const auto fetches = [](idx_t piece, idx_t held, idx_t first, idx_t count) {
		DuckDB db(nullptr);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET external_file_cache_remote_max_block_size = " + to_string(piece)));
		CountingSourceFileSystem fs;
		ExternalFileCache cache(*db.instance);
		TestFile file("external_file_cache_bridged.bin", 64 * BLOCK);
		auto cached = file.Open(cache, fs);
		REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, held), held));
		fs.TakeReads();
		PrefetchNeeded(cache, *cached, first * BLOCK, count * BLOCK);
		for (idx_t block = first; block < first + count; block++) {
			REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, block), block));
		}
		auto reads = fs.TakeReads();
		std::sort(reads.begin(), reads.end());
		return reads;
	};
	// one request holds the three blocks: one fewer than a request for each side
	REQUIRE(fetches(32 * BLOCK, 11, 10, 3) == vector<pair<idx_t, idx_t>> {{10 * BLOCK, 3 * BLOCK}});
	// with requests of two blocks, five blocks take three either way: the block in between is left out
	REQUIRE(fetches(2 * BLOCK, 5, 3, 5) == vector<pair<idx_t, idx_t>> {{3 * BLOCK, 2 * BLOCK}, {6 * BLOCK, 2 * BLOCK}});
}

TEST_CASE("External file cache evicts the blocks a reader marked unused before the blocks in use",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_unused.bin", 8 * BLOCK);
	auto cached = file.Open(cache, fs, "v1", true);
	for (idx_t block = 0; block < 8; block++) {
		cache.Pin(*cached, block);
	}
	cache.MarkUnused(*cached, 4 * BLOCK + BLOCK / 2, 4 * BLOCK);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	buffers.SetMemoryLimit(buffers.GetUsedMemory() - 3 * BLOCK);
	for (idx_t block = 0; block < 8; block++) {
		REQUIRE(cache.IsCached(*cached, block * BLOCK, BLOCK) == (block < 5));
	}
}

TEST_CASE("External file cache drops a hinted prefetch past the bytes a file has in flight but never a needed one",
          "[external_file_cache]") {
	constexpr idx_t in_flight = BlockCountOf(ExternalFileCache::MAX_FILE_IN_FLIGHT);
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_need.bin", (in_flight + 44) * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	cache.Prefetch(*cached, 0, in_flight * BLOCK);
	cache.Prefetch(*cached, in_flight * BLOCK, 4 * BLOCK);
	const bool hint_dropped = !cache.IsCached(*cached, in_flight * BLOCK, 4 * BLOCK);
	PrefetchNeeded(cache, *cached, in_flight * BLOCK, 4 * BLOCK);
	const bool need_started = cache.IsCached(*cached, in_flight * BLOCK, 4 * BLOCK);
	fs.Hold(false);
	REQUIRE(hint_dropped);
	REQUIRE(need_started);
	for (idx_t block = 0; block < in_flight + 4; block++) {
		REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, block), block));
	}
}

TEST_CASE("External file cache reads ahead the bytes a reader reads next past the bytes in flight, and says where the "
          "bytes it asked for end",
          "[external_file_cache]") {
	constexpr idx_t in_flight = BlockCountOf(ExternalFileCache::MAX_FILE_IN_FLIGHT);
	constexpr idx_t first = in_flight + 8;
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_read_ahead_needed.bin", (first + 8) * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	cache.Prefetch(*cached, 0, in_flight * BLOCK);
	const auto needed = cached->ReadAhead(first * BLOCK + 100, 8 * BLOCK - 100, 3 * BLOCK);
	const bool needed_asked = cache.IsCached(*cached, first * BLOCK, 4 * BLOCK);
	const bool rest_dropped = !cache.IsCached(*cached, (first + 4) * BLOCK, BLOCK);
	const auto hinted = cached->ReadAhead((first + 4) * BLOCK, 4 * BLOCK, 0);
	const auto held = cached->ReadAhead(first * BLOCK, 4 * BLOCK, 0);
	fs.Hold(false);
	REQUIRE(needed == (first + 4) * BLOCK);
	REQUIRE(needed_asked);
	REQUIRE(rest_dropped);
	REQUIRE(hinted == (first + 4) * BLOCK);
	REQUIRE(held == (first + 4) * BLOCK);
	for (idx_t block = first; block < first + 4; block++) {
		REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, block), block));
	}
}

TEST_CASE("External file cache reads at most MAX_BURST bytes of remote files at once, a caller's read first",
          "[external_file_cache]") {
	constexpr idx_t PIECES = ExternalFileCache::MAX_BURST / PIECE_SIZE;
	constexpr idx_t PREFETCHED = (PIECES + 8) * PIECE_BLOCKS;
	DuckDB db(nullptr);
	TaskScheduler::GetScheduler(*db.instance).SetAsyncThreads(2 * PIECES);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_burst.bin", (PREFETCHED + 1) * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	PrefetchNeeded(cache, *cached, 0, PREFETCHED * BLOCK);
	fs.AwaitReads(PIECES);
	REQUIRE_FALSE(fs.AwaitReadsFor(PIECES + 1, absl::Milliseconds(200)));

	auto caller = std::async(std::launch::async, [&] { return cache.Pin(*cached, PREFETCHED); });
	REQUIRE_FALSE(fs.AwaitReadsFor(PIECES + 1, absl::Milliseconds(200)));
	const auto held = fs.TakeReads();
	REQUIRE(held.size() == PIECES);
	for (auto &read : held) {
		REQUIRE(read.second == PIECE_SIZE);
	}
	fs.ReleaseRead(held.front().first);
	fs.AwaitReads(1);
	REQUIRE_FALSE(fs.AwaitReadsFor(2, absl::Milliseconds(200)));
	REQUIRE(fs.TakeReads().front().first == PREFETCHED * BLOCK);

	fs.Hold(false);
	REQUIRE(HoldsExpectedBytes(caller.get(), PREFETCHED));
	for (idx_t block = 0; block < PREFETCHED; block += PIECE_BLOCKS) {
		REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, block), block));
	}
}

TEST_CASE("External file cache reads the request of a block a reader waits for ahead of the prefetched ones",
          "[external_file_cache]") {
	constexpr idx_t PIECES = ExternalFileCache::MAX_BURST / PIECE_SIZE;
	constexpr idx_t PREFETCHED = (PIECES + 8) * PIECE_BLOCKS;
	DuckDB db(nullptr);
	TaskScheduler::GetScheduler(*db.instance).SetAsyncThreads(2 * PIECES);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile prefetched_file("external_file_cache_awaited_prefetched.bin", PREFETCHED * BLOCK);
	TestFile read_file("external_file_cache_awaited_read.bin", 4 * BLOCK);
	auto prefetched = prefetched_file.Open(cache, fs);
	auto cached = read_file.Open(cache, fs);

	fs.Hold(true);
	PrefetchNeeded(cache, *prefetched, 0, PREFETCHED * BLOCK);
	fs.AwaitReads(PIECES);
	auto reader = std::async(std::launch::async, [&] {
		InputFileReader in(*cached);
		in.Seek(BLOCK);
		in.ReadAheadUntil(3 * BLOCK);
		return in.Read<uint64_t>();
	});
	const bool gated = !fs.AwaitReadsFor(PIECES + 1, absl::Milliseconds(200));
	const auto held = fs.TakeReads();
	fs.ReleaseRead(held.front().first);
	fs.AwaitReads(1);
	const auto next = fs.TakeReads().front();
	fs.Hold(false);
	REQUIRE(gated);
	REQUIRE(held.size() == PIECES);
	REQUIRE(next == std::make_pair(BLOCK, 2 * BLOCK));
	REQUIRE(reader.get() == ExpectedValue<uint64_t>(BLOCK));
}

TEST_CASE("A read-ahead window doubles each time its reader comes within half of it of what was asked",
          "[external_file_cache]") {
	constexpr idx_t MAX = 16 * BLOCK;
	const auto asks = [](FileRange range, idx_t offset, idx_t size) {
		return range.offset == offset && range.size == size;
	};
	ReadAheadWindow window;
	REQUIRE(window.Extend(0, MAX).size == 0);

	window.Arm(0, 100 * BLOCK, BLOCK);
	REQUIRE(window.Stop() == 100 * BLOCK);
	REQUIRE(asks(window.Extend(0, MAX), 0, BLOCK));
	REQUIRE(window.Extend(0, MAX).size == 0);
	REQUIRE(asks(window.Extend(BLOCK / 2, MAX), BLOCK, 2 * BLOCK));
	REQUIRE(window.Extend(BLOCK, MAX).size == 0);
	REQUIRE(asks(window.Extend(2 * BLOCK, MAX), 3 * BLOCK, 4 * BLOCK));
	REQUIRE(window.Extend(4 * BLOCK, MAX).size == 0);
	REQUIRE(asks(window.Extend(5 * BLOCK, MAX), 7 * BLOCK, 8 * BLOCK));
	// no window is larger than the maximum
	REQUIRE(asks(window.Extend(11 * BLOCK, MAX), 15 * BLOCK, 16 * BLOCK));
	REQUIRE(window.Extend(22 * BLOCK, MAX).size == 0);
	REQUIRE(asks(window.Extend(23 * BLOCK, MAX), 31 * BLOCK, 16 * BLOCK));
	// a reader past what was asked is asked for from where it is
	REQUIRE(asks(window.Extend(60 * BLOCK, MAX), 60 * BLOCK, 16 * BLOCK));

	// out of its order nothing counts as asked behind the reader and the window is small again; past unread bytes
	// it is half as large
	window.Restart(70 * BLOCK, 2 * BLOCK);
	REQUIRE(asks(window.Extend(70 * BLOCK, MAX), 70 * BLOCK, 2 * BLOCK));
	window.Halve(3 * BLOCK);
	REQUIRE(asks(window.Extend(71 * BLOCK, MAX), 72 * BLOCK, 3 * BLOCK));
	// bytes the bytes in flight left out are asked for again, by a window that starts over from the least size
	window.Rewind(73 * BLOCK);
	REQUIRE(window.Extend(73 * BLOCK, MAX).size == 0);
	REQUIRE(asks(window.Extend(73 * BLOCK, MAX, 3 * BLOCK), 73 * BLOCK, 3 * BLOCK));
	// nothing is asked for past the offset the reader stops at
	REQUIRE(asks(window.Extend(75 * BLOCK, MAX), 76 * BLOCK, 6 * BLOCK));
	REQUIRE(asks(window.Extend(80 * BLOCK, MAX), 82 * BLOCK, 12 * BLOCK));
	REQUIRE(asks(window.Extend(89 * BLOCK, MAX), 94 * BLOCK, 6 * BLOCK));
	REQUIRE(window.Extend(99 * BLOCK, MAX).size == 0);
	window.Until(104 * BLOCK);
	REQUIRE(asks(window.Extend(99 * BLOCK, MAX), 100 * BLOCK, 4 * BLOCK));

	// a caller's least size is what a small window asks for, and what it doubles from
	window.Arm(0, 100 * BLOCK, 0);
	REQUIRE(asks(window.Extend(0, MAX, 3 * BLOCK), 0, 3 * BLOCK));
	REQUIRE(window.Extend(0, MAX, 3 * BLOCK).size == 0);
	REQUIRE(asks(window.Extend(2 * BLOCK, MAX, 3 * BLOCK), 3 * BLOCK, 6 * BLOCK));
}

TEST_CASE("A cached file reader reads the bytes of a block in place and copies only the ones across two blocks",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	constexpr idx_t SIZE = 5 * BLOCK + 1000;
	TestFile file("input_file_reader.bin", SIZE);
	auto cached = file.Open(cache, fs);

	REQUIRE_FALSE(InputFileReader());
	InputFileReader in(*cached);
	REQUIRE(in);
	REQUIRE(in.File().get() == cached.get());
	REQUIRE(in.Position() == 0);
	REQUIRE(fs.ReadCount() == 0);

	in.Seek(BLOCK + 100);
	const auto inside = in.Peek(1000);
	REQUIRE(inside);
	REQUIRE(inside == cache.Pin(*cached, 1).Ptr() + 100);
	REQUIRE(HoldsExpectedBytesAt(inside, 1000, BLOCK + 100));
	REQUIRE(in.Position() == BLOCK + 100);
	in.Consume(1000);
	REQUIRE(in.Read<uint32_t>() == ExpectedValue<uint32_t>(BLOCK + 1100));
	REQUIRE(in.Position() == BLOCK + 1104);

	// bytes across two blocks are not read in place
	in.Seek(2 * BLOCK - 5);
	REQUIRE(in.Peek(5));
	REQUIRE_FALSE(in.Peek(6));
	REQUIRE(in.Read<uint64_t>() == ExpectedValue<uint64_t>(2 * BLOCK - 5));
	data_t across[64];
	in.Seek(3 * BLOCK - 20);
	in.ReadData(across, sizeof(across));
	REQUIRE(HoldsExpectedBytesAt(across, sizeof(across), 3 * BLOCK - 20));
	REQUIRE(in.Position() == 3 * BLOCK + 44);

	in.Skip(BLOCK);
	REQUIRE(in.Position() == 4 * BLOCK + 44);
	REQUIRE(in.Read<uint8_t>() == ExpectedByte(4 * BLOCK + 44));

	in.Seek(SIZE - 2);
	REQUIRE(in.Peek(2));
	REQUIRE_FALSE(in.Peek(3));
	REQUIRE_THROWS_AS(in.Read<uint32_t>(), DataCorruptionException);
	in.Seek(SIZE);
	REQUIRE_FALSE(in.Peek(1));
	REQUIRE_THROWS_AS(in.Read<uint8_t>(), DataCorruptionException);

	InputFileReader moved(std::move(in));
	moved.Seek(10);
	REQUIRE(moved.Read<uint16_t>() == ExpectedValue<uint16_t>(10));
	in = std::move(moved);
	REQUIRE(in.Position() == 12);
	REQUIRE(in.Read<uint16_t>() == ExpectedValue<uint16_t>(12));
}

TEST_CASE("A cached file reader pins the bytes of the block it is in with that block's pin, and bytes anywhere else",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	constexpr idx_t SIZE = 5 * BLOCK + 1000;
	TestFile file("input_file_reader_pins.bin", SIZE);
	auto cached = file.Open(cache, fs);

	InputFileReader in(*cached);
	in.Seek(BLOCK + 10);
	const auto window = in.Peek(64);
	REQUIRE(window);
	auto inside = in.Pin(BLOCK + 10, 64);
	REQUIRE(inside.IsValid());
	REQUIRE(inside.Ptr() == window);
	REQUIRE(inside.Size() == 64);
	REQUIRE(fs.ReadCount() == 1);

	auto elsewhere = in.Pin(3 * BLOCK + 7, 100);
	REQUIRE(elsewhere.Size() == 100);
	REQUIRE(HoldsExpectedBytesAt(elsewhere, 3 * BLOCK + 7));
	auto across = in.Pin(2 * BLOCK - 10, 20);
	REQUIRE(across.Size() == 20);
	REQUIRE(HoldsExpectedBytesAt(across, 2 * BLOCK - 10));
	REQUIRE(in.Position() == BLOCK + 10);

	// a pin keeps its bytes after the reader left the block and the cache dropped it
	in.Seek(4 * BLOCK);
	REQUIRE(in.Read<uint8_t>() == ExpectedByte(4 * BLOCK));
	cache.ForgetFile(file.path);
	REQUIRE(HoldsExpectedBytesAt(inside, BLOCK + 10));

	REQUIRE(in.Pin(SIZE, 0).Size() == 0);
	REQUIRE_THROWS_AS(in.Pin(SIZE - 1, 2), DataCorruptionException);
	REQUIRE_THROWS_AS(in.Pin(SIZE + 1, 0), DataCorruptionException);
}

TEST_CASE("A cached file reader has the cache read ahead of values read through the ends of blocks, up to the end "
          "it named",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("input_file_reader_read_ahead.bin", 200 * BLOCK);
	auto cached = file.Open(cache, fs);
	const auto read_blocks = [](InputFileReader &in, idx_t blocks) {
		for (idx_t i = 0; i < blocks * BLOCK / sizeof(uint64_t); i++) {
			in.Read<uint64_t>();
		}
	};

	// a reader that named no end is read ahead of once it went through the end of a block
	InputFileReader in_order(*cached);
	in_order.Seek(50 * BLOCK);
	read_blocks(in_order, 1);
	REQUIRE_FALSE(cache.IsCached(*cached, 51 * BLOCK, BLOCK));
	REQUIRE(in_order.Read<uint64_t>() == ExpectedValue<uint64_t>(51 * BLOCK));
	REQUIRE(cache.IsCached(*cached, 52 * BLOCK, 2 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 54 * BLOCK, BLOCK));

	// one that named its end gets its second block with the first, and no block past that end
	InputFileReader bounded(*cached);
	bounded.Seek(60 * BLOCK);
	bounded.ReadAheadUntil(63 * BLOCK);
	REQUIRE(bounded.Read<uint64_t>() == ExpectedValue<uint64_t>(60 * BLOCK));
	REQUIRE(cache.IsCached(*cached, 61 * BLOCK, BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 62 * BLOCK, BLOCK));
	bounded.Seek(60 * BLOCK);
	read_blocks(bounded, 3);
	REQUIRE(bounded.Position() == 63 * BLOCK);
	REQUIRE_FALSE(cache.IsCached(*cached, 63 * BLOCK, BLOCK));

	// a reader that scans up to its end goes on after a seek into the next block like one that read through; a
	// seek of its anywhere else starts anew
	InputFileReader scanning(*cached);
	scanning.Seek(140 * BLOCK);
	scanning.ReadAheadUntil(160 * BLOCK, ReadAheadSteps::SCANNING);
	REQUIRE(scanning.Read<uint64_t>() == ExpectedValue<uint64_t>(140 * BLOCK));
	REQUIRE(cache.IsCached(*cached, 141 * BLOCK, BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 142 * BLOCK, BLOCK));
	scanning.Seek(141 * BLOCK + 8 * 1024);
	REQUIRE(scanning.Read<uint64_t>() == ExpectedValue<uint64_t>(141 * BLOCK + 8 * 1024));
	REQUIRE(cache.IsCached(*cached, 142 * BLOCK, 2 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 144 * BLOCK, BLOCK));
	scanning.Seek(150 * BLOCK + 8 * 1024);
	REQUIRE(scanning.Read<uint64_t>() == ExpectedValue<uint64_t>(150 * BLOCK + 8 * 1024));
	REQUIRE_FALSE(cache.IsCached(*cached, 151 * BLOCK, BLOCK));

	// the steps of a reader's user ask while it reads more than it skips; a read of its through the end of a
	// block it entered in order asks whatever it skipped, and starts small
	InputFileReader stepping(*cached);
	stepping.Seek(100 * BLOCK);
	stepping.ReadAheadUntil(120 * BLOCK, ReadAheadSteps::SKIPPING);
	for (idx_t step = 0; step < 8; step++) {
		stepping.Advance();
		REQUIRE(stepping.Read<uint64_t>() == ExpectedValue<uint64_t>(100 * BLOCK + step * 1024));
		stepping.Skip(1024 - sizeof(uint64_t));
	}
	REQUIRE(cache.IsCached(*cached, 102 * BLOCK, 2 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 104 * BLOCK, BLOCK));
	stepping.Seek(110 * BLOCK);
	stepping.Advance();
	REQUIRE(stepping.Read<uint64_t>() == ExpectedValue<uint64_t>(110 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 111 * BLOCK, BLOCK));
	stepping.Seek(110 * BLOCK);
	read_blocks(stepping, 1);
	REQUIRE(stepping.Read<uint64_t>() == ExpectedValue<uint64_t>(111 * BLOCK));
	REQUIRE(cache.IsCached(*cached, 112 * BLOCK, 2 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 114 * BLOCK, BLOCK));

	// a user that steps through every byte is asked ahead of in windows that double; a window larger than a mapped
	// file's is asked for only once the user is within half of the window before of its end
	InputFileReader sequential(*cached);
	sequential.ReadAheadUntil(48 * BLOCK, ReadAheadSteps::SKIPPING);
	const auto step_to = [&](idx_t position) {
		while (sequential.Position() < position) {
			sequential.Advance();
			sequential.Read<uint64_t>();
			sequential.Skip(1024 - sizeof(uint64_t));
		}
	};
	step_to(8 * BLOCK);
	REQUIRE(cache.IsCached(*cached, 16 * BLOCK, 15 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 32 * BLOCK, BLOCK));
	step_to(16 * BLOCK);
	REQUIRE_FALSE(cache.IsCached(*cached, 32 * BLOCK, BLOCK));
	step_to(24 * BLOCK);
	REQUIRE(cache.IsCached(*cached, 32 * BLOCK, 16 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 48 * BLOCK, BLOCK));

	// a seek of a reader that does not scan says nothing of what it reads next
	InputFileReader landing(*cached);
	landing.Seek(170 * BLOCK);
	landing.ReadAheadUntil(190 * BLOCK);
	REQUIRE(landing.Read<uint64_t>() == ExpectedValue<uint64_t>(170 * BLOCK));
	landing.Seek(171 * BLOCK + 8 * 1024);
	REQUIRE(landing.Read<uint64_t>() == ExpectedValue<uint64_t>(171 * BLOCK + 8 * 1024));
	REQUIRE_FALSE(cache.IsCached(*cached, 172 * BLOCK, BLOCK));

	// a value across two blocks is one of a sequence, a longer read says nothing of what its reader reads next
	InputFileReader values(*cached);
	values.Seek(131 * BLOCK - 4);
	REQUIRE(values.Read<uint64_t>() == ExpectedValue<uint64_t>(131 * BLOCK - 4));
	REQUIRE(cache.IsCached(*cached, 132 * BLOCK, BLOCK));
	InputFileReader exact(*cached);
	exact.Seek(121 * BLOCK - 4);
	data_t bytes[16];
	exact.ReadData(bytes, sizeof(bytes));
	REQUIRE(HoldsExpectedBytesAt(bytes, sizeof(bytes), 121 * BLOCK - 4));
	REQUIRE_FALSE(cache.IsCached(*cached, 122 * BLOCK, BLOCK));
}

TEST_CASE("A cached file reader asks for two blocks at a time while the bytes in flight leave out its read-ahead",
          "[external_file_cache]") {
	constexpr idx_t in_flight = BlockCountOf(ExternalFileCache::MAX_FILE_IN_FLIGHT);
	constexpr idx_t first = in_flight + 8;
	constexpr idx_t blocks = 24;
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("input_file_reader_read_ahead_budget.bin", (first + blocks) * BLOCK);
	auto cached = file.Open(cache, fs, "v1", true);

	fs.Hold(true);
	cache.Prefetch(*cached, 0, in_flight * BLOCK);
	for (idx_t block = first; block < first + blocks; block++) {
		fs.ReleaseRead(block * BLOCK);
	}
	bool read_back = true;
	{
		InputFileReader in(*cached);
		in.Seek(first * BLOCK);
		in.ReadAheadUntil((first + blocks) * BLOCK);
		for (idx_t offset = first * BLOCK; offset < (first + blocks) * BLOCK; offset += sizeof(uint64_t)) {
			read_back = read_back && in.Read<uint64_t>() == ExpectedValue<uint64_t>(offset);
		}
	}
	auto reads = fs.TakeReads();
	fs.Hold(false);
	REQUIRE(read_back);
	std::erase_if(reads, [&](const pair<idx_t, idx_t> &read) { return read.first < first * BLOCK; });
	std::sort(reads.begin(), reads.end());
	REQUIRE(reads.size() == blocks / 2);
	for (idx_t i = 0; i < reads.size(); i++) {
		REQUIRE(reads[i] == std::make_pair((first + 2 * i) * BLOCK, 2 * BLOCK));
	}
}

TEST_CASE("A cached file reader that steps back into the block before the one it left reads ahead as in order",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("input_file_reader_step_back.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	// a reader that rereads the start of each group, which may lie in the block before
	InputFileReader in(*cached);
	in.ReadAheadUntil(64 * BLOCK, ReadAheadSteps::SCANNING);
	bool read_back = true;
	for (idx_t block = 1; block != 12; block++) {
		while (in.Position() < block * BLOCK + 64) {
			const auto at = in.Position();
			read_back = read_back && in.Read<uint64_t>() == ExpectedValue<uint64_t>(at);
		}
		in.Seek(block * BLOCK - 16);
	}
	REQUIRE(read_back);
	REQUIRE(cache.IsCached(*cached, 16 * BLOCK, 16 * BLOCK));
}

TEST_CASE("A cached file reader fetches the blocks of a long read at once, and reads bytes its user keeps a copy of "
          "from the source",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("input_file_reader_long.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	InputFileReader in(*cached);
	vector<data_t> bytes(16 * BLOCK);
	in.Seek(BLOCK / 2);
	in.ReadData(bytes.data(), bytes.size());
	REQUIRE(HoldsExpectedBytesAt(bytes.data(), bytes.size(), BLOCK / 2));
	REQUIRE(in.Position() == 16 * BLOCK + BLOCK / 2);
	REQUIRE(fs.ReadCount() == 1);
	REQUIRE(cache.IsCached(*cached, 0, 17 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 17 * BLOCK, BLOCK));

	// at least a block: straight from the source, and no block holds the bytes afterwards
	in.Seek(32 * BLOCK + 100);
	in.ReadOnce(bytes.data(), 8 * BLOCK);
	REQUIRE(HoldsExpectedBytesAt(bytes.data(), 8 * BLOCK, 32 * BLOCK + 100));
	REQUIRE(in.Position() == 40 * BLOCK + 100);
	REQUIRE(fs.ReadCount() == 2);
	REQUIRE_FALSE(cache.IsCached(*cached, 32 * BLOCK, BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 39 * BLOCK, BLOCK));
	// less than a block: through the blocks its neighbours share
	in.Seek(50 * BLOCK - 10);
	in.ReadOnce(bytes.data(), 100);
	REQUIRE(HoldsExpectedBytesAt(bytes.data(), 100, 50 * BLOCK - 10));
	REQUIRE(in.Position() == 50 * BLOCK + 90);
	REQUIRE(cache.IsCached(*cached, 49 * BLOCK, 2 * BLOCK));
	REQUIRE_THROWS_AS(cached->ReadOnce(64 * BLOCK - 1, bytes.data(), 2), DataCorruptionException);
}

TEST_CASE("A cached file reader of bytes in memory reads them in place and stops at their end",
          "[external_file_cache]") {
	vector<data_t> data(1000);
	for (idx_t i = 0; i < data.size(); i++) {
		data[i] = ExpectedByte(i);
	}
	InputFileReader in {std::span<const data_t>(data)};
	REQUIRE(in);
	REQUIRE_FALSE(in.File());
	REQUIRE(in.Peek(1000) == data.data());
	REQUIRE_FALSE(in.Peek(1001));
	in.Seek(10);
	REQUIRE(in.Read<uint32_t>() == ExpectedValue<uint32_t>(10));
	in.ReadAheadUntil(1000);
	const auto pin = in.Pin(900, 100);
	REQUIRE_FALSE(pin.IsValid());
	REQUIRE(pin.Ptr() == data.data() + 900);
	REQUIRE(pin.Size() == 100);
	REQUIRE_THROWS_AS(in.Pin(900, 101), DataCorruptionException);
	data_t copy[100];
	in.Seek(500);
	in.ReadOnce(copy, sizeof(copy));
	REQUIRE(HoldsExpectedBytesAt(copy, sizeof(copy), 500));
	REQUIRE(in.Position() == 600);
	in.Seek(998);
	REQUIRE_THROWS_AS(in.Read<uint32_t>(), DataCorruptionException);
	in.Seek(2000);
	REQUIRE_THROWS_AS(in.Read<uint8_t>(), DataCorruptionException);
}

TEST_CASE("A file system opens a local file mapped, and its readers read the mapping in place",
          "[external_file_cache]") {
	constexpr idx_t SIZE = 3 * BLOCK + 123;
	constexpr idx_t PAGE = PageResidency::PAGE_SIZE;
	LocalFileSystem fs;
	TestFile file("cached_file_mapped.bin", SIZE);
	auto mapped = fs.OpenInputFile(file.path, MappedFileAccess::RANDOM);
	REQUIRE(mapped->IsMapped());
	REQUIRE(mapped->IsPaged());
	REQUIRE(mapped->GetSize() == SIZE);
	REQUIRE(mapped->GetPath() == file.path);
	REQUIRE(mapped->Mapping());
	REQUIRE(HoldsExpectedBytesAt(mapped->Data(), SIZE, 0));

	const auto pin = mapped->Pin(BLOCK - 10, 20);
	REQUIRE_FALSE(pin.IsValid());
	REQUIRE(pin.Ptr() == mapped->Data() + BLOCK - 10);
	REQUIRE(pin.Size() == 20);
	REQUIRE_THROWS_AS(mapped->Pin(SIZE - 1, 2), DataCorruptionException);

	InputFileReader in(*mapped);
	REQUIRE(in.File().get() == mapped.get());
	REQUIRE(in.Peek(SIZE) == mapped->Data());
	in.Seek(2 * BLOCK - 3);
	in.ReadAheadUntil(SIZE);
	REQUIRE(in.Read<uint64_t>() == ExpectedValue<uint64_t>(2 * BLOCK - 3));
	REQUIRE(in.Pin(BLOCK - 10, 20).Ptr() == pin.Ptr());
	in.Seek(SIZE - 1);
	REQUIRE_THROWS_AS(in.Read<uint16_t>(), DataCorruptionException);

	MemoryReader unchecked(*mapped);
	REQUIRE(unchecked);
	REQUIRE(unchecked.File().get() == mapped.get());
	REQUIRE(unchecked.Data() == mapped->Data());
	unchecked.Seek(BLOCK + 5);
	REQUIRE(unchecked.Read<uint32_t>() == ExpectedValue<uint32_t>(BLOCK + 5));
	unchecked.Seek(SIZE - 1);
	data_t past[2];
	REQUIRE_THROWS_AS(unchecked.ReadData(past, sizeof(past)), DataCorruptionException);

	// what the file remembers of its pages lasts until the process may have lost some
	REQUIRE(mapped->Warm(0, 3 * PAGE) == 0);
	mapped->MarkWarm(100, 2 * PAGE - 100);
	REQUIRE(mapped->Warm(0, 3 * PAGE) == 2 * PAGE);
	REQUIRE(mapped->Warm(PAGE + 7, 100) == 100);
	REQUIRE(mapped->Warm(2 * PAGE, PAGE) == 0);
	auto epoch = PageResidency::Epoch();
	PageResidency::Invalidate();
	REQUIRE(mapped->Warm(0, 2 * PAGE) == 0);
	REQUIRE(mapped->Evicted(0, 2 * PAGE, epoch));
	REQUIRE(epoch == PageResidency::Epoch());
	REQUIRE_FALSE(mapped->Evicted(0, 2 * PAGE, epoch));

	// the steps of a mapped file's readers have the pages ahead of them asked for once a page was read in order
	MemoryReader stepping(*mapped);
	stepping.ReadAheadUntil(SIZE, ReadAheadSteps::SKIPPING);
	InputFileReader stepping_checked(*mapped);
	stepping_checked.Seek(2 * BLOCK);
	stepping_checked.ReadAheadUntil(SIZE, ReadAheadSteps::SKIPPING);
	for (idx_t step = 0; step < 4; step++) {
		stepping.Advance();
		stepping.Skip(PAGE / 4);
		stepping_checked.Advance();
		stepping_checked.Skip(PAGE / 4);
	}
	REQUIRE(mapped->Warm(PAGE, PAGE) == 0);
	REQUIRE(mapped->Warm(2 * BLOCK + PAGE, PAGE) == 0);
	stepping.Advance();
	stepping_checked.Advance();
	REQUIRE(mapped->Warm(PAGE, InputFile::HINT_CHUNK) == InputFile::HINT_CHUNK);
	REQUIRE(mapped->Warm(2 * BLOCK + PAGE, BLOCK - PAGE) == BLOCK - PAGE);

	vector<data_t> copy(SIZE);
	in.Seek(0);
	in.ReadOnce(copy.data(), SIZE);
	REQUIRE(HoldsExpectedBytesAt(copy.data(), SIZE, 0));
	REQUIRE(in.Position() == SIZE);
	REQUIRE_THROWS_AS(mapped->ReadOnce(1, copy.data(), SIZE), DataCorruptionException);

	// hints never fail and change no byte, whatever the kernel does with them
	FileRange ranges[] = {{BLOCK, 100}, {0, 10}, {SIZE - 1, 1}};
	mapped->Prefetch(ranges);
	mapped->Prefetch(BLOCK, SIZE);
	mapped->Hint(BLOCK, BLOCK);
	mapped->Evict(0, SIZE);
	REQUIRE(HoldsExpectedBytesAt(mapped->Data(), SIZE, 0));

	TestFile empty("cached_file_mapped_empty.bin", 0);
	auto none = fs.OpenInputFile(empty.path);
	REQUIRE(none->IsMapped());
	REQUIRE(none->GetSize() == 0);
	REQUIRE_FALSE(none->Data());
	REQUIRE_FALSE(none->Mapping());
	MemoryReader nothing(*none);
	REQUIRE(nothing);
	REQUIRE(nothing.Position() == 0);
	REQUIRE_THROWS_AS(InputFileReader(*none).Read<uint8_t>(), DataCorruptionException);
}

TEST_CASE("A read-ahead window asks ahead of where its reader lands past what was asked", "[external_file_cache]") {
	constexpr idx_t SIZE = 64 * BLOCK;
	constexpr idx_t PAGE = PageResidency::PAGE_SIZE;
	LocalFileSystem fs;
	TestFile file("read_ahead_landing.bin", SIZE);
	auto mapped = fs.OpenInputFile(file.path, MappedFileAccess::RANDOM);
	REQUIRE(mapped->IsMapped());
	mapped->Evict(0, SIZE);
	// the kernel may keep pages a request to drop them leaves: a landing on a page in memory asks nothing
	const auto asked_at = [&](idx_t at) {
		return PageResidency::IsResident(mapped->Data() + at, PAGE) ? idx_t(0) : InputFile::HINT_CHUNK - PAGE;
	};

	// a reader armed where its list starts and sent to a block past what was asked asks a window from there at once
	MemoryReader reader(*mapped);
	reader.ReadAheadUntil(SIZE, ReadAheadSteps::SKIPPING);
	auto asked = asked_at(16 * BLOCK);
	reader.Seek(16 * BLOCK);
	reader.Advance();
	REQUIRE(mapped->Warm(16 * BLOCK + PAGE, InputFile::HINT_CHUNK - PAGE) == asked);

	// a step inside what was asked asks nothing more until the reader read a page
	reader.Seek(16 * BLOCK + 2 * PAGE);
	reader.Advance();
	REQUIRE(mapped->Warm(16 * BLOCK + InputFile::HINT_CHUNK, PAGE) == 0);

	// a landing past what was asked asks a window from there again
	asked = asked_at(40 * BLOCK);
	reader.Seek(40 * BLOCK);
	reader.Advance();
	REQUIRE(mapped->Warm(40 * BLOCK + PAGE, InputFile::HINT_CHUNK - PAGE) == asked);
	REQUIRE(mapped->Warm(40 * BLOCK + InputFile::HINT_CHUNK, PAGE) == 0);

	// a landing on a page the file knows to be in memory asks nothing
	mapped->MarkWarm(56 * BLOCK, PAGE);
	reader.Seek(56 * BLOCK);
	reader.Advance();
	REQUIRE(mapped->Warm(56 * BLOCK + PAGE, PAGE) == 0);

	// a landing where its reader stops asks nothing
	MemoryReader stopping(*mapped);
	stopping.ReadAheadUntil(4 * PAGE, ReadAheadSteps::SKIPPING);
	stopping.Seek(4 * PAGE);
	stopping.Advance();
	REQUIRE(mapped->Warm(4 * PAGE, PAGE) == 0);
}

TEST_CASE("A mapped file is read once a window at a time when the read is longer than the kernel is asked to hold",
          "[external_file_cache]") {
	constexpr idx_t SIZE = 160ULL * 1024 * 1024;
	LocalFileSystem fs;
	const auto path = TestCreatePath("cached_file_mapped_large.bin");
	fs.TryRemoveFile(path);
	{
		// a file of zeros that takes no disk space, with one byte at its end
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		data_t last = 42;
		handle->Write(QueryContext(), &last, 1, SIZE - 1);
	}
	{
		auto mapped = fs.OpenInputFile(path, MappedFileAccess::SEQUENTIAL);
		vector<data_t> bytes(SIZE, 0xFF);
		mapped->ReadOnce(0, bytes.data(), SIZE);
		REQUIRE(bytes[0] == 0);
		REQUIRE(bytes[SIZE / 2] == 0);
		REQUIRE(bytes[SIZE - 2] == 0);
		REQUIRE(bytes[SIZE - 1] == 42);
	}
	fs.RemoveFile(path);
}

TEST_CASE("External file cache fetches a long run in concurrent pieces and hands out each piece's blocks as it lands",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 4;
	constexpr idx_t RUN = 4 * PIECE_BLOCKS;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_pieces.bin", RUN * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	cache.Prefetch(*cached, 0, RUN * BLOCK);
	const bool concurrent = fs.AwaitReadsFor(4, absl::Seconds(10));
	fs.ReleaseRead(0);
	atomic<bool> right {false};
	Signal pinned;
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	executor.ScheduleTask(
	    [&]() {
		    absl::Cleanup done = [&]() {
			    pinned.Set();
		    };
		    right = HoldsExpectedBytes(cache.Pin(*cached, 0), 0);
	    },
	    "ExternalFileCacheTestTask");
	const bool first_landed = pinned.AwaitFor(absl::Seconds(10));
	fs.Hold(false);
	executor.WorkOnTasks();
	REQUIRE(concurrent);
	REQUIRE(first_landed);
	REQUIRE(right);

	auto pins = cache.PinRange(*cached, 0, RUN * BLOCK);
	REQUIRE(pins.size() == RUN);
	for (idx_t block = 0; block < RUN; block++) {
		REQUIRE(HoldsExpectedBytes(pins[block], block));
	}
	auto reads = fs.TakeReads();
	REQUIRE(reads.size() == 4);
	idx_t covered = 0;
	for (auto &read : reads) {
		REQUIRE(read.second <= PIECE_SIZE);
		covered += read.second;
	}
	REQUIRE(covered == RUN * BLOCK);
}

TEST_CASE("External file cache hands out each block of a request once its bytes are in", "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 4;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_arrival.bin", 6 * BLOCK);
	auto cached = file.Open(cache, fs);

	absl::Cleanup arrive = [&]() {
		fs.Arrive(NumericLimits<idx_t>::Maximum());
	};

	// the request waits for its second block while the first is read
	fs.Arrive(1);
	PrefetchNeeded(cache, *cached, 0, 4 * BLOCK);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 0), 0));
	REQUIRE(cache.TryPinRange(*cached, BLOCK, BLOCK).empty());
	fs.Arrive(3);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 2), 2));
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 1), 1));
	REQUIRE(cache.TryPinRange(*cached, 3 * BLOCK, BLOCK).empty());

	// a request that fails keeps the blocks it handed out; the rest is fetched again
	fs.Fail(1);
	fs.Arrive(4);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 3), 3));
	REQUIRE(cache.IsCached(*cached, 0, 4 * BLOCK));
	auto reads = fs.TakeReads();
	REQUIRE(reads == vector<pair<idx_t, idx_t>> {{0, 4 * BLOCK}, {3 * BLOCK, BLOCK}});

	// a response that forbids reuse hands its blocks to the pins of its request only
	fs.NoStore(true);
	auto pins = cache.PinRange(*cached, 4 * BLOCK, 2 * BLOCK);
	REQUIRE(pins.size() == 2);
	REQUIRE(HoldsExpectedBytes(pins[0], 4));
	REQUIRE(HoldsExpectedBytes(pins[1], 5));
	REQUIRE(!cache.IsCached(*cached, 4 * BLOCK, 2 * BLOCK));
	REQUIRE(fs.TakeReads() == vector<pair<idx_t, idx_t>> {{4 * BLOCK, 2 * BLOCK}});
}

TEST_CASE("External file cache gives a file version the block size set when the cache starts holding it",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	CountingSourceFileSystem large_fs;
	CountingSourceFileSystem small_fs;
	ExternalFileCache cache(*db.instance);
	constexpr idx_t SMALL = 32 * 1024;
	constexpr idx_t SIZE = 3 * BLOCK + 100;
	TestFile large_file("external_file_cache_large_blocks.bin", SIZE);
	TestFile small_file("external_file_cache_small_blocks.bin", SIZE);
	auto large = large_file.Open(cache, large_fs);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_remote_min_block_size = 32768"));
	auto small = small_file.Open(cache, small_fs);
	auto reopened = large_file.Open(cache, large_fs);
	REQUIRE(large->BlockSize() == BLOCK);
	REQUIRE(reopened->BlockSize() == BLOCK);
	REQUIRE(small->BlockSize() == SMALL);

	auto pin = cache.Pin(*small, 5);
	REQUIRE(pin.Size() == SMALL);
	REQUIRE(HoldsExpectedBytesAt(pin, 5 * SMALL));
	REQUIRE(cache.Pin(*large, 3).Size() == 100);
	REQUIRE(cache.Pin(*small, SIZE / SMALL).Size() == 100);
	for (auto &file : {large, small}) {
		const auto block_size = file->BlockSize();
		auto pins = cache.PinRange(*file, block_size - 10, block_size + 20);
		REQUIRE(pins.size() == 3);
		idx_t at = block_size - 10;
		for (auto &range_pin : pins) {
			REQUIRE(HoldsExpectedBytesAt(range_pin, at));
			at += range_pin.Size();
		}
		REQUIRE(at == 2 * block_size + 10);
		auto bytes = cache.PinBytes(*file, block_size - 10, 20);
		REQUIRE(bytes.Size() == 20);
		REQUIRE(HoldsExpectedBytesAt(bytes, block_size - 10));
	}
	for (auto &read : small_fs.TakeReads()) {
		REQUIRE(read.first % SMALL == 0);
	}
	for (auto &read : large_fs.TakeReads()) {
		REQUIRE(read.first % BLOCK == 0);
	}
}

TEST_CASE("External file cache fetches a long run in pieces of the remote or local max block size",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 4;
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_remote_max_block_size = 1048576"));
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_local_max_block_size = 524288"));
	ExternalFileCache cache(*db.instance);
	constexpr idx_t SIZE = 4ULL * 1024ULL * 1024ULL;
	for (const bool on_disk : {false, true}) {
		CountingSourceFileSystem fs;
		TestFile file(on_disk ? "external_file_cache_local_pieces.bin" : "external_file_cache_remote_pieces.bin", SIZE);
		auto cached = file.Open(cache, fs, "v1", on_disk);
		auto pins = cache.PinRange(*cached, 0, SIZE);
		REQUIRE(pins.size() == SIZE / BLOCK);
		const idx_t piece = on_disk ? 524288 : 1048576;
		auto reads = fs.TakeReads();
		REQUIRE(reads.size() == SIZE / piece);
		for (auto &read : reads) {
			REQUIRE(read.second == piece);
		}
	}
}

TEST_CASE("External file cache range pins fetch the runs and pieces of a range concurrently", "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 4;
	constexpr idx_t FULL = 3 * PIECE_BLOCKS;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_concurrent_runs.bin", FULL * BLOCK + 100);
	auto cached = file.Open(cache, fs);
	for (idx_t block = PIECE_BLOCKS / 2; block < PIECE_BLOCKS; block++) {
		cache.Pin(*cached, block);
	}
	fs.TakeReads();

	fs.Hold(true);
	vector<FilePin> pins;
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	executor.ScheduleTask([&]() { pins = cache.PinRange(*cached, 0, FULL * BLOCK + 100); },
	                      "ExternalFileCacheTestTask");
	const bool concurrent = fs.AwaitReadsFor(4, absl::Seconds(10));
	fs.Hold(false);
	executor.WorkOnTasks();
	REQUIRE(concurrent);
	REQUIRE(pins.size() == FULL + 1);
	for (idx_t block = 0; block <= FULL; block++) {
		REQUIRE(HoldsExpectedBytes(pins[block], block));
	}
	auto reads = fs.TakeReads();
	REQUIRE(reads.size() == 4);
	idx_t covered = 0;
	for (auto &read : reads) {
		REQUIRE(read.second <= PIECE_SIZE);
		covered += read.second;
	}
	REQUIRE(covered == (FULL - PIECE_BLOCKS / 2) * BLOCK + 100);
}

TEST_CASE("External file cache fetches the pieces of long runs when the scheduler has no workers",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	constexpr idx_t RUN = 2 * PIECE_BLOCKS + PIECE_BLOCKS / 2;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_no_workers_pieces.bin", RUN * BLOCK);
	{
		auto cached = file.Open(cache, fs);
		cache.Prefetch(*cached, 0, RUN * BLOCK);
		REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, RUN - 1), RUN - 1));
		auto pins = cache.PinRange(*cached, 0, RUN * BLOCK);
		REQUIRE(pins.size() == RUN);
		for (idx_t block = 0; block < RUN; block++) {
			REQUIRE(HoldsExpectedBytes(pins[block], block));
		}
		REQUIRE(fs.TakeReads().size() == 3);
	}
	auto changed = file.Open(cache, fs, "v2");
	auto pins = cache.PinRange(*changed, 0, RUN * BLOCK);
	REQUIRE(pins.size() == RUN);
	for (idx_t block = 0; block < RUN; block++) {
		REQUIRE(HoldsExpectedBytes(pins[block], block));
	}
	REQUIRE(fs.TakeReads().size() == 3);
}

TEST_CASE("External file cache range pins fetch the missing blocks of a range in one request",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_range.bin", 20 * BLOCK + 10);
	auto cached = file.Open(cache, fs);
	cache.Pin(*cached, 5);
	fs.TakeReads();

	auto pins = cache.PinRange(*cached, 2 * BLOCK + 100, 10 * BLOCK);
	REQUIRE(pins.size() == 11);
	idx_t at = 2 * BLOCK + 100;
	for (auto &pin : pins) {
		REQUIRE(HoldsExpectedBytesAt(pin, at));
		at += pin.Size();
	}
	REQUIRE(at == 12 * BLOCK + 100);
	auto reads = fs.TakeReads();
	REQUIRE(reads.size() == 1);
	REQUIRE(reads[0] == make_pair(2 * BLOCK, 11 * BLOCK));

	REQUIRE(cache.TryPinRange(*cached, 2 * BLOCK, 11 * BLOCK).size() == 11);
	REQUIRE(cache.TryPinRange(*cached, 0, 3 * BLOCK).empty());
	REQUIRE(fs.ReadCount() == 0);
}

TEST_CASE("External file cache range read uses a response that forbids reuse once, without workers",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_no_store.bin", 8 * BLOCK);
	auto cached = file.Open(cache, fs);
	fs.NoStore(true);
	{
		auto pins = cache.PinRange(*cached, 0, 8 * BLOCK);
		REQUIRE(pins.size() == 8);
		for (idx_t i = 0; i < pins.size(); i++) {
			REQUIRE(HoldsExpectedBytes(pins[i], i));
		}
		REQUIRE(fs.ReadCount() == 1);
		REQUIRE_FALSE(cache.IsCached(*cached, 3 * BLOCK, BLOCK));
	}
	REQUIRE(cache.GetStatistics().used == 0);
	auto pin = cache.Pin(*cached, 3);
	REQUIRE(HoldsExpectedBytes(pin, 3));
	REQUIRE(fs.ReadCount() == 2);
}

TEST_CASE("External file cache readers of prefetched blocks run the fetches when the scheduler has no workers",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_no_workers.bin", 16 * BLOCK);
	auto cached = file.Open(cache, fs);

	cache.Prefetch(*cached, 0, 8 * BLOCK);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 3), 3));
	cache.Prefetch(*cached, 8 * BLOCK, 4 * BLOCK);
	cache.Prefetch(*cached, 12 * BLOCK, 4 * BLOCK);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 14), 14));
	auto pins = cache.PinRange(*cached, 0, 16 * BLOCK);
	REQUIRE(pins.size() == 16);
	for (idx_t block = 0; block < 16; block++) {
		REQUIRE(HoldsExpectedBytes(pins[block], block));
	}
	REQUIRE(fs.ReadCount() == 3);
}

TEST_CASE("External file cache drops pinned blocks of a forgotten file with their last pin", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_forget.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);
	auto held = cache.Pin(*cached, 1);
	cache.Pin(*cached, 2);
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);

	cache.ForgetFile(file.path);
	REQUIRE(HoldsExpectedBytes(held, 1));
	REQUIRE_FALSE(cache.IsCached(*cached, BLOCK, BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 2 * BLOCK, BLOCK));
	REQUIRE(cache.GetStatistics().used == BLOCK);
	held.Reset();
	REQUIRE(cache.GetStatistics().used == 0);
	ExternalFileCacheFileInfo info;
	REQUIRE_FALSE(cache.TryGetFile(file.path, info));
}

namespace {

void WriteBlocks(ExternalFileCacheAppender &appender, idx_t size) {
	vector<data_t> bytes(size);
	for (idx_t i = 0; i < size; i++) {
		bytes[i] = ExpectedByte(i);
	}
	appender.Append(bytes.data(), size);
}

shared_ptr<InputFile> OpenToRead(ExternalFileCache &cache, CountingSourceFileSystem &fs, const TestFile &file) {
	shared_ptr<FileHandle> handle =
	    fs.OpenFile(file.path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_PARALLEL_ACCESS);
	ExternalFileCacheFileInfo info;
	info.validation.file_size = handle->GetFileSize();
	return cache.OpenFile(file.path, info, false, std::move(handle));
}

} // namespace

TEST_CASE("External file cache shares the blocks a writer created once the file is published",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_written.bin", 2 * BLOCK + 100);
	{
		ExternalFileCacheAppender appender(cache.CreateWrittenFile(file.path));
		WriteBlocks(appender, 2 * BLOCK + 100);
		appender.Finish();
	}
	auto cached = OpenToRead(cache, fs, file);
	REQUIRE(cached->GetSize() == 2 * BLOCK + 100);
	for (idx_t block = 0; block < 3; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(pin.Size() == (block == 2 ? 100 : BLOCK));
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 0);
}

TEST_CASE("External file cache drops the blocks of a written file that is abandoned", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_abandoned.bin", 2 * BLOCK);
	{
		ExternalFileCacheAppender appender(cache.CreateWrittenFile(file.path));
		WriteBlocks(appender, 2 * BLOCK);
		REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
		appender.Abandon();
	}
	REQUIRE(cache.GetStatistics().used == 0);
	auto cached = OpenToRead(cache, fs, file);
	auto pin = cache.Pin(*cached, 1);
	REQUIRE(HoldsExpectedBytes(pin, 1));
	REQUIRE(fs.ReadCount() == 1);
}

TEST_CASE("External file cache appender publishes a written file at the size appended, or drops it",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_appended.bin", 2 * BLOCK + 100);
	vector<uint8_t> data(2 * BLOCK + 100);
	for (idx_t i = 0; i < data.size(); i++) {
		data[i] = ExpectedByte(i);
	}
	{
		ExternalFileCacheAppender appender(cache.CreateWrittenFile(file.path));
		appender.Append(data.data(), 1000);
		appender.Append(data.data() + 1000, data.size() - 1000);
		REQUIRE(appender.Size() == data.size());
		appender.Finish();
	}
	auto cached = OpenToRead(cache, fs, file);
	REQUIRE(cached->GetSize() == data.size());
	for (idx_t block = 0; block < 3; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 0);
	cached.reset();
	cache.ForgetFile(file.path);

	ExternalFileCacheAppender abandoned(cache.CreateWrittenFile(file.path));
	abandoned.Append(data.data(), BLOCK + 1);
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
	abandoned.Abandon();
	REQUIRE(cache.GetStatistics().used == 0);
}

TEST_CASE("External file cache knows a published written file as the version its writer names",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	ExternalFileCache cache(*db.instance);
	const string path = "external_file_cache_versioned.bin";
	const auto modified = Timestamp::FromEpochSeconds(1700000000);
	{
		ExternalFileCacheAppender appender(cache.CreateWrittenFile(path));
		vector<uint8_t> data(BLOCK + 10, 7);
		appender.Append(data.data(), data.size());
		appender.Finish("\"etag-1\"", modified);
	}
	ExternalFileCacheFileInfo found;
	REQUIRE(cache.TryGetFile(path, found));
	REQUIRE(found.validation.file_size == BLOCK + 10);
	REQUIRE(found.validation.version_tag == "\"etag-1\"");
	REQUIRE(found.validation.last_modified == modified);

	auto reopened = cache.OpenFile(path, found, false, nullptr);
	REQUIRE(reopened->Info().validation.version_tag == "\"etag-1\"");
	REQUIRE(cache.IsCached(*reopened, 0, BLOCK + 10));
}

namespace {

//! Serves remote://<path> from the local file at <path>, counting the opens that were handed a version to expect
class RemoteTestFileSystem : public LocalFileSystem {
public:
	static constexpr const char *PREFIX = "remote://";

	string GetName() const override {
		return "RemoteTestFileSystem";
	}
	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, PREFIX);
	}
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		return LocalFileSystem::OpenFile(Local(path), flags, opener);
	}
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override {
		LocalFileSystem::MoveFile(Local(source), Local(target), opener);
	}
	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override {
		LocalFileSystem::RemoveFile(Local(filename), opener);
	}
	bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener = nullptr) override {
		if (filename == failing_removal) {
			throw IOException("cannot remove \"%s\"", filename);
		}
		return LocalFileSystem::TryRemoveFile(Local(filename), opener);
	}
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options,
	                             optional_ptr<FileOpener> opener = nullptr) override {
		return LocalFileSystem::RemoveDirectoryExtended(Local(directory), options, opener);
	}

	//! The version an upload gets, as an object store's ETag
	static constexpr const char *UPLOAD_TAG = "\"upload\"";
	string GetVersionTag(FileHandle &handle) override {
		return UPLOAD_TAG;
	}
	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		return Timestamp::FromEpochSeconds(1700000100);
	}
	//! An aborted upload leaves no file, as an aborted multipart upload does
	void AbortFileWrite(FileHandle &handle) override {
		const auto path = handle.GetPath();
		handle.Close();
		LocalFileSystem::TryRemoveFile(path);
		aborted_writes++;
	}

	atomic<idx_t> versioned_opens {0};
	atomic<idx_t> aborted_writes {0};
	//! A removal of this path fails
	string failing_removal;

protected:
	bool SupportsOpenFileExtended() const override {
		return true;
	}
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override {
		if (file.extended_info && file.extended_info->options.count("etag")) {
			versioned_opens++;
		}
		return OpenFile(file.path, flags, opener);
	}

private:
	static string Local(const string &path) {
		return path.substr(strlen(PREFIX));
	}
};

//! Publishes [path] as a file the external file cache knows at version "v1"
void PublishVersion(ExternalFileCache &cache, const string &path, idx_t size) {
	ExternalFileCacheAppender appender(cache.CreateWrittenFile(path));
	WriteBlocks(appender, size);
	appender.Finish("v1", Timestamp::FromEpochSeconds(1700000000));
}

//! Writes the bytes [from, to) of the test pattern through the windows of [writer], committing a varying share of each
void WriteThrough(FileWriter &writer, idx_t from, idx_t to) {
	for (idx_t written = from; written < to;) {
		const auto window = writer.Window();
		const auto count = MinValue(window.size, MinValue(to - written, 1000 + written % 7000));
		for (idx_t i = 0; i < count; i++) {
			window.data[i] = ExpectedByte(written + i);
		}
		writer.Commit(count, 0);
		written += count;
	}
}

//! Whether the local file at [path] holds the first [size] bytes of the test pattern, and nothing else
bool HoldsUploadedBytes(const string &path, idx_t size) {
	LocalFileSystem local;
	auto handle = local.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	if (NumericCast<idx_t>(handle->GetFileSize()) != size) {
		return false;
	}
	vector<uint8_t> data(size);
	handle->Read(data.data(), size, 0);
	for (idx_t i = 0; i < size; i++) {
		if (data[i] != ExpectedByte(i)) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("The database filesystem reads remote files through the external file cache and forgets the ones it changes",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	auto owned = make_uniq<RemoteTestFileSystem>();
	auto &remote = *owned;
	fs.RegisterSubSystem(std::move(owned));
	auto &cache = ExternalFileCache::Get(*db.instance);
	TestFile file("database_fs_remote.bin", 2 * BLOCK + 10);
	const string path = RemoteTestFileSystem::PREFIX + file.path;
	ExternalFileCacheFileInfo info;

	auto fetched = fs.OpenBlockFile(path);
	REQUIRE(fetched->GetSize() == 2 * BLOCK + 10);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*fetched, 1), 1));
	REQUIRE(remote.versioned_opens == 0);

	PublishVersion(cache, path, 2 * BLOCK + 10);
	auto known = fs.OpenBlockFile(path);
	REQUIRE(remote.versioned_opens == 1);
	REQUIRE(cache.IsCached(*known, 0, 2 * BLOCK + 10));

	fs.OpenFile(path, FileFlags::FILE_FLAGS_READ)->Close();
	REQUIRE(cache.TryGetFile(path, info));
	fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE)->Close();
	REQUIRE_FALSE(cache.TryGetFile(path, info));

	const string moved = path + ".moved";
	PublishVersion(cache, path, BLOCK);
	PublishVersion(cache, moved, BLOCK);
	fs.MoveFile(path, moved);
	REQUIRE_FALSE(cache.TryGetFile(path, info));
	REQUIRE_FALSE(cache.TryGetFile(moved, info));

	PublishVersion(cache, moved, BLOCK);
	REQUIRE(fs.TryRemoveFile(moved));
	REQUIRE_FALSE(cache.TryGetFile(moved, info));
	PublishVersion(cache, moved, BLOCK);
	REQUIRE_FALSE(fs.TryRemoveFile(moved));
	REQUIRE(cache.TryGetFile(moved, info));

	TestFile removed("database_fs_removed.bin", BLOCK);
	TestFile batch("database_fs_batch.bin", BLOCK);
	const string removed_path = RemoteTestFileSystem::PREFIX + removed.path;
	const string batch_path = RemoteTestFileSystem::PREFIX + batch.path;
	PublishVersion(cache, removed_path, BLOCK);
	PublishVersion(cache, batch_path, BLOCK);
	fs.RemoveFile(removed_path);
	fs.RemoveFiles({batch_path});
	REQUIRE_FALSE(cache.TryGetFile(removed_path, info));
	REQUIRE_FALSE(cache.TryGetFile(batch_path, info));

	TestFile before_failure("database_fs_before_failure.bin", BLOCK);
	const string before_failure_path = RemoteTestFileSystem::PREFIX + before_failure.path;
	remote.failing_removal = before_failure_path + ".failing";
	PublishVersion(cache, before_failure_path, BLOCK);
	REQUIRE_THROWS(fs.RemoveFiles({before_failure_path, remote.failing_removal}));
	REQUIRE_FALSE(cache.TryGetFile(before_failure_path, info));
	remote.failing_removal.clear();

	REQUIRE_FALSE(fs.OpenBlockFile(file.path));
	const string memory = string(MemoryFileSystem::PREFIX) + "database_fs.bin";
	auto writer = fs.OpenFileWriter(memory);
	writer->Window().data[0] = 1;
	writer->Commit(1, 0);
	writer->Close();
	REQUIRE(fs.OpenBlockFile(memory));
}

TEST_CASE("The database filesystem forgets the cached files below a remote directory it removes",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	fs.RegisterSubSystem(make_uniq<RemoteTestFileSystem>());
	auto &cache = ExternalFileCache::Get(*db.instance);
	LocalFileSystem local;
	const auto directory = TestCreatePath("database_fs_directory");
	local.CreateDirectory(directory);
	const auto inside = local.JoinPath(directory, "inside.bin");
	local.OpenFile(inside, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW)->Close();
	const string remote_directory = RemoteTestFileSystem::PREFIX + directory;
	const string remote_inside = RemoteTestFileSystem::PREFIX + inside;
	const string remote_beside = remote_directory + "_beside.bin";
	PublishVersion(cache, remote_inside, BLOCK);
	PublishVersion(cache, remote_beside, BLOCK);

	fs.RemoveDirectory(remote_directory);
	REQUIRE_FALSE(local.DirectoryExists(directory));
	ExternalFileCacheFileInfo info;
	REQUIRE_FALSE(cache.TryGetFile(remote_inside, info));
	REQUIRE(cache.TryGetFile(remote_beside, info));
}

TEST_CASE("The database filesystem uploads a remote file from the cache blocks its writer fills",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	auto owned = make_uniq<RemoteTestFileSystem>();
	auto &remote = *owned;
	fs.RegisterSubSystem(std::move(owned));
	auto &cache = ExternalFileCache::Get(*db.instance);
	LocalFileSystem local;
	const auto local_path = TestCreatePath("database_fs_upload.bin");
	const string path = RemoteTestFileSystem::PREFIX + local_path;
	constexpr idx_t SIZE = 2 * BLOCK + 10;
	ExternalFileCacheFileInfo info;

	// the uploads the process has in flight count the file while it is written
	const auto idle = DatabaseFileSystem::GetPendingUploads();
	const auto pending_files = [&] {
		return DatabaseFileSystem::GetPendingUploads().files - idle.files;
	};
	const auto pending_bytes = [&] {
		return DatabaseFileSystem::GetPendingUploads().bytes - idle.bytes;
	};
	cache.SetEnabled(false);
	auto uncached = fs.OpenFileWriter(path);
	WriteThrough(*uncached, 0, SIZE);
	REQUIRE(pending_files() == 1);
	REQUIRE(pending_bytes() == SIZE);
	uncached->Close();
	REQUIRE(pending_files() == 0);
	REQUIRE(pending_bytes() == 0);
	REQUIRE(HoldsUploadedBytes(local_path, SIZE));
	REQUIRE_FALSE(cache.TryGetFile(path, info));
	cache.SetEnabled(true);
	auto writer = fs.OpenFileWriter(path);
	REQUIRE(writer);
	WriteThrough(*writer, 0, SIZE);
	REQUIRE(pending_files() == 1);
	REQUIRE(pending_bytes() == SIZE);
	writer->Close();
	REQUIRE(pending_files() == 0);
	REQUIRE(pending_bytes() == 0);
	REQUIRE(HoldsUploadedBytes(local_path, SIZE));
	REQUIRE(cache.TryGetFile(path, info));
	REQUIRE(info.validation.file_size == SIZE);
	REQUIRE(info.validation.version_tag == RemoteTestFileSystem::UPLOAD_TAG);
	auto written = fs.OpenBlockFile(path);
	REQUIRE(remote.versioned_opens == 1);
	REQUIRE(cache.IsCached(*written, 0, SIZE));
	REQUIRE(HoldsExpectedBytes(cache.Pin(*written, 2), 2));

	const string aborted = path + ".aborted";
	auto aborting = fs.OpenFileWriter(aborted);
	WriteThrough(*aborting, 0, BLOCK + 1);
	aborting->Abort();
	REQUIRE(remote.aborted_writes == 1);
	REQUIRE(pending_files() == 0);
	REQUIRE(pending_bytes() == 0);
	REQUIRE_FALSE(cache.TryGetFile(aborted, info));
	REQUIRE_FALSE(local.FileExists(local_path + ".aborted"));

	const string dropped = path + ".dropped";
	{
		auto dropping = fs.OpenFileWriter(dropped);
		WriteThrough(*dropping, 0, 10);
	}
	REQUIRE(remote.aborted_writes == 2);
	REQUIRE(pending_files() == 0);
	REQUIRE(pending_bytes() == 0);
	REQUIRE_FALSE(cache.TryGetFile(dropped, info));

	// a stream writes into the blocks of a kept file, and hands the large writes of any other to the upload
	vector<data_t> bytes(4 * BLOCK + 10);
	for (idx_t i = 0; i < bytes.size(); i++) {
		bytes[i] = ExpectedByte(i);
	}
	const auto crc = static_cast<uint32_t>(absl::ComputeCrc32c({const_char_ptr_cast(bytes.data()), bytes.size()}));
	for (const bool cached : {true, false}) {
		cache.SetEnabled(cached);
		BufferedFileWriter stream(fs, path, fs.OpenFileWriter(path));
		stream.WriteData(bytes.data(), 100);
		stream.WriteData(bytes.data() + 100, bytes.size() - 100);
		REQUIRE(stream.GetTotalWritten() == bytes.size());
		REQUIRE(stream.Checksum() == crc);
		REQUIRE(pending_bytes() == bytes.size());
		stream.Close();
		REQUIRE(HoldsUploadedBytes(local_path, bytes.size()));
		REQUIRE(cache.TryGetFile(path, info) == cached);
	}
	cache.SetEnabled(true);
	{
		BufferedFileWriter unclosed(fs, dropped, fs.OpenFileWriter(dropped));
		unclosed.WriteData(bytes.data(), bytes.size());
	}
	REQUIRE(remote.aborted_writes == 3);
	REQUIRE(pending_files() == 0);
	REQUIRE_FALSE(local.FileExists(local_path + ".dropped"));

	cache.SetEnabled(false);
	auto disabled = fs.OpenFileWriter(path);
	WriteThrough(*disabled, 0, BLOCK);
	disabled->Close();
	REQUIRE(HoldsUploadedBytes(local_path, BLOCK));
	REQUIRE_FALSE(cache.TryGetFile(path, info));
}

#ifdef SDB_FAULT_INJECTION
TEST_CASE("A remote file whose writer gets no cache block uploads every byte", "[external_file_cache]") {
	DuckDB db(nullptr);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	fs.RegisterSubSystem(make_uniq<RemoteTestFileSystem>());
	auto &cache = ExternalFileCache::Get(*db.instance);
	const auto local_path = TestCreatePath("database_fs_upload_uncached.bin");
	const string path = RemoteTestFileSystem::PREFIX + local_path;
	constexpr idx_t SIZE = 3 * BLOCK + 10;
	absl::Cleanup clear = [] {
		FaultPoints::Clear();
	};

	auto writer = fs.OpenFileWriter(path);
	WriteThrough(*writer, 0, BLOCK + 5);
	REQUIRE(FaultPoints::Arm("remote_write_cache_block"));
	WriteThrough(*writer, BLOCK + 5, SIZE);
	writer->Close();
	REQUIRE(HoldsUploadedBytes(local_path, SIZE));
	ExternalFileCacheFileInfo info;
	REQUIRE_FALSE(cache.TryGetFile(path, info));
}
#endif

TEST_CASE("External file cache forgets files no one opens and holds no block of", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile with_block("external_file_cache_sweep_cached.bin", 2 * BLOCK);
	TestFile kept_open("external_file_cache_sweep_open.bin", 2 * BLOCK);
	cache.Pin(*with_block.Open(cache, fs), 0);
	auto open = kept_open.Open(cache, fs);

	ExternalFileCacheFileInfo info;
	info.validation.file_size = 100;
	info.validation.version_tag = "v1";
	for (idx_t i = 0; i < 3000; i++) {
		cache.OpenFile("unused_" + to_string(i), info, true, nullptr);
	}
	ExternalFileCacheFileInfo found;
	REQUIRE_FALSE(cache.TryGetFile("unused_0", found));
	REQUIRE(cache.TryGetFile(with_block.path, found));
	REQUIRE(cache.TryGetFile(kept_open.path, found));
	REQUIRE(cache.GetCachedFileInformation().size() == 1);
}

TEST_CASE("Disabling the external file cache drops blocks of backed files and keeps in-memory ones",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_disable.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);
	auto held = cache.Pin(*cached, 0);
	cache.Pin(*cached, 1);
	auto memory = cache.CreateUnbackedFile("memory://disable.bin");
	AppendBlocks(memory, 1, [](idx_t) { return 7; });

	cache.SetEnabled(false);
	REQUIRE_FALSE(cache.IsEnabled());
	REQUIRE(HoldsExpectedBytes(held, 0));
	REQUIRE_FALSE(cache.IsCached(*cached, 0, BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, BLOCK, BLOCK));
	REQUIRE(cache.TryPinRange(*memory, 0, BLOCK)[0].Ptr()[BLOCK - 1] == 7);
	held.Reset();
	REQUIRE(cache.GetStatistics().used == BLOCK);
	cache.SetEnabled(true);
	REQUIRE(cache.IsEnabled());
}

TEST_CASE("External file cache never lets the buffer pool evict a pinned block", "[external_file_cache]") {
	DBConfig config;
	LimitMemory(config, 16, "");
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_pinned.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	vector<FilePin> pins;
	for (idx_t block = 0; block < 4; block++) {
		pins.push_back(cache.Pin(*cached, block));
	}
	for (idx_t block = 4; block < 64; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	for (idx_t block = 0; block < 4; block++) {
		REQUIRE(HoldsExpectedBytes(pins[block], block));
		REQUIRE(cache.IsCached(*cached, block * BLOCK, BLOCK));
	}
	REQUIRE(cache.GetStatistics().used <= 16 * BLOCK);
}

TEST_CASE("External file cache pins a fetched last block with only the bytes of its file", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile partial("external_file_cache_partial.bin", BLOCK + 1000);

	auto cached = partial.Open(cache, fs);
	auto single = cache.Pin(*cached, 1);
	REQUIRE(single.Size() == 1000);
	REQUIRE(HoldsExpectedBytes(single, 1));
	single.Reset();
	cached.reset();
	cache.ForgetFile(partial.path);

	cached = partial.Open(cache, fs);
	auto pins = cache.PinRange(*cached, 0, BLOCK + 1000);
	REQUIRE(pins.size() == 2);
	REQUIRE(pins[1].Size() == 1000);
	REQUIRE(HoldsExpectedBytes(pins[1], 1));
}

TEST_CASE("External file cache pins fail with the read error and later pins fetch again", "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_failure.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Fail(1);
	REQUIRE_THROWS_WITH(cache.Pin(*cached, 1), Catch::Contains("injected read failure"));
	auto pin = cache.Pin(*cached, 1);
	REQUIRE(HoldsExpectedBytes(pin, 1));

	fs.Fail(1);
	cache.Prefetch(*cached, 2 * BLOCK, BLOCK);
	auto prefetched = cache.Pin(*cached, 2);
	REQUIRE(HoldsExpectedBytes(prefetched, 2));
}

TEST_CASE("External file cache gives back the blocks of a fetch piece that never ran after another piece failed",
          "[external_file_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	constexpr idx_t PIECE_BLOCKS = PIECE_SIZE / BLOCK;
	TestFile file("external_file_cache_cancelled_piece.bin", 2 * PIECE_BLOCKS * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Fail(1);
	REQUIRE_THROWS_WITH(cache.PinRange(*cached, 0, 2 * PIECE_BLOCKS * BLOCK), Catch::Contains("injected read failure"));
	REQUIRE(!cache.IsCached(*cached, PIECE_BLOCKS * BLOCK, BLOCK));
	auto pin = cache.Pin(*cached, PIECE_BLOCKS);
	REQUIRE(HoldsExpectedBytes(pin, PIECE_BLOCKS));
}

TEST_CASE("External file cache keeps evicted blocks of remote files in temporary files and reads them back",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_remote_copies");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_remote_copies.bin", 32 * BLOCK);
	auto cached = file.Open(cache, fs);

	for (idx_t block = 0; block < 32; block++) {
		cache.Pin(*cached, block);
	}
	REQUIRE(fs.ReadCount() == 32);
	REQUIRE(cache.GetStatistics().spilled > 0);
	REQUIRE(cache.GetStatistics().evictions > 0);
	REQUIRE(CountFiles(directory, ".block") == 0);
	for (idx_t round = 0; round < 3; round++) {
		for (idx_t block = 0; block < 32; block++) {
			auto pin = cache.Pin(*cached, block);
			REQUIRE(HoldsExpectedBytes(pin, block));
		}
	}
	REQUIRE(fs.ReadCount() == 32);
	REQUIRE(cache.GetStatistics().spill_reads > 0);
	REQUIRE(cache.GetStatistics().spilled <= 32);

	cached.reset();
	cache.ForgetFile(file.path);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	REQUIRE(cache.GetStatistics().used == 0);
	REQUIRE(buffer_manager.GetUsedSwap() == 0);
}

TEST_CASE("External file cache keeps the end of a fetched file in a buffer of its own size and fetches it again once "
          "it left memory",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_tail");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_tail.bin", 32 * BLOCK + 1000);
	auto cached = file.Open(cache, fs);
	const auto tail_memory = BufferManager::GetAllocSize(1000);
	REQUIRE(tail_memory < BLOCK);

	{
		auto tail = cache.Pin(*cached, 32);
		REQUIRE(tail.Size() == 1000);
		REQUIRE(HoldsExpectedBytesAt(tail, 32 * BLOCK));
	}
	REQUIRE(cache.GetStatistics().used == tail_memory);
	REQUIRE(fs.ReadCount() == 1);

	cache.MarkUnused(*cached, 32 * BLOCK, 1000);
	for (idx_t round = 0; round < 2; round++) {
		for (idx_t block = 0; block < 32; block++) {
			auto pin = cache.Pin(*cached, block);
			REQUIRE(HoldsExpectedBytes(pin, block));
		}
	}
	REQUIRE(fs.ReadCount() == 33);
	REQUIRE(CountFiles(directory, ".block") == 0);
	auto tail = cache.Pin(*cached, 32);
	REQUIRE(tail.Size() == 1000);
	REQUIRE(HoldsExpectedBytesAt(tail, 32 * BLOCK));
	REQUIRE(fs.ReadCount() == 34);

	TestFile tiny("external_file_cache_tiny.bin", 47);
	auto tiny_cached = tiny.Open(cache, fs);
	const auto before = cache.GetStatistics().used;
	auto bytes = cache.Pin(*tiny_cached, 0);
	REQUIRE(bytes.Size() == 47);
	REQUIRE(HoldsExpectedBytesAt(bytes, 0));
	REQUIRE(cache.GetStatistics().used <= before + BufferManager::GetAllocSize(47));
}

TEST_CASE("External file cache drops evicted blocks of remote files when external_file_cache_spill_size is 0",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_no_copies");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_spill_size = 0"));
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_no_copies.bin", 32 * BLOCK);
	auto cached = file.Open(cache, fs);
	for (idx_t round = 0; round < 2; round++) {
		for (idx_t block = 0; block < 32; block++) {
			REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, block), block));
		}
	}
	REQUIRE(fs.ReadCount() > 32);
	REQUIRE(cache.GetStatistics().spilled == 0);
	REQUIRE(BufferManager::GetBufferManager(*db.instance).GetUsedSwap() == 0);
}

TEST_CASE("External file cache drops evicted blocks of remote files when external_file_cache_spill is off",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_spill_off");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_spill = false"));
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_spill_off.bin", 32 * BLOCK);
	auto cached = file.Open(cache, fs);
	for (idx_t round = 0; round < 2; round++) {
		for (idx_t block = 0; block < 32; block++) {
			REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, block), block));
		}
	}
	REQUIRE(fs.ReadCount() > 32);
	REQUIRE(cache.GetStatistics().spilled == 0);
	REQUIRE(BufferManager::GetBufferManager(*db.instance).GetUsedSwap() == 0);
}

TEST_CASE("External file cache fetches evicted blocks again when the temporary files have no room or the file is local",
          "[external_file_cache]") {
	for (const bool local : {false, true}) {
		const auto directory = TestCreatePath("external_file_cache_dropped");
		DBConfig config;
		LimitMemory(config, 8, directory);
		DuckDB db(nullptr, &config);
		if (!local) {
			BufferManager::GetBufferManager(*db.instance).SetSwapLimit(2 * BLOCK);
		}
		CountingSourceFileSystem fs;
		ExternalFileCache cache(*db.instance);
		TestFile file("external_file_cache_dropped.bin", 32 * BLOCK);
		auto cached = file.Open(cache, fs, "v1", local);

		for (idx_t round = 0; round < 2; round++) {
			for (idx_t block = 0; block < 32; block++) {
				auto pin = cache.Pin(*cached, block);
				REQUIRE(HoldsExpectedBytes(pin, block));
			}
		}
		REQUIRE(fs.ReadCount() > 32);
		REQUIRE(BufferManager::GetBufferManager(*db.instance).GetUsedSwap() <= BLOCK);
		REQUIRE(cache.GetStatistics().used <= 8 * BLOCK);
	}
}

TEST_CASE("External file cache blocks of in-memory files spill to temporary files and are never dropped",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_unbacked_spill");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	ExternalFileCache cache(*db.instance);
	auto file = cache.CreateUnbackedFile("memory://unbacked.bin");
	AppendBlocks(file, 32, [](idx_t block) { return block + 1; });
	REQUIRE(cache.GetStatistics().spilled > 0);
	REQUIRE(CountFiles(directory, ".block") == 0);
	const auto fetches = cache.GetStatistics().fetches;
	REQUIRE(file->ReadAhead(0, 32 * BLOCK, BLOCK) == 32 * BLOCK);
	REQUIRE(cache.GetStatistics().fetches == fetches);
	for (idx_t round = 0; round < 2; round++) {
		for (idx_t block = 0; block < 32; block++) {
			auto pin = cache.Pin(*file, block);
			REQUIRE(pin.Ptr()[0] == block + 1);
			REQUIRE(pin.Ptr()[BLOCK - 1] == block + 1);
		}
	}
	file.reset();
	REQUIRE(cache.GetStatistics().used == 0);
	REQUIRE(BufferManager::GetBufferManager(*db.instance).GetUsedSwap() == 0);
}

TEST_CASE("External file cache keeps external_file_cache_min_memory of remote blocks while operator blocks can spill",
          "[external_file_cache]") {
	DBConfig config;
	LimitMemory(config, 64, TestCreatePath("external_file_cache_min_memory"));
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_min_memory = '256KiB'"));
	auto setting = con.Query("SELECT current_setting('external_file_cache_min_memory')");
	REQUIRE(CHECK_COLUMN(setting, 0, {"256.0 KiB"}));
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile remote_file("external_file_cache_min_memory_remote.bin", 6 * BLOCK);
	TestFile local_file("external_file_cache_min_memory_local.bin", BLOCK);
	auto remote = remote_file.Open(cache, fs);
	auto local = local_file.Open(cache, fs, "v1", true);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	auto &pool = db.instance->GetBufferPool();
	buffer_manager.SetMemoryLimit(pool.GetUsedMemory() + 8 * BLOCK);
	const auto loaded = [&](const shared_ptr<InputFile> &file, idx_t blocks) {
		idx_t result = 0;
		for (idx_t block = 0; block < blocks; block++) {
			result += !cache.TryPinRange(*file, block * BLOCK, BLOCK).empty();
		}
		return result;
	};
	const auto allocate = [&] {
		return buffer_manager.Allocate(MemoryTag::HASH_TABLE, BLOCK - Storage::DEFAULT_BLOCK_HEADER_SIZE, false);
	};

	for (idx_t block = 0; block < 6; block++) {
		cache.Pin(*remote, block);
	}
	cache.Pin(*local, 0);
	const auto spillable = allocate().GetBlockHandle();
	vector<BufferHandle> pinned;
	pinned.push_back(allocate());
	REQUIRE(loaded(local, 1) == 0);
	REQUIRE(loaded(remote, 6) == 6);

	for (idx_t i = 0; i < 4; i++) {
		pinned.push_back(allocate());
	}
	REQUIRE(loaded(remote, 6) == 2);
	REQUIRE(!spillable->GetMemory().IsUnloaded());

	pinned.push_back(allocate());
	REQUIRE(spillable->GetMemory().IsUnloaded());
	REQUIRE(loaded(remote, 6) == 2);

	pinned.push_back(allocate());
	REQUIRE(loaded(remote, 6) == 1);

	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_min_memory = 0"));
	REQUIRE(pool.GetExternalFileCacheMinMemory() == 0);
	REQUIRE_NO_FAIL(con.Query("RESET external_file_cache_min_memory"));
	REQUIRE(pool.GetExternalFileCacheMinMemory() == pool.GetMaxMemory() / 20);
}

#ifdef SDB_FAULT_INJECTION
namespace {

void AwaitParked(const string &fault_point) {
	while (!FaultPoints::Armed(fault_point + ":parked")) {
		absl::SleepFor(absl::Milliseconds(1));
	}
}

struct ReadWhileParked {
	//! Whether the read waited for the parked thread instead of returning while it was parked
	bool waited;
	uint8_t first_byte;
};

//! Reads block [block] of [file] in a thread of its own while another thread is parked at [fault_point], then releases
//! the parked thread
ReadWhileParked ReadWhileParkedAt(ExternalFileCache &cache, const shared_ptr<InputFile> &file, idx_t block,
                                  const string &fault_point) {
	auto reader =
	    std::async(std::launch::async, [&] { return cache.PinRange(*file, block * BLOCK, BLOCK)[0].Ptr()[0]; });
	ReadWhileParked result;
	result.waited = reader.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
	REQUIRE(FaultPoints::Arm(fault_point + ":release"));
	result.first_byte = 0;
	REQUIRE_NOTHROW(result.first_byte = reader.get());
	return result;
}

} // namespace

TEST_CASE("External file cache reads an in-memory block while an eviction writes it to a temporary file",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_unload_window");
	DBConfig config;
	LimitMemory(config, 4, directory);
	DuckDB db(nullptr, &config);
	ExternalFileCache cache(*db.instance);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	auto file = cache.CreateUnbackedFile("memory://unload_window.bin");
	AppendBlocks(file, 2, [](idx_t block) { return block + 1; });
	absl::Cleanup clear = [] {
		FaultPoints::Clear();
	};
	REQUIRE(FaultPoints::Arm("block_unload_before_temporary_write"));
	auto evictor =
	    std::async(std::launch::async, [&] { return buffer_manager.Allocate(MemoryTag::IN_MEMORY_TABLE, 2 * BLOCK); });
	AwaitParked("block_unload_before_temporary_write");
	const idx_t victim = cache.TryPinRange(*file, 0, BLOCK).empty() ? 0 : 1;
	REQUIRE(cache.TryPinRange(*file, victim * BLOCK, BLOCK).empty());
	const auto read = ReadWhileParkedAt(cache, file, victim, "block_unload_before_temporary_write");
	BufferHandle allocated;
	REQUIRE_NOTHROW(allocated = evictor.get());
	REQUIRE(allocated.IsValid());
	REQUIRE(read.waited);
	REQUIRE(read.first_byte == victim + 1);
}

TEST_CASE("External file cache reads an in-memory block while another reader loads it back from its temporary file",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_load_window");
	DBConfig config;
	LimitMemory(config, 16, directory);
	DuckDB db(nullptr, &config);
	ExternalFileCache cache(*db.instance);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	auto file = cache.CreateUnbackedFile("memory://load_window.bin");
	AppendBlocks(file, 2, [](idx_t block) { return block + 1; });
	buffer_manager.SetMemoryLimit(BLOCK);
	cache.Pin(*file, 1);
	REQUIRE(cache.TryPinRange(*file, 0, BLOCK).empty());
	absl::Cleanup clear = [] {
		FaultPoints::Clear();
	};
	REQUIRE(FaultPoints::Arm("block_load_before_loaded"));
	auto loader = std::async(std::launch::async, [&] { return cache.Pin(*file, 0).Ptr()[0]; });
	AwaitParked("block_load_before_loaded");
	const auto read = ReadWhileParkedAt(cache, file, 0, "block_load_before_loaded");
	uint8_t loaded = 0;
	REQUIRE_NOTHROW(loaded = loader.get());
	REQUIRE(loaded == 1);
	REQUIRE(read.waited);
	REQUIRE(read.first_byte == 1);
}
#endif

TEST_CASE("External file cache gives memory back to the buffer pool", "[external_file_cache]") {
	CountingSourceFileSystem fs;
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='64MiB'"));
	auto &cache = ExternalFileCache::Get(*db.instance);
	constexpr idx_t FULL = BlockCountOf(16 * 1024 * 1024);
	TestFile file("external_file_cache_shrink.bin", FULL * BLOCK);
	auto cached = file.Open(cache, fs);
	for (idx_t block = 0; block < FULL; block++) {
		cache.Pin(*cached, block);
	}
	const auto filled = cache.GetStatistics().used;
	REQUIRE(filled == FULL * BLOCK);

	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	vector<BufferHandle> buffers;
	for (idx_t i = 0; i < 14; i++) {
		buffers.push_back(buffer_manager.Allocate(MemoryTag::IN_MEMORY_TABLE, 4ULL * 1024ULL * 1024ULL));
	}
	REQUIRE(cache.GetStatistics().used < filled);
	buffers.clear();
	for (idx_t block = 0; block < FULL; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
}

TEST_CASE("External file cache gives the memory of a dropped in-memory file back at once", "[external_file_cache]") {
	DuckDB db(nullptr);
	ExternalFileCache cache(*db.instance);
	for (idx_t round = 0; round < 2; round++) {
		auto file = cache.CreateUnbackedFile("memory://dropped.bin");
		AppendBlocks(file, 32, [](idx_t block) { return block + 1; });
		REQUIRE(cache.GetStatistics().used == 32 * BLOCK);
		file.reset();
		REQUIRE(cache.GetStatistics().used == 0);
	}
}

TEST_CASE("External file cache concurrent pins and evictions return the right bytes", "[external_file_cache]") {
	constexpr idx_t BLOCKS = 1024;
	for (const bool copies : {false, true}) {
		DBConfig config;
		LimitMemory(config, 256, copies ? TestCreatePath("external_file_cache_stress") : "");
		DuckDB db(nullptr, &config);
		CountingSourceFileSystem fs;
		ExternalFileCache cache(*db.instance);
		TestFile file("external_file_cache_stress.bin", BLOCKS * BLOCK);
		auto cached = file.Open(cache, fs);

		atomic<idx_t> wrong {0};
		TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
		for (idx_t t = 0; t < 16; t++) {
			executor.ScheduleTask(
			    [&, t]() {
				    uint64_t seed = t * 7919 + 1;
				    try {
					    for (idx_t i = 0; i < 400; i++) {
						    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
						    const auto block = (seed >> 33) % BLOCKS;
						    if (i % 3 == 0) {
							    auto pins = cache.TryPinRange(*cached, block * BLOCK, BLOCK);
							    if (!pins.empty() && !HoldsExpectedBytes(pins[0], block)) {
								    wrong++;
							    }
							    continue;
						    }
						    if (i % 7 == 0) {
							    cache.Prefetch(*cached, block * BLOCK, 4 * BLOCK);
						    }
						    if (i % 11 == 0) {
							    auto pins = cache.PinRange(*cached, block * BLOCK, 3 * BLOCK);
							    for (idx_t j = 0; j < pins.size(); j++) {
								    wrong += !HoldsExpectedBytes(pins[j], block + j);
							    }
							    continue;
						    }
						    auto pin = cache.Pin(*cached, block);
						    if (!HoldsExpectedBytes(pin, block)) {
							    wrong++;
						    }
					    }
				    } catch (std::exception &) {
					    wrong++;
				    }
			    },
			    "ExternalFileCacheTestTask");
		}
		executor.WorkOnTasks();
		REQUIRE(wrong == 0);
	}
}

TEST_CASE("External file cache serves concurrent pins of one block from memory and lets the block go after them",
          "[external_file_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile hot("external_file_cache_hot.bin", BLOCK);
	auto file = hot.Open(cache, fs);
	cache.Pin(*file, 0);
	const auto before = cache.GetStatistics();
	constexpr idx_t TASKS = 8;
	constexpr idx_t PINS = 10000;
	atomic<idx_t> wrong {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t task = 0; task < TASKS; task++) {
		executor.ScheduleTask(
		    [&]() {
			    for (idx_t i = 0; i < PINS; i++) {
				    auto pin = cache.Pin(*file, 0);
				    if (pin.Ptr()[0] != ExpectedByte(0) || pin.Ptr()[BLOCK - 1] != ExpectedByte(BLOCK - 1)) {
					    wrong++;
				    }
			    }
		    },
		    "ExternalFileCacheTestTask");
	}
	executor.WorkOnTasks();
	REQUIRE(wrong == 0);
	const auto after = cache.GetStatistics();
	REQUIRE(after.misses == before.misses);
	file.reset();
	cache.ForgetFile(hot.path);
	REQUIRE(cache.GetStatistics().used == 0);
}

TEST_CASE("External file cache pins of an in-memory file return the right bytes while it grows and spills",
          "[external_file_cache]") {
	DBConfig config;
	LimitMemory(config, 16, TestCreatePath("external_file_cache_growing_spill"));
	DuckDB db(nullptr, &config);
	ExternalFileCache cache(*db.instance);
	auto file = cache.CreateUnbackedFile("memory://growing.bin");
	constexpr idx_t BLOCKS = 256;
	constexpr idx_t RECENT = 6;
	const auto fill = [](idx_t block) {
		return static_cast<uint8_t>(block % 251 + 1);
	};
	atomic<idx_t> created {0};
	ExternalFileCacheAppender appender(file);
	vector<data_t> bytes(BLOCK);
	const auto create = [&](idx_t block) {
		memset(bytes.data(), fill(block), BLOCK);
		appender.Append(bytes.data(), BLOCK);
		created.store(block + 1, std::memory_order_release);
	};
	for (idx_t block = 0; block < RECENT; block++) {
		create(block);
	}
	atomic<bool> done {false};
	atomic<idx_t> wrong {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t task = 0; task < 3; task++) {
		executor.ScheduleTask(
		    [&, task]() {
			    uint64_t seed = task * 7919 + 1;
			    while (!done.load(std::memory_order_acquire)) {
				    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
				    const auto block = created.load(std::memory_order_acquire) - 1 - (seed >> 33) % RECENT;
				    auto pin = cache.Pin(*file, block);
				    if (pin.Ptr()[0] != fill(block) || pin.Ptr()[BLOCK - 1] != fill(block)) {
					    wrong++;
				    }
			    }
		    },
		    "ExternalFileCacheTestTask");
	}
	for (idx_t block = RECENT; block < BLOCKS; block++) {
		create(block);
	}
	done.store(true, std::memory_order_release);
	executor.WorkOnTasks();
	REQUIRE(wrong == 0);
	for (idx_t block = 0; block < BLOCKS; block++) {
		auto pin = cache.Pin(*file, block);
		REQUIRE(pin.Ptr()[0] == fill(block));
		REQUIRE(pin.Ptr()[BLOCK - 1] == fill(block));
	}
}

TEST_CASE("Instances sharing a temporary directory spill in-memory blocks to files of their own",
          "[external_file_cache]") {
	const auto directory = TestCreatePath("external_file_cache_shared_spill");
	constexpr idx_t FIT = 8;
	{
		DBConfig first_config;
		LimitMemory(first_config, FIT, directory);
		DBConfig second_config;
		LimitMemory(second_config, FIT, directory);
		DuckDB first(nullptr, &first_config);
		DuckDB second(nullptr, &second_config);
		vector<shared_ptr<InputFile>> files;
		for (auto db : {&first, &second}) {
			auto &cache = ExternalFileCache::Get(*db->instance);
			auto file = cache.CreateUnbackedFile("memory://shared_spill.bin");
			const auto fill = files.size() * 100 + 1;
			AppendBlocks(file, 4 * FIT, [&](idx_t block) { return fill + block; });
			REQUIRE(BufferManager::GetBufferManager(*db->instance).GetUsedSwap() > 0);
			files.push_back(std::move(file));
		}
		REQUIRE(CountFiles(directory, ".tmp") >= 2);
		for (idx_t i = 0; i < files.size(); i++) {
			auto &cache = ExternalFileCache::Get(*(i == 0 ? first : second).instance);
			for (idx_t block = 0; block < 4 * FIT; block++) {
				auto pin = cache.Pin(*files[i], block);
				REQUIRE(pin.Ptr()[0] == uint8_t(i * 100 + 1 + block));
				REQUIRE(pin.Ptr()[BLOCK - 1] == uint8_t(i * 100 + 1 + block));
			}
		}
	}
	REQUIRE(CountFiles(directory, ".tmp") == 0);
}

namespace {

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

uint8_t BenchByte(idx_t block) {
	return static_cast<uint8_t>(block % 251 + 1);
}

} // namespace

TEST_CASE("External file cache benchmark: concurrent reads of blocks evicted to temporary files",
          "[.][external_file_cache_bench]") {
	constexpr idx_t MEMORY_BLOCKS = 64;
	constexpr idx_t BLOCKS = 1024;
	constexpr idx_t READS = 256;
	vector<pair<string, string>> directories {
	    {"the test directory", TestCreatePath("external_file_cache_bench_spill")}};
	LocalFileSystem local;
	if (local.DirectoryExists("/dev/shm")) {
		directories.emplace_back("tmpfs", "/dev/shm/duckdb_external_file_cache_bench_spill_" + to_string(getpid()));
	}
	for (auto &directory : directories) {
		DBConfig config;
		LimitMemory(config, MEMORY_BLOCKS, directory.second);
		DuckDB db(nullptr, &config);
		ExternalFileCache cache(*db.instance);
		auto file = cache.CreateUnbackedFile("memory://bench_spill.bin");
		AppendBlocks(file, BLOCKS, BenchByte);
		for (idx_t block = 0; block < BLOCKS; block++) {
			cache.Pin(*file, block);
		}
		for (const idx_t readers : {idx_t(1), idx_t(8)}) {
			for (idx_t round = 0; round < 3; round++) {
				atomic<idx_t> wrong {0};
				const auto spill_reads = cache.GetStatistics().spill_reads;
				const auto start = std::chrono::steady_clock::now();
				TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
				for (idx_t reader = 0; reader < readers; reader++) {
					executor.ScheduleTask(
					    [&, reader]() {
						    uint64_t seed = (round * 8 + reader) * 7919 + 1;
						    for (idx_t i = 0; i < READS; i++) {
							    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
							    const auto block = (seed >> 33) % BLOCKS;
							    if (cache.Pin(*file, block).Ptr()[BLOCK - 1] != BenchByte(block)) {
								    wrong++;
							    }
						    }
					    },
					    "ExternalFileCacheBenchTask");
				}
				executor.WorkOnTasks();
				const auto elapsed = MillisecondsSince(start);
				REQUIRE(wrong == 0);
				const idx_t read = cache.GetStatistics().spill_reads - spill_reads;
				Printer::PrintF("temporary file reads (%s), %llu concurrent readers, round %llu: %llu blocks in %.1f "
				                "ms, %.0f MiB/s",
				                directory.first, readers, round, read, elapsed,
				                double(read * BLOCK) / (1024.0 * 1024.0) / (elapsed / 1000.0));
			}
		}
	}
}

TEST_CASE("External file cache benchmark: releasing the blocks of a 512 MiB in-memory file",
          "[.][external_file_cache_bench]") {
	constexpr idx_t BLOCKS = 4096;
	DuckDB db(nullptr);
	ExternalFileCache cache(*db.instance);
	for (idx_t round = 0; round < 5; round++) {
		auto file = cache.CreateUnbackedFile("memory://bench_release.bin");
		AppendBlocks(file, BLOCKS, BenchByte);
		REQUIRE(cache.GetStatistics().used == BLOCKS * BLOCK);
		const auto start = std::chrono::steady_clock::now();
		file.reset();
		const auto elapsed = MillisecondsSince(start);
		REQUIRE(cache.GetStatistics().used == 0);
		Printer::PrintF("releasing the blocks of a 512 MiB in-memory file, round %llu: %.2f ms", round, elapsed);
	}
}

TEST_CASE("External file cache benchmark: pins that evict while the buffer pool is full",
          "[.][external_file_cache_bench]") {
	constexpr idx_t BLOCKS = 1024;
	constexpr idx_t RESIDENT = 64;
	constexpr idx_t HEADROOM = 2ULL * 1024ULL * 1024ULL;
	CountingSourceFileSystem fs;
	DBConfig config;
	config.options.maximum_memory = 256ULL * 1024ULL * 1024ULL;
	DuckDB db(nullptr, &config);
	auto &cache = ExternalFileCache::Get(*db.instance);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	TestFile file("external_file_cache_bench_source.bin", BLOCKS * BLOCK);
	for (idx_t round = 0; round < 3; round++) {
		auto cached = file.Open(cache, fs, "v" + to_string(round));
		for (idx_t block = 0; block < RESIDENT; block++) {
			cache.Pin(*cached, block);
		}
		const auto used = buffer_manager.GetUsedMemory();
		const auto limit = buffer_manager.GetMaxMemory();
		const idx_t reserved = limit > used + HEADROOM ? limit - used - HEADROOM : 0;
		buffer_manager.ReserveMemory(reserved);
		const auto misses = cache.GetStatistics().misses;
		idx_t wrong = 0;
		const auto start = std::chrono::steady_clock::now();
		for (idx_t block = RESIDENT; block < BLOCKS; block++) {
			if (cache.Pin(*cached, block).Ptr()[0] != ExpectedByte(block * BLOCK)) {
				wrong++;
			}
		}
		const auto elapsed = MillisecondsSince(start);
		const idx_t missed = cache.GetStatistics().misses - misses;
		buffer_manager.FreeReservedMemory(reserved);
		REQUIRE(wrong == 0);
		Printer::PrintF("pins that evict with the buffer pool full, round %llu: %.1f us per block, %llu misses", round,
		                elapsed * 1000.0 / double(BLOCKS - RESIDENT), missed);
	}
}

TEST_CASE("External file cache benchmark: concurrent pins of one block and of distinct blocks",
          "[.][external_file_cache_bench]") {
	constexpr idx_t PINS = 500000;
	constexpr idx_t BLOCKS = 64;
	TestFile remote_file("external_file_cache_bench_pins.bin", BLOCKS * BLOCK);
	CountingSourceFileSystem fs;
	for (const idx_t threads : {idx_t(1), idx_t(8), idx_t(16), idx_t(32), idx_t(64)}) {
		DBConfig config;
		config.options.maximum_threads = threads;
		DuckDB db(nullptr, &config);
		ExternalFileCache cache(*db.instance);
		auto memory_file = cache.CreateUnbackedFile("memory://bench_pins.bin");
		AppendBlocks(memory_file, BLOCKS, BenchByte);
		auto remote = remote_file.Open(cache, fs);
		cache.PinRange(*remote, 0, BLOCKS * BLOCK);
		for (const bool one_block : {true, false}) {
			for (const bool in_memory : {true, false}) {
				const auto &file = in_memory ? memory_file : remote;
				atomic<idx_t> wrong {0};
				const auto start = std::chrono::steady_clock::now();
				TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
				for (idx_t thread = 0; thread < threads; thread++) {
					executor.ScheduleTask(
					    [&, thread]() {
						    const auto block = one_block ? 0 : thread % BLOCKS;
						    const auto expected = in_memory ? BenchByte(block) : ExpectedByte(block * BLOCK);
						    idx_t mismatches = 0;
						    for (idx_t i = 0; i < PINS; i++) {
							    mismatches += cache.Pin(*file, block).Ptr()[0] != expected;
						    }
						    wrong += mismatches;
					    },
					    "ExternalFileCacheBenchTask");
				}
				executor.WorkOnTasks();
				const auto elapsed = MillisecondsSince(start);
				REQUIRE(wrong == 0);
				Printer::PrintF("pins of %s of %s file, %llu threads: %.1f M pins/s, %.1f ns per pin and unpin in a "
				                "thread",
				                one_block ? "one block" : "distinct blocks", in_memory ? "an in-memory" : "a remote",
				                threads, double(threads * PINS) / elapsed / 1000.0, elapsed * 1000000.0 / double(PINS));
			}
		}
	}
}

TEST_CASE("External file cache benchmark: concurrent buffer manager pins of one buffer and of distinct buffers",
          "[.][external_file_cache_bench]") {
	constexpr idx_t PINS = 500000;
	constexpr idx_t BLOCKS = 64;
	for (const idx_t threads : {idx_t(1), idx_t(8), idx_t(16), idx_t(32), idx_t(64)}) {
		DBConfig config;
		config.options.maximum_threads = threads;
		DuckDB db(nullptr, &config);
		auto &buffers = BufferManager::GetBufferManager(*db.instance);
		vector<shared_ptr<BlockHandle>> handles;
		for (idx_t block = 0; block < BLOCKS; block++) {
			auto buffer = buffers.Allocate(MemoryTag::IN_MEMORY_TABLE, BLOCK, false);
			memset(buffer.GetDataMutable(), BenchByte(block), BLOCK);
			handles.push_back(buffer.GetBlockHandle());
		}
		for (const bool one_block : {true, false}) {
			atomic<idx_t> wrong {0};
			const auto start = std::chrono::steady_clock::now();
			TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
			for (idx_t thread = 0; thread < threads; thread++) {
				executor.ScheduleTask(
				    [&, thread]() {
					    const auto block = one_block ? 0 : thread % BLOCKS;
					    auto handle = handles[block];
					    idx_t mismatches = 0;
					    for (idx_t i = 0; i < PINS; i++) {
						    mismatches += buffers.Pin(handle).Ptr()[0] != BenchByte(block);
					    }
					    wrong += mismatches;
				    },
				    "BufferManagerBenchTask");
			}
			executor.WorkOnTasks();
			const auto elapsed = MillisecondsSince(start);
			REQUIRE(wrong == 0);
			Printer::PrintF("buffer manager pins of %s, %llu threads: %.1f M pins/s, %.1f ns per pin and unpin in a "
			                "thread",
			                one_block ? "one buffer" : "distinct buffers", threads,
			                double(threads * PINS) / elapsed / 1000.0, elapsed * 1000000.0 / double(PINS));
		}
	}
}

TEST_CASE("External file cache benchmark: concurrent buffer manager allocations and frees",
          "[.][external_file_cache_bench]") {
	constexpr idx_t ALLOCATIONS = 20000;
	for (const idx_t threads : {idx_t(1), idx_t(8), idx_t(32), idx_t(64)}) {
		DBConfig config;
		config.options.maximum_threads = threads;
		DuckDB db(nullptr, &config);
		auto &buffers = BufferManager::GetBufferManager(*db.instance);
		const auto start = std::chrono::steady_clock::now();
		TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
		for (idx_t thread = 0; thread < threads; thread++) {
			executor.ScheduleTask(
			    [&]() {
				    for (idx_t i = 0; i < ALLOCATIONS; i++) {
					    auto buffer = buffers.Allocate(MemoryTag::HASH_TABLE, BLOCK, true);
					    buffer.GetDataMutable()[0] = 1;
				    }
			    },
			    "BufferManagerAllocateBenchTask");
		}
		executor.WorkOnTasks();
		const auto elapsed = MillisecondsSince(start);
		Printer::PrintF("buffer manager allocations, %llu threads: %.2f M allocations/s, %.1f ns per allocation in a "
		                "thread",
		                threads, double(threads * ALLOCATIONS) / elapsed / 1000.0,
		                elapsed * 1000000.0 / double(ALLOCATIONS));
	}
}

TEST_CASE("External file cache benchmark: cold reads of a 16 MiB range from a slow source",
          "[.][external_file_cache_bench]") {
	constexpr idx_t BLOCKS = 64;
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	ExternalFileCache cache(*db.instance);
	TestFile file("external_file_cache_bench_slow.bin", BLOCKS * BLOCK);
	fs.Throttle(absl::Milliseconds(20), 50.0 * 1024.0 * 1024.0);
	const auto holds_range = [](const vector<FilePin> &pins) {
		for (idx_t block = 0; block < pins.size(); block++) {
			if (pins[block].Ptr()[0] != ExpectedByte(block * BLOCK)) {
				return false;
			}
		}
		return pins.size() == BLOCKS;
	};
	for (idx_t round = 0; round < 3; round++) {
		{
			auto cached = file.Open(cache, fs, "prefetched" + to_string(round));
			const auto start = std::chrono::steady_clock::now();
			cache.Prefetch(*cached, 0, BLOCKS * BLOCK);
			auto first = cache.Pin(*cached, 0);
			const auto first_ms = MillisecondsSince(start);
			auto pins = cache.PinRange(*cached, 0, BLOCKS * BLOCK);
			const auto all_ms = MillisecondsSince(start);
			REQUIRE(HoldsExpectedBytes(first, 0));
			REQUIRE(holds_range(pins));
			Printer::PrintF(
			    "16 MiB prefetched from a source of 20 ms and 50 MiB/s per request, round %llu: first block "
			    "in %.1f ms, every block in %.1f ms, %llu requests",
			    round, first_ms, all_ms, idx_t(fs.TakeReads().size()));
		}
		{
			auto cached = file.Open(cache, fs, "pinned" + to_string(round));
			const auto start = std::chrono::steady_clock::now();
			auto pins = cache.PinRange(*cached, 0, BLOCKS * BLOCK);
			const auto all_ms = MillisecondsSince(start);
			REQUIRE(holds_range(pins));
			Printer::PrintF("16 MiB range pinned from a source of 20 ms and 50 MiB/s per request, round %llu: every "
			                "block in %.1f ms, %llu requests",
			                round, all_ms, idx_t(fs.TakeReads().size()));
		}
	}
}

} // namespace duckdb
