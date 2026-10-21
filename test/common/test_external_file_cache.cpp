#include "catch.hpp"
#include "caching_test_utils.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"
#include "duckdb/storage/external_file_cache/cache_validation.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"

#include <chrono>

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

idx_t CountCachedBlocks(BlockCache &cache) {
	return cache.GetBlockInformation().size();
}

idx_t TotalCachedBytes(BlockCache &cache) {
	idx_t total = 0;
	for (auto &info : cache.GetBlockInformation()) {
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

TEST_CASE("A file no larger than the block size is fetched whole on the first read", "[external_file_cache]") {
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto recording_fs = make_uniq<ReadRecordingFileSystem>();

	const idx_t FILE_SIZE = 3000;
	auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_small_file.bin", content);

	CachingFileSystem cfs(*recording_fs, db_instance);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	auto &cache = db_instance.GetBlockCache();

	REQUIRE(ReadFull(*handle, 100, 2800) == content.substr(2800, 100));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{0, FILE_SIZE}});
	REQUIRE(CountCachedBlocks(cache) == 1);
	REQUIRE(TotalCachedBytes(cache) == FILE_SIZE);

	// Any later read of the file is served from the cache
	REQUIRE(ReadFull(*handle, 500, 100) == content.substr(100, 500));
	REQUIRE(recording_fs->TakeReads().empty());
}

TEST_CASE("Reads fetch the cache blocks around them", "[external_file_cache]") {
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto recording_fs = make_uniq<ReadRecordingFileSystem>();
	auto &cache = db_instance.GetBlockCache();

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto recording_fs = make_uniq<ReadRecordingFileSystem>();
	auto &cache = db_instance.GetBlockCache();

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
	const idx_t FILE_SIZE = 2 * BLOCK_SIZE + 50;
	auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_past_end.bin", content);

	CachingFileSystem cfs(*recording_fs, db_instance);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);

	auto group = handle->Read(100, FILE_SIZE - 10);
	REQUIRE(group.GetHandles().size() == 1);
	REQUIRE(group.GetHandles()[0].length == 10);
	string tail(10, '\0');
	group.CopyTo(reinterpret_cast<data_ptr_t>(&tail[0]), 10);
	REQUIRE(tail == content.substr(FILE_SIZE - 10));
	REQUIRE(recording_fs->TakeReads() == vector<pair<idx_t, idx_t>> {{2 * BLOCK_SIZE, 50}});

	REQUIRE(handle->Read(10, FILE_SIZE).GetHandles().empty());
	REQUIRE(recording_fs->TakeReads().empty());
	REQUIRE(CountCachedBlocks(cache) == 1);
}

TEST_CASE("Disabled external file cache does not cache blocks", "[external_file_cache]") {
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetBlockCache();
	Connection con(db);

	const idx_t FILE_SIZE = 16384;
	const auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_disabled.bin", content);

	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();
	CachingFileSystem cfs(*tracking_fs, db_instance);

	// Disable the cache.
	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=false"));
	REQUIRE(cache.GetBlockInformation().empty());

	// Open and fully read the file with caching disabled.
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, FILE_SIZE) == content);
	}

	// With caching disabled, no block should be cached.
	REQUIRE(cache.GetBlockInformation().empty());

	// When cache enabled, opening and reading the file does cache its block.
	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=true"));
	{
		auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
		REQUIRE(ReadFull(*handle, FILE_SIZE) == content);
	}
	REQUIRE(CountCachedBlocks(cache) == 1);
}

TEST_CASE("Sequential read preserves its position when the cache is disabled", "[external_file_cache]") {
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetBlockCache();
	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();

	const idx_t block_size = BlockCache::BLOCK_SIZE;
	const string first_block(block_size, 'A');
	const string second_block(block_size, 'B');
	EFCTestFileGuard test_file("test_efc_disabled_sequential_position.bin", first_block + second_block);

	CachingFileSystem cfs(*tracking_fs, *db.instance);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadSequential(*handle, block_size) == first_block);
	REQUIRE(CountCachedBlocks(cache) == 1);

	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET enable_external_file_cache=false"));
	REQUIRE(ReadSequential(*handle, block_size) == second_block);
}

TEST_CASE("Concurrent SET and Read do not corrupt data or cache state", "[external_file_cache]") {
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;

	constexpr idx_t FILE_SIZE = 2 * BlockCache::BLOCK_SIZE + 137; // odd tail to stress boundaries
	const auto content = MakeTestContent(FILE_SIZE);
	EFCTestFileGuard test_file("test_efc_set_vs_read.bin", content);

	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();
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
	    {BlockCache::BLOCK_SIZE - 8500, 17000},
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
	auto &cache = db_instance.GetBlockCache();
	idx_t total_cached_bytes = 0;
	for (auto &info : cache.GetBlockInformation()) {
		REQUIRE(info.location + info.nr_bytes <= FILE_SIZE);
		total_cached_bytes += info.nr_bytes;
	}

	REQUIRE(total_cached_bytes <= FILE_SIZE);
	auto handle = cfs.OpenFile(MakeTestOpenFileInfo(test_file.GetPath()), FileFlags::FILE_FLAGS_READ);
	REQUIRE(ReadFull(*handle, FILE_SIZE) == content);
}

TEST_CASE("Disabling external file cache drops its blocks", "[external_file_cache]") {
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto tracking_fs = make_uniq<EFCTrackingFileSystem>();
	CachingFileSystem cfs(*tracking_fs, db_instance);
	auto &cache = db_instance.GetBlockCache();
	Connection con(db);

	const auto content = MakeTestContent(BlockCache::BLOCK_SIZE);
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetBlockCache();

	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
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
	BlockCacheFileInfo info;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetBlockCache();

	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	const auto max_age_micros = fresh_fs->max_age_micros;
	fresh_fs->max_age_micros = SHORT_MAX_AGE_MICROS;

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
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
	BlockCacheFileInfo info;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetBlockCache();

	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;

	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	fresh_fs->max_age_micros = SHORT_MAX_AGE_MICROS;

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto validating_fs = make_uniq<CachePolicyFileSystem>();

	const idx_t block_size = BlockCache::BLOCK_SIZE;
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

TEST_CASE("File marked as not cacheable does not retain cached blocks", "[external_file_cache]") {
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetBlockCache();
	auto policy_fs = make_uniq<CachePolicyFileSystem>();

	const idx_t block_size = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetBlockCache();
	auto policy_fs = make_uniq<CachePolicyFileSystem>();
	policy_fs->cache_valid_until = timestamp_t::ninfinity();

	const idx_t block_size = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;

	auto fresh_fs = make_uniq<FreshnessOnlyFileSystem>();
	fresh_fs->max_age_micros = -1000000; // deadline is always in the past

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetBlockCache();
	auto policy_fs = make_uniq<BlockingCachePolicyFileSystem>();

	const idx_t block_size = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetBlockCache();
	auto policy_fs = make_uniq<CachePolicyFileSystem>();

	const idx_t block_size = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &db_instance = *db.instance;
	auto &cache = db_instance.GetBlockCache();

	auto no_meta_fs = make_uniq<EFCNoMetadataFileSystem>();

	const idx_t BLOCK_SIZE = BlockCache::BLOCK_SIZE;
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
	DuckDB db = MakeCacheLocalFilesDB();
	auto &cache = db.instance->GetBlockCache();
	auto policy_fs = make_uniq<CachePolicyFileSystem>();

	const idx_t block_size = BlockCache::BLOCK_SIZE;
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

} // namespace duckdb
