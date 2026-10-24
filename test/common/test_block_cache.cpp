#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <absl/time/clock.h>
#include <chrono>

namespace duckdb {

namespace {

constexpr idx_t BLOCK = BlockCache::BLOCK_SIZE;

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
				return !hold;
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

	//! Makes every read take [latency_p] plus the time its bytes take at [bandwidth_p] bytes per second, as one request
	//! to an object store does
	void Throttle(absl::Duration latency_p, double bandwidth_p) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		latency = latency_p;
		bandwidth = bandwidth_p;
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			writes.push_back(location);
		}
		LocalFileSystem::Write(handle, buffer, nr_bytes, location);
	}

	vector<idx_t> TakeWrites() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return std::move(writes);
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
	vector<idx_t> writes DUCKDB_GUARDED_BY(lock);
	idx_t failures DUCKDB_GUARDED_BY(lock) = 0;
	bool hold DUCKDB_GUARDED_BY(lock) = false;
	bool no_store DUCKDB_GUARDED_BY(lock) = false;
	absl::Duration latency DUCKDB_GUARDED_BY(lock) = absl::ZeroDuration();
	double bandwidth DUCKDB_GUARDED_BY(lock) = 0;
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

	shared_ptr<BlockCacheFile> Open(BlockCache &cache, CountingSourceFileSystem &fs,
	                                const string &version = "v1") const {
		shared_ptr<FileHandle> handle =
		    fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_PARALLEL_ACCESS);
		BlockCacheFileInfo info;
		info.validation.file_size = handle->GetFileSize();
		info.validation.version_tag = version;
		return cache.OpenFile(path, info, true, std::move(handle));
	}

	const string path;
};

bool HoldsExpectedBytes(const BlockCachePin &pin, idx_t block) {
	for (idx_t i = 0; i < pin.Size(); i++) {
		if (pin.Ptr()[i] != ExpectedByte(block * BLOCK + i)) {
			return false;
		}
	}
	return true;
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

//! The memory the buffer pool of [db] accounts to block cache frames
idx_t CacheReservation(DatabaseInstance &db) {
	for (auto &info : BufferManager::GetBufferManager(db).GetMemoryUsageInfo()) {
		if (info.tag == MemoryTag::EXTERNAL_FILE_CACHE) {
			return info.size;
		}
	}
	return 0;
}

idx_t CountSpillFiles(const string &directory) {
	LocalFileSystem local;
	idx_t count = 0;
	local.ListFiles(directory, [&](const string &name, bool is_directory) {
		if (!is_directory && StringUtil::EndsWith(name, "block_cache.spill")) {
			count++;
		}
	});
	return count;
}

} // namespace

TEST_CASE("Block cache pins hold the bytes of their block", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_pins.bin", 5 * BLOCK + 1000);
	auto cached = file.Open(cache, fs);

	REQUIRE(cached->BlockCount() == 6);
	for (idx_t block = 0; block < 6; block++) {
		auto pin = cache.Pin(cached, block);
		REQUIRE(pin.Size() == (block == 5 ? 1000 : BLOCK));
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 6);

	for (idx_t block = 0; block < 6; block++) {
		auto pin = cache.Pin(cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 6);
}

TEST_CASE("Block cache reopens of an unchanged file share its blocks", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_reopen.bin", 3 * BLOCK);
	{
		auto cached = file.Open(cache, fs);
		auto pin = cache.Pin(cached, 1);
	}
	auto again = file.Open(cache, fs);
	auto pin = cache.Pin(again, 1);
	REQUIRE(HoldsExpectedBytes(pin, 1));
	REQUIRE(fs.ReadCount() == 1);

	auto changed = file.Open(cache, fs, "v2");
	auto changed_pin = cache.Pin(changed, 1);
	REQUIRE(fs.ReadCount() == 2);
}

TEST_CASE("Block cache fetches a missing block once for concurrent readers", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_once.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	atomic<idx_t> correct {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t i = 0; i < 16; i++) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor,
		    [&]() {
			    auto pin = cache.Pin(cached, 2);
			    if (HoldsExpectedBytes(pin, 2)) {
				    correct++;
			    }
		    },
		    "BlockCacheTestTask"));
	}
	fs.AwaitReads(1);
	fs.Hold(false);
	executor.WorkOnTasks();
	REQUIRE(correct == 16);
	REQUIRE(fs.ReadCount() == 1);
}

TEST_CASE("Block cache prefetch merges runs across small gaps and caps each request", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 256 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_prefetch.bin", 200 * BLOCK);
	auto cached = file.Open(cache, fs);

	for (idx_t block : {10, 11, 30}) {
		cache.Pin(cached, block);
	}
	fs.TakeReads();

	cache.Prefetch(cached, 0, 100 * BLOCK);
	for (idx_t block = 0; block < 100; block++) {
		auto pin = cache.Pin(cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	auto reads = fs.TakeReads();
	idx_t covered = 0;
	for (auto &read : reads) {
		REQUIRE(read.second <= BlockCache::MAX_REQUEST);
		REQUIRE(read.first % BLOCK == 0);
		covered += read.second;
	}
	REQUIRE(reads.size() == 2);
	REQUIRE(covered == 100 * BLOCK);
}

TEST_CASE("Block cache range pins fetch the missing blocks of a range in one request", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_range.bin", 20 * BLOCK + 10);
	auto cached = file.Open(cache, fs);
	cache.Pin(cached, 5);
	fs.TakeReads();

	auto pins = cache.PinRange(cached, 2 * BLOCK + 100, 10 * BLOCK);
	REQUIRE(pins.size() == 11);
	for (idx_t i = 0; i < pins.size(); i++) {
		REQUIRE(HoldsExpectedBytes(pins[i], 2 + i));
	}
	auto reads = fs.TakeReads();
	REQUIRE(reads.size() == 1);
	REQUIRE(reads[0] == make_pair(2 * BLOCK, 11 * BLOCK));

	REQUIRE(cache.TryPinRange(*cached, 2 * BLOCK, 11 * BLOCK).size() == 11);
	REQUIRE(cache.TryPinRange(*cached, 0, 3 * BLOCK).empty());
	REQUIRE(fs.ReadCount() == 0);
}

TEST_CASE("Block cache range read uses a response that forbids reuse once, without workers", "[block_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_no_store.bin", 8 * BLOCK);
	auto cached = file.Open(cache, fs);
	fs.NoStore(true);
	{
		auto pins = cache.PinRange(cached, 0, 8 * BLOCK);
		REQUIRE(pins.size() == 8);
		for (idx_t i = 0; i < pins.size(); i++) {
			REQUIRE(HoldsExpectedBytes(pins[i], i));
		}
		REQUIRE(fs.ReadCount() == 1);
		REQUIRE_FALSE(cache.IsCached(*cached, 3 * BLOCK, BLOCK));
	}
	REQUIRE(cache.GetStatistics().used == 0);
	auto pin = cache.Pin(cached, 3);
	REQUIRE(HoldsExpectedBytes(pin, 3));
	REQUIRE(fs.ReadCount() == 2);
}

TEST_CASE("Block cache drops pinned blocks of a forgotten file with their last pin", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_forget.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);
	auto held = cache.Pin(cached, 1);
	cache.Pin(cached, 2);
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);

	cache.ForgetFile(file.path);
	REQUIRE(HoldsExpectedBytes(held, 1));
	REQUIRE_FALSE(cache.IsCached(*cached, BLOCK, BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 2 * BLOCK, BLOCK));
	REQUIRE(cache.GetStatistics().used == BLOCK);
	held.Reset();
	REQUIRE(cache.GetStatistics().used == 0);
	BlockCacheFileInfo info;
	REQUIRE_FALSE(cache.TryGetFile(file.path, info));
}

namespace {

void WriteBlocks(BlockCache &cache, BlockCacheFile &file, idx_t size) {
	for (idx_t block = 0; block * BLOCK < size; block++) {
		auto pin = cache.CreateBlock(file, block);
		const auto bytes = MinValue<idx_t>(BLOCK, size - block * BLOCK);
		for (idx_t i = 0; i < bytes; i++) {
			pin.GetDataMutable()[i] = ExpectedByte(block * BLOCK + i);
		}
	}
}

shared_ptr<BlockCacheFile> OpenToRead(BlockCache &cache, CountingSourceFileSystem &fs, const TestFile &file) {
	shared_ptr<FileHandle> handle =
	    fs.OpenFile(file.path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_PARALLEL_ACCESS);
	BlockCacheFileInfo info;
	info.validation.file_size = handle->GetFileSize();
	return cache.OpenFile(file.path, info, false, std::move(handle));
}

} // namespace

TEST_CASE("Block cache shares the blocks a writer created once the file is published", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_written.bin", 2 * BLOCK + 100);
	{
		auto written = cache.CreateWrittenFile(file.path);
		WriteBlocks(cache, *written, 2 * BLOCK + 100);
		cache.FinishWrittenFile(*written, 2 * BLOCK + 100, true);
	}
	auto cached = OpenToRead(cache, fs, file);
	REQUIRE(cached->GetSize() == 2 * BLOCK + 100);
	for (idx_t block = 0; block < 3; block++) {
		auto pin = cache.Pin(cached, block);
		REQUIRE(pin.Size() == (block == 2 ? 100 : BLOCK));
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 0);
}

TEST_CASE("Block cache drops the blocks of a written file that is abandoned", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_abandoned.bin", 2 * BLOCK);
	{
		auto written = cache.CreateWrittenFile(file.path);
		WriteBlocks(cache, *written, 2 * BLOCK);
		REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
		cache.FinishWrittenFile(*written, 2 * BLOCK, false);
	}
	REQUIRE(cache.GetStatistics().used == 0);
	auto cached = OpenToRead(cache, fs, file);
	auto pin = cache.Pin(cached, 1);
	REQUIRE(HoldsExpectedBytes(pin, 1));
	REQUIRE(fs.ReadCount() == 1);
}

TEST_CASE("Block cache appender publishes a written file at the size appended, or drops it", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_appended.bin", 2 * BLOCK + 100);
	vector<uint8_t> data(2 * BLOCK + 100);
	for (idx_t i = 0; i < data.size(); i++) {
		data[i] = ExpectedByte(i);
	}
	{
		BlockCacheAppender appender(cache, cache.CreateWrittenFile(file.path));
		appender.Append(data.data(), 1000);
		appender.Append(data.data() + 1000, data.size() - 1000);
		REQUIRE(appender.Size() == data.size());
		appender.Finish();
	}
	auto cached = OpenToRead(cache, fs, file);
	REQUIRE(cached->GetSize() == data.size());
	for (idx_t block = 0; block < 3; block++) {
		auto pin = cache.Pin(cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(fs.ReadCount() == 0);

	BlockCacheAppender abandoned(cache, cache.CreateWrittenFile(file.path));
	abandoned.Append(data.data(), BLOCK + 1);
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
	abandoned.Abandon();
	REQUIRE(cache.GetStatistics().used == 0);
}

TEST_CASE("Block cache forgets files no one opens and holds no block of", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile with_block("block_cache_sweep_cached.bin", 2 * BLOCK);
	TestFile kept_open("block_cache_sweep_open.bin", 2 * BLOCK);
	cache.Pin(with_block.Open(cache, fs), 0);
	auto open = kept_open.Open(cache, fs);

	BlockCacheFileInfo info;
	info.validation.file_size = 100;
	info.validation.version_tag = "v1";
	for (idx_t i = 0; i < 3000; i++) {
		cache.OpenFile("unused_" + to_string(i), info, true, nullptr);
	}
	BlockCacheFileInfo found;
	REQUIRE_FALSE(cache.TryGetFile("unused_0", found));
	REQUIRE(cache.TryGetFile(with_block.path, found));
	REQUIRE(cache.TryGetFile(kept_open.path, found));
	REQUIRE(cache.GetBlockInformation().size() == 1);
}

TEST_CASE("Disabling the block cache drops blocks of backed files and keeps in-memory ones", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_disable.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);
	auto held = cache.Pin(cached, 0);
	cache.Pin(cached, 1);
	auto memory = cache.CreateUnbackedFile("memory://disable.bin");
	memset(cache.CreateBlock(*memory, 0).GetDataMutable(), 7, BLOCK);
	cache.SetUnbackedSize(*memory, BLOCK);

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

TEST_CASE("Block cache eviction keeps blocks read twice over blocks read once", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 16 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_probation.bin", 200 * BLOCK);
	auto cached = file.Open(cache, fs);

	for (idx_t round = 0; round < 2; round++) {
		for (idx_t block = 0; block < 4; block++) {
			cache.Pin(cached, block);
		}
	}
	for (idx_t block = 4; block < 200; block++) {
		cache.Pin(cached, block);
	}
	idx_t kept = 0;
	for (idx_t block = 0; block < 4; block++) {
		kept += cache.IsCached(*cached, block * BLOCK, BLOCK);
	}
	REQUIRE(kept == 4);
	REQUIRE(cache.GetStatistics().used <= 16 * BLOCK);
}

TEST_CASE("Block cache never evicts a pinned block", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 8 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_pinned.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	vector<BlockCachePin> pins;
	for (idx_t block = 0; block < 4; block++) {
		pins.push_back(cache.Pin(cached, block));
	}
	for (idx_t block = 4; block < 64; block++) {
		auto pin = cache.Pin(cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	for (idx_t block = 0; block < 4; block++) {
		REQUIRE(HoldsExpectedBytes(pins[block], block));
		REQUIRE(cache.IsCached(*cached, block * BLOCK, BLOCK));
	}
}

TEST_CASE("Block cache reads a block into a buffer of its own when every frame is pinned", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 2 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_private.bin", 8 * BLOCK);
	auto cached = file.Open(cache, fs);
	auto first = cache.Pin(cached, 0);
	auto second = cache.Pin(cached, 1);

	auto third = cache.Pin(cached, 5);
	REQUIRE(third.IsValid());
	REQUIRE(HoldsExpectedBytes(third, 5));
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
	third.Reset();
	REQUIRE_FALSE(cache.IsCached(*cached, 5 * BLOCK, BLOCK));
	REQUIRE(HoldsExpectedBytes(first, 0));
	REQUIRE(HoldsExpectedBytes(second, 1));
}

TEST_CASE("Block cache zeroes a fetched last block past the end of its file", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 2 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile full("block_cache_full.bin", 2 * BLOCK);
	TestFile partial("block_cache_partial.bin", BLOCK + 1000);
	auto full_cached = full.Open(cache, fs);
	const auto fill_frames = [&] {
		for (idx_t block = 0; block < 2; block++) {
			REQUIRE(HoldsExpectedBytes(cache.Pin(full_cached, block), block));
		}
	};
	const auto zeroed_past_end = [](const BlockCachePin &pin) {
		for (idx_t i = pin.Size(); i < BLOCK; i++) {
			if (pin.Ptr()[i] != 0) {
				return false;
			}
		}
		return true;
	};

	fill_frames();
	auto cached = partial.Open(cache, fs);
	auto single = cache.Pin(cached, 1);
	REQUIRE(single.Size() == 1000);
	REQUIRE(HoldsExpectedBytes(single, 1));
	REQUIRE(zeroed_past_end(single));
	single.Reset();
	cached.reset();
	cache.ForgetFile(partial.path);

	fill_frames();
	cached = partial.Open(cache, fs);
	auto pins = cache.PinRange(cached, 0, BLOCK + 1000);
	REQUIRE(pins.size() == 2);
	REQUIRE(HoldsExpectedBytes(pins[1], 1));
	REQUIRE(zeroed_past_end(pins[1]));
}

TEST_CASE("Block cache pins fail with the read error and later pins fetch again", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 16 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_failure.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Fail(1);
	REQUIRE_THROWS_WITH(cache.Pin(cached, 1), Catch::Contains("injected read failure"));
	auto pin = cache.Pin(cached, 1);
	REQUIRE(HoldsExpectedBytes(pin, 1));

	fs.Fail(1);
	cache.Prefetch(cached, 2 * BLOCK, BLOCK);
	auto prefetched = cache.Pin(cached, 2);
	REQUIRE(HoldsExpectedBytes(prefetched, 2));
}

TEST_CASE("Block cache spills evicted blocks and reads misses back from the spill tier", "[block_cache]") {
	for (const bool tmpfs : {false, true}) {
		string spill_path;
		if (tmpfs) {
			LocalFileSystem local;
			if (!local.DirectoryExists("/dev/shm")) {
				continue;
			}
			spill_path = "/dev/shm/duckdb_block_cache_spill_test_" + to_string(getpid());
		} else {
			spill_path = TestCreatePath("block_cache_spill");
		}
		DuckDB db(nullptr);
		BlockCache cache(*db.instance, 8 * BLOCK);
		cache.SetSpill(spill_path, 64 * BLOCK);
		CountingSourceFileSystem fs;
		TestFile file("block_cache_spill.bin", 32 * BLOCK);
		auto cached = file.Open(cache, fs);

		for (idx_t block = 0; block < 32; block++) {
			cache.Pin(cached, block);
		}
		REQUIRE(fs.ReadCount() == 32);
		REQUIRE(cache.GetStatistics().spilled > 0);
		for (idx_t block = 0; block < 32; block++) {
			auto pin = cache.Pin(cached, block);
			REQUIRE(HoldsExpectedBytes(pin, block));
		}
		REQUIRE(fs.ReadCount() == 32);
		REQUIRE(cache.GetStatistics().spill_reads > 0);
		cache.SetSpill("", 0);
	}
}

TEST_CASE("Block cache unbacked blocks spill and are never dropped", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 4 * BLOCK);
	cache.SetSpill(TestCreatePath("block_cache_unbacked_spill"), 0);
	auto file = cache.CreateUnbackedFile("memory://unbacked.bin");
	for (idx_t block = 0; block < 16; block++) {
		auto pin = cache.CreateBlock(*file, block);
		memset(pin.GetDataMutable(), int(block + 1), BLOCK);
	}
	cache.SetUnbackedSize(*file, 16 * BLOCK);
	for (idx_t block = 0; block < 16; block++) {
		auto pin = cache.Pin(file, block);
		REQUIRE(pin.Ptr()[0] == block + 1);
		REQUIRE(pin.Ptr()[BLOCK - 1] == block + 1);
	}
}

TEST_CASE("Block cache reads a spilled block into a buffer of its own when every frame is pinned", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 2 * BLOCK);
	cache.SetSpill(TestCreatePath("block_cache_private_spill"), 0);
	auto file = cache.CreateUnbackedFile("memory://private_spill.bin");
	memset(cache.CreateBlock(*file, 0).GetDataMutable(), 1, BLOCK);
	auto first = cache.CreateBlock(*file, 1);
	memset(first.GetDataMutable(), 2, BLOCK);
	auto second = cache.CreateBlock(*file, 2);
	memset(second.GetDataMutable(), 3, BLOCK);
	cache.SetUnbackedSize(*file, 3 * BLOCK);

	auto spilled = cache.Pin(file, 0);
	REQUIRE(spilled.Ptr()[0] == 1);
	REQUIRE(spilled.Ptr()[BLOCK - 1] == 1);
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
}

#ifdef DEBUG
TEST_CASE("Block cache keeps no copy of a block whose in-memory file goes while the block is evicted",
          "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 4 * BLOCK);
	cache.SetSpill(TestCreatePath("block_cache_released_spill"), 0);
	auto file = cache.CreateUnbackedFile("memory://released.bin");
	for (idx_t block = 0; block < 4; block++) {
		memset(cache.CreateBlock(*file, block).GetDataMutable(), int(block + 1), BLOCK);
	}
	cache.SetUnbackedSize(*file, 4 * BLOCK);
	cache.debug_before_keep_copy = [&]() {
		file.reset();
	};
	auto other = cache.CreateUnbackedFile("memory://other.bin");
	auto pin = cache.CreateBlock(*other, 0);
	REQUIRE_FALSE(file);
	REQUIRE_NOTHROW(cache.SetSpill(TestCreatePath("block_cache_released_spill_moved"), 0));
}

TEST_CASE("Block cache readers of an in-memory block an eviction is copying get the copy", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 2 * BLOCK);
	cache.SetSpill(TestCreatePath("block_cache_copying_spill"), 0);
	auto file = cache.CreateUnbackedFile("memory://copying.bin");
	for (idx_t block = 0; block < 2; block++) {
		memset(cache.CreateBlock(*file, block).GetDataMutable(), int(block + 1), BLOCK);
	}
	cache.SetUnbackedSize(*file, 2 * BLOCK);
	auto held = cache.Pin(file, 1);

	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	atomic<bool> right {false};
	Signal finished;
	cache.debug_before_keep_copy = [&]() {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor,
		    [&]() {
			    absl::Cleanup finish = [&]() {
				    finished.Set();
			    };
			    auto pin = cache.Pin(file, 0);
			    right = pin.Ptr()[0] == 1 && pin.Ptr()[BLOCK - 1] == 1;
		    },
		    "BlockCacheTestTask"));
		finished.AwaitFor(absl::Milliseconds(500));
	};
	auto other = cache.CreateUnbackedFile("memory://copying_other.bin");
	auto pin = cache.CreateBlock(*other, 0);
	executor.WorkOnTasks();
	REQUIRE(right);
}
#endif

TEST_CASE("Block cache gives memory back to the buffer pool", "[block_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='64MiB'"));
	auto &cache = BlockCache::Get(*db.instance);
	REQUIRE(cache.GetStatistics().capacity == 16 * 1024 * 1024);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_shrink.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);
	for (idx_t block = 0; block < 64; block++) {
		cache.Pin(cached, block);
	}
	const auto filled = cache.GetStatistics().used;
	REQUIRE(filled == 64 * BLOCK);

	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	vector<BufferHandle> buffers;
	for (idx_t i = 0; i < 14; i++) {
		buffers.push_back(buffer_manager.Allocate(MemoryTag::IN_MEMORY_TABLE, 4ULL * 1024ULL * 1024ULL));
	}
	REQUIRE(cache.GetStatistics().used < filled);
	for (idx_t block = 0; block < 64; block++) {
		auto pin = cache.Pin(cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
}

TEST_CASE("Block cache returns the frames of a dropped file and hands them out again in address order",
          "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 64 * BLOCK);
	const auto before = CacheReservation(*db.instance);
	for (idx_t round = 0; round < 2; round++) {
		auto file = cache.CreateUnbackedFile("memory://dropped.bin");
		vector<const_data_ptr_t> frames;
		for (idx_t block = 0; block < 32; block++) {
			auto pin = cache.CreateBlock(*file, block);
			memset(pin.GetDataMutable(), int(block + 1), BLOCK);
			frames.push_back(pin.Ptr());
		}
		cache.SetUnbackedSize(*file, 32 * BLOCK);
		REQUIRE(CacheReservation(*db.instance) == before + 32 * BLOCK);
		for (idx_t block = 1; block < 32; block++) {
			REQUIRE(frames[block] == frames[0] + block * BLOCK);
		}
		file.reset();
		REQUIRE(cache.GetStatistics().used == 0);
		REQUIRE(CacheReservation(*db.instance) == before);
	}
}

TEST_CASE("Block cache reuses the frame it evicts when the buffer pool has no room for another", "[block_cache]") {
	DBConfig config;
	config.options.maximum_memory = 64ULL * 1024ULL * 1024ULL;
	DuckDB db(nullptr, &config);
	auto &cache = BlockCache::Get(*db.instance);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_reuse.bin", 3 * BLOCK);
	auto cached = file.Open(cache, fs);
	const auto evicted = cache.Pin(cached, 0).Ptr();
	auto held = cache.Pin(cached, 1);
	idx_t reserved = 0;
	while (buffer_manager.TryReserveMemory(MemoryTag::IN_MEMORY_TABLE, BLOCK, false)) {
		reserved += BLOCK;
	}

	auto pin = cache.Pin(cached, 2);
	buffer_manager.FreeReservedMemory(MemoryTag::IN_MEMORY_TABLE, reserved);
	REQUIRE(pin.Ptr() == evicted);
	REQUIRE(HoldsExpectedBytes(pin, 2));
	REQUIRE(HoldsExpectedBytes(held, 1));
	REQUIRE_FALSE(cache.IsCached(*cached, 0, BLOCK));
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
	REQUIRE(CacheReservation(*db.instance) == 2 * BLOCK);
}

TEST_CASE("Block cache concurrent pins and evictions return the right bytes", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 16 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_stress.bin", 128 * BLOCK);
	auto cached = file.Open(cache, fs);

	atomic<idx_t> wrong {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t t = 0; t < 16; t++) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor,
		    [&, t]() {
			    uint64_t seed = t * 7919 + 1;
			    try {
				    for (idx_t i = 0; i < 400; i++) {
					    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
					    const auto block = (seed >> 33) % 128;
					    if (i % 3 == 0) {
						    auto pins = cache.TryPinRange(*cached, block * BLOCK, BLOCK);
						    if (!pins.empty() && !HoldsExpectedBytes(pins[0], block)) {
							    wrong++;
						    }
						    continue;
					    }
					    if (i % 7 == 0) {
						    cache.Prefetch(cached, block * BLOCK, 4 * BLOCK);
					    }
					    auto pin = cache.Pin(cached, block);
					    if (!HoldsExpectedBytes(pin, block)) {
						    wrong++;
					    }
				    }
			    } catch (std::exception &) {
				    wrong++;
			    }
		    },
		    "BlockCacheTestTask"));
	}
	executor.WorkOnTasks();
	REQUIRE(wrong == 0);
}

TEST_CASE("Block cache counts every hit of concurrent pins of one block and lets the block go after them",
          "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 4 * BLOCK);
	auto file = cache.CreateUnbackedFile("memory://hot.bin");
	memset(cache.CreateBlock(*file, 0).GetDataMutable(), 9, BLOCK);
	cache.SetUnbackedSize(*file, BLOCK);
	const auto before = cache.GetStatistics();
	constexpr idx_t TASKS = 8;
	constexpr idx_t PINS = 10000;
	atomic<idx_t> wrong {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t task = 0; task < TASKS; task++) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor,
		    [&]() {
			    for (idx_t i = 0; i < PINS; i++) {
				    auto pin = cache.Pin(file, 0);
				    if (pin.Ptr()[0] != 9 || pin.Ptr()[BLOCK - 1] != 9) {
					    wrong++;
				    }
			    }
		    },
		    "BlockCacheTestTask"));
	}
	executor.WorkOnTasks();
	REQUIRE(wrong == 0);
	const auto after = cache.GetStatistics();
	REQUIRE(after.hits - before.hits == TASKS * PINS);
	REQUIRE(after.misses == before.misses);
	file.reset();
	REQUIRE(cache.GetStatistics().used == 0);
}

TEST_CASE("Block cache spill drops the oldest copy first after the copies of a forgotten file go", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, BLOCK);
	cache.SetSpill(TestCreatePath("block_cache_spill_order"), 2 * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_spill_order.bin", 4 * BLOCK);
	TestFile other("block_cache_spill_order_other.bin", BLOCK);
	auto cached = file.Open(cache, fs);
	cache.Pin(cached, 0);
	cache.Pin(cached, 1);
	cache.ForgetFile(file.path);
	cache.Pin(cached, 2);
	cache.Pin(cached, 0);
	cache.Pin(cached, 3);
	cache.Pin(other.Open(cache, fs), 0);
	cache.ForgetFile(other.path);

	const auto reads = fs.ReadCount();
	auto pin = cache.Pin(cached, 0);
	REQUIRE(HoldsExpectedBytes(pin, 0));
	REQUIRE(fs.ReadCount() == reads);
}

namespace {

uint8_t FillOf(uint64_t key) {
	return static_cast<uint8_t>(key % 251 + 1);
}

//! Spills a copy of [key] filled with FillOf([key]) from [buffer]
bool WriteCopy(BlockCacheSpill &spill, uint64_t key, BlockCachePin &buffer) {
	memset(buffer.GetDataMutable(), FillOf(key), BLOCK);
	BlockCacheSpill::Reservation copy;
	return spill.Reserve(key, true, copy) && spill.Write(copy, buffer.GetDataMutable());
}

//! Whether the copy of [key] reads back into [buffer] as WriteCopy wrote it
bool ReadsCopy(BlockCacheSpill &spill, uint64_t key, BlockCachePin &buffer) {
	return spill.Read(key, buffer.GetDataMutable()) && buffer.Ptr()[0] == FillOf(key) &&
	       buffer.Ptr()[BLOCK - 1] == FillOf(key);
}

} // namespace

TEST_CASE("Block cache spill reads and writes of concurrent tasks return the right bytes", "[block_cache]") {
	DuckDB db(nullptr);
	LocalFileSystem fs;
	BlockCacheSpill spill;
	spill.Configure(fs, TestCreatePath("block_cache_spill_concurrent"), 0);
	constexpr idx_t TASKS = 8;
	constexpr idx_t BLOCKS = 32;
	atomic<idx_t> wrong {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t task = 0; task < TASKS; task++) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor,
		    [&, task]() {
			    auto buffer = BlockCache::AllocateUncached(*db.instance, BLOCK);
			    uint64_t seed = task * 7919 + 1;
			    for (idx_t block = 0; block < BLOCKS; block++) {
				    if (!WriteCopy(spill, BlockCache::MakeKey(NumericCast<uint32_t>(task + 1), block), buffer)) {
					    wrong++;
				    }
				    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
				    const auto file = NumericCast<uint32_t>((seed >> 33) % TASKS + 1);
				    const auto key = BlockCache::MakeKey(file, (seed >> 13) % BLOCKS);
				    if (spill.Contains(key) && !ReadsCopy(spill, key, buffer)) {
					    wrong++;
				    }
			    }
		    },
		    "BlockCacheTestTask"));
	}
	executor.WorkOnTasks();
	REQUIRE(wrong == 0);
	auto buffer = BlockCache::AllocateUncached(*db.instance, BLOCK);
	for (idx_t task = 0; task < TASKS; task++) {
		for (idx_t block = 0; block < BLOCKS; block++) {
			REQUIRE(ReadsCopy(spill, BlockCache::MakeKey(NumericCast<uint32_t>(task + 1), block), buffer));
		}
	}
}

TEST_CASE("Block cache spill reuses the slot of a copy dropped while it is read only after the read", "[block_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	BlockCacheSpill spill;
	spill.Configure(fs, TestCreatePath("block_cache_spill_slot_reuse"), 0);
	auto buffer = BlockCache::AllocateUncached(*db.instance, BLOCK);
	const auto dropped = BlockCache::MakeKey(1, 0);
	REQUIRE(WriteCopy(spill, dropped, buffer));
	const auto dropped_slot = fs.TakeWrites();
	REQUIRE(dropped_slot.size() == 1);

	fs.Hold(true);
	atomic<bool> right {false};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
	    executor,
	    [&]() {
		    auto target = BlockCache::AllocateUncached(*db.instance, BLOCK);
		    right = ReadsCopy(spill, dropped, target);
	    },
	    "BlockCacheTestTask"));
	fs.AwaitReads(1);
	spill.Erase(dropped);
	REQUIRE_FALSE(spill.Contains(dropped));
	REQUIRE(WriteCopy(spill, BlockCache::MakeKey(2, 0), buffer));
	REQUIRE(fs.TakeWrites() != dropped_slot);
	fs.Hold(false);
	executor.WorkOnTasks();
	REQUIRE(right);

	REQUIRE(WriteCopy(spill, BlockCache::MakeKey(3, 0), buffer));
	REQUIRE(fs.TakeWrites() == dropped_slot);
	REQUIRE(ReadsCopy(spill, BlockCache::MakeKey(2, 0), buffer));
	REQUIRE(ReadsCopy(spill, BlockCache::MakeKey(3, 0), buffer));
}

TEST_CASE("Block cache spill drops the copies of one file and keeps the copies of the others", "[block_cache]") {
	DuckDB db(nullptr);
	LocalFileSystem fs;
	BlockCacheSpill spill;
	spill.Configure(fs, TestCreatePath("block_cache_spill_erase_file"), 0);
	auto buffer = BlockCache::AllocateUncached(*db.instance, BLOCK);
	const vector<uint32_t> files {1, 2, 3, NumericLimits<uint32_t>::Maximum()};
	for (auto file : files) {
		for (idx_t block = 0; block < 4; block++) {
			REQUIRE(WriteCopy(spill, BlockCache::MakeKey(file, block), buffer));
		}
	}
	spill.EraseFile(2);
	spill.EraseFile(NumericLimits<uint32_t>::Maximum());
	REQUIRE(spill.Keys().size() == 8);
	for (auto file : files) {
		const bool kept = file == 1 || file == 3;
		for (idx_t block = 0; block < 4; block++) {
			const auto key = BlockCache::MakeKey(file, block);
			REQUIRE(spill.Contains(key) == kept);
			REQUIRE(ReadsCopy(spill, key, buffer) == kept);
		}
	}
}

TEST_CASE("Block cache spill readers of a reserved copy wait until it is written or cancelled", "[block_cache]") {
	DuckDB db(nullptr);
	LocalFileSystem fs;
	BlockCacheSpill spill;
	spill.Configure(fs, TestCreatePath("block_cache_spill_reserved"), 0);
	const auto written = BlockCache::MakeKey(1, 0);
	const auto cancelled = BlockCache::MakeKey(1, 1);
	BlockCacheSpill::Reservation written_copy;
	BlockCacheSpill::Reservation cancelled_copy;
	REQUIRE(spill.Reserve(written, true, written_copy));
	REQUIRE(spill.Reserve(cancelled, true, cancelled_copy));

	atomic<bool> read_written {false};
	atomic<bool> read_cancelled {true};
	Signal finished;
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
	    executor,
	    [&]() {
		    absl::Cleanup finish = [&]() {
			    finished.Set();
		    };
		    auto target = BlockCache::AllocateUncached(*db.instance, BLOCK);
		    read_written = ReadsCopy(spill, written, target);
		    read_cancelled = spill.Read(cancelled, target.GetDataMutable());
	    },
	    "BlockCacheTestTask"));
	REQUIRE_FALSE(finished.AwaitFor(absl::Milliseconds(100)));
	auto buffer = BlockCache::AllocateUncached(*db.instance, BLOCK);
	memset(buffer.GetDataMutable(), FillOf(written), BLOCK);
	REQUIRE(spill.Write(written_copy, buffer.GetDataMutable()));
	REQUIRE_FALSE(finished.AwaitFor(absl::Milliseconds(100)));
	spill.Cancel(cancelled_copy);
	executor.WorkOnTasks();
	REQUIRE(read_written);
	REQUIRE_FALSE(read_cancelled);
	REQUIRE_FALSE(spill.Contains(cancelled));
}

TEST_CASE("Block cache spill moves its file only after the reads in flight end", "[block_cache]") {
	DuckDB db(nullptr);
	CountingSourceFileSystem fs;
	BlockCacheSpill spill;
	spill.Configure(fs, TestCreatePath("block_cache_spill_move"), 16 * BLOCK);
	const auto moved_path = TestCreatePath("block_cache_spill_moved");
	auto buffer = BlockCache::AllocateUncached(*db.instance, BLOCK);
	const auto key = BlockCache::MakeKey(1, 0);
	memset(buffer.GetDataMutable(), FillOf(key), BLOCK);
	BlockCacheSpill::Reservation copy;
	REQUIRE(spill.Reserve(key, false, copy));
	REQUIRE(spill.Write(copy, buffer.GetDataMutable()));

	fs.Hold(true);
	atomic<bool> right {false};
	Signal moved;
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
	    executor,
	    [&]() {
		    auto target = BlockCache::AllocateUncached(*db.instance, BLOCK);
		    right = ReadsCopy(spill, key, target);
	    },
	    "BlockCacheTestTask"));
	fs.AwaitReads(1);
	executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
	    executor,
	    [&]() {
		    absl::Cleanup finish = [&]() {
			    moved.Set();
		    };
		    spill.Configure(fs, moved_path, 16 * BLOCK);
	    },
	    "BlockCacheTestTask"));
	REQUIRE_FALSE(moved.AwaitFor(absl::Milliseconds(100)));
	fs.Hold(false);
	executor.WorkOnTasks();
	REQUIRE(right);
	REQUIRE(spill.IsConfigured());
	REQUIRE_FALSE(spill.Contains(key));
}

TEST_CASE("Block cache pins of an in-memory file return the right bytes while it grows and spills", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 4 * BLOCK);
	cache.SetSpill(TestCreatePath("block_cache_growing_spill"), 0);
	auto file = cache.CreateUnbackedFile("memory://growing.bin");
	constexpr idx_t BLOCKS = 256;
	constexpr idx_t RECENT = 6;
	const auto fill = [](idx_t block) {
		return static_cast<uint8_t>(block % 251 + 1);
	};
	atomic<idx_t> created {0};
	const auto create = [&](idx_t block) {
		memset(cache.CreateBlock(*file, block).GetDataMutable(), fill(block), BLOCK);
		cache.SetUnbackedSize(*file, (block + 1) * BLOCK);
		created.store(block + 1, std::memory_order_release);
	};
	for (idx_t block = 0; block < RECENT; block++) {
		create(block);
	}
	atomic<bool> done {false};
	atomic<idx_t> wrong {0};
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	for (idx_t task = 0; task < 3; task++) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor,
		    [&, task]() {
			    uint64_t seed = task * 7919 + 1;
			    while (!done.load(std::memory_order_acquire)) {
				    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
				    const auto block = created.load(std::memory_order_acquire) - 1 - (seed >> 33) % RECENT;
				    auto pin = cache.Pin(file, block);
				    if (pin.Ptr()[0] != fill(block) || pin.Ptr()[BLOCK - 1] != fill(block)) {
					    wrong++;
				    }
			    }
		    },
		    "BlockCacheTestTask"));
	}
	for (idx_t block = RECENT; block < BLOCKS; block++) {
		create(block);
	}
	done.store(true, std::memory_order_release);
	executor.WorkOnTasks();
	REQUIRE(wrong == 0);
	for (idx_t block = 0; block < BLOCKS; block++) {
		auto pin = cache.Pin(file, block);
		REQUIRE(pin.Ptr()[0] == fill(block));
		REQUIRE(pin.Ptr()[BLOCK - 1] == fill(block));
	}
}

TEST_CASE("Changing the spill size keeps the spill file of in-memory blocks", "[block_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	const auto directory = TestCreatePath("block_cache_default_spill");
	REQUIRE_NO_FAIL(con.Query("SET temp_directory='" + directory + "'"));
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_size='1MiB'"));
	auto &cache = BlockCache::Get(*db.instance);
	auto file = cache.CreateUnbackedFile("memory://default_spill.bin");
	for (idx_t block = 0; block < 16; block++) {
		memset(cache.CreateBlock(*file, block).GetDataMutable(), int(block + 1), BLOCK);
	}
	cache.SetUnbackedSize(*file, 16 * BLOCK);
	REQUIRE(cache.GetStatistics().spilled > 0);

	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_spill_size='64MiB'"));
	REQUIRE_NO_FAIL(con.Query("RESET external_file_cache_spill_size"));
	for (idx_t block = 0; block < 16; block++) {
		auto pin = cache.Pin(file, block);
		REQUIRE(pin.Ptr()[0] == block + 1);
		REQUIRE(pin.Ptr()[BLOCK - 1] == block + 1);
	}

	file.reset();
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_spill_size='64MiB'"));
	REQUIRE(CountSpillFiles(directory) == 1);
}

TEST_CASE("Instances sharing a temporary directory spill in-memory blocks to files of their own", "[block_cache]") {
	const auto directory = TestCreatePath("block_cache_shared_spill");
	{
		DuckDB first(nullptr);
		DuckDB second(nullptr);
		vector<shared_ptr<BlockCacheFile>> files;
		for (auto db : {&first, &second}) {
			Connection con(*db);
			REQUIRE_NO_FAIL(con.Query("SET temp_directory='" + directory + "'"));
			REQUIRE_NO_FAIL(con.Query("SET external_file_cache_size='1MiB'"));
			auto &cache = BlockCache::Get(*db->instance);
			auto file = cache.CreateUnbackedFile("memory://shared_spill.bin");
			const auto fill = files.size() * 100 + 1;
			for (idx_t block = 0; block < 16; block++) {
				memset(cache.CreateBlock(*file, block).GetDataMutable(), int(fill + block), BLOCK);
			}
			cache.SetUnbackedSize(*file, 16 * BLOCK);
			REQUIRE(BufferManager::GetBufferManager(*db->instance).GetUsedSwap() == 12 * BLOCK);
			files.push_back(std::move(file));
		}
		REQUIRE(CountSpillFiles(directory) == 2);
		for (idx_t i = 0; i < files.size(); i++) {
			auto &cache = BlockCache::Get(*(i == 0 ? first : second).instance);
			for (idx_t block = 0; block < 16; block++) {
				auto pin = cache.Pin(files[i], block);
				REQUIRE(pin.Ptr()[0] == uint8_t(i * 100 + 1 + block));
				REQUIRE(pin.Ptr()[BLOCK - 1] == uint8_t(i * 100 + 1 + block));
			}
		}
	}
	REQUIRE(CountSpillFiles(directory) == 0);
}

namespace {

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

uint8_t BenchByte(idx_t block) {
	return static_cast<uint8_t>(block % 251 + 1);
}

} // namespace

TEST_CASE("Block cache benchmark: concurrent reads of spilled blocks", "[.][block_cache_bench]") {
	constexpr idx_t FRAMES = 64;
	constexpr idx_t BLOCKS = 1024;
	constexpr idx_t READS = 256;
	vector<pair<string, string>> spills {
	    {"direct I/O in the test directory", TestCreatePath("block_cache_bench_spill")}};
	LocalFileSystem local;
	if (local.DirectoryExists("/dev/shm")) {
		spills.emplace_back("tmpfs", "/dev/shm/duckdb_block_cache_bench_spill_" + to_string(getpid()));
	}
	for (auto &spill : spills) {
		DuckDB db(nullptr);
		BlockCache cache(*db.instance, FRAMES * BLOCK);
		cache.SetSpill(spill.second, 0);
		auto file = cache.CreateUnbackedFile("memory://bench_spill.bin");
		for (idx_t block = 0; block < BLOCKS; block++) {
			memset(cache.CreateBlock(*file, block).GetDataMutable(), BenchByte(block), BLOCK);
		}
		cache.SetUnbackedSize(*file, BLOCKS * BLOCK);
		for (idx_t block = 0; block < BLOCKS; block++) {
			cache.Pin(file, block);
		}
		for (const idx_t readers : {idx_t(1), idx_t(8)}) {
			for (idx_t round = 0; round < 3; round++) {
				atomic<idx_t> wrong {0};
				const auto spill_reads = cache.GetStatistics().spill_reads;
				const auto start = std::chrono::steady_clock::now();
				TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
				for (idx_t reader = 0; reader < readers; reader++) {
					executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
					    executor,
					    [&, reader]() {
						    uint64_t seed = (round * 8 + reader) * 7919 + 1;
						    for (idx_t i = 0; i < READS; i++) {
							    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
							    const auto block = (seed >> 33) % BLOCKS;
							    if (cache.Pin(file, block).Ptr()[BLOCK - 1] != BenchByte(block)) {
								    wrong++;
							    }
						    }
					    },
					    "BlockCacheBenchTask"));
				}
				executor.WorkOnTasks();
				const auto elapsed = MillisecondsSince(start);
				REQUIRE(wrong == 0);
				const idx_t read = cache.GetStatistics().spill_reads - spill_reads;
				Printer::PrintF(
				    "spill reads (%s), %llu concurrent readers, round %llu: %llu blocks in %.1f ms, %.0f MiB/s",
				    spill.first, readers, round, read, elapsed,
				    double(read * BLOCK) / (1024.0 * 1024.0) / (elapsed / 1000.0));
			}
		}
	}
}

TEST_CASE("Block cache benchmark: releasing the frames of a 1 GiB in-memory file", "[.][block_cache_bench]") {
	constexpr idx_t BLOCKS = 4096;
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, BLOCKS * BLOCK);
	for (idx_t round = 0; round < 5; round++) {
		auto file = cache.CreateUnbackedFile("memory://bench_release.bin");
		for (idx_t block = 0; block < BLOCKS; block++) {
			memset(cache.CreateBlock(*file, block).GetDataMutable(), BenchByte(block), BLOCK);
		}
		cache.SetUnbackedSize(*file, BLOCKS * BLOCK);
		REQUIRE(cache.GetStatistics().used == BLOCKS * BLOCK);
		const auto start = std::chrono::steady_clock::now();
		file.reset();
		const auto elapsed = MillisecondsSince(start);
		REQUIRE(cache.GetStatistics().used == 0);
		Printer::PrintF("releasing the frames of a 1 GiB in-memory file, round %llu: %.2f ms", round, elapsed);
	}
}

TEST_CASE("Block cache benchmark: pins that evict while the buffer pool is full", "[.][block_cache_bench]") {
	constexpr idx_t BLOCKS = 1024;
	constexpr idx_t RESIDENT = 64;
	constexpr idx_t HEADROOM = 2ULL * 1024ULL * 1024ULL;
	DBConfig config;
	config.options.maximum_memory = 256ULL * 1024ULL * 1024ULL;
	DuckDB db(nullptr, &config);
	auto &cache = BlockCache::Get(*db.instance);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_bench_source.bin", BLOCKS * BLOCK);
	for (idx_t round = 0; round < 3; round++) {
		auto cached = file.Open(cache, fs, "v" + to_string(round));
		for (idx_t block = 0; block < RESIDENT; block++) {
			cache.Pin(cached, block);
		}
		const auto used = buffer_manager.GetUsedMemory();
		const auto limit = buffer_manager.GetMaxMemory();
		const idx_t reserved = limit > used + HEADROOM ? limit - used - HEADROOM : 0;
		buffer_manager.ReserveMemory(MemoryTag::IN_MEMORY_TABLE, reserved);
		const auto evictions = cache.GetStatistics().evictions;
		idx_t wrong = 0;
		const auto start = std::chrono::steady_clock::now();
		for (idx_t block = RESIDENT; block < BLOCKS; block++) {
			if (cache.Pin(cached, block).Ptr()[0] != ExpectedByte(block * BLOCK)) {
				wrong++;
			}
		}
		const auto elapsed = MillisecondsSince(start);
		const idx_t evicted = cache.GetStatistics().evictions - evictions;
		buffer_manager.FreeReservedMemory(MemoryTag::IN_MEMORY_TABLE, reserved);
		REQUIRE(wrong == 0);
		Printer::PrintF("pins that evict with the buffer pool full, round %llu: %.1f us per block, %llu evictions",
		                round, elapsed * 1000.0 / double(BLOCKS - RESIDENT), evicted);
	}
}

TEST_CASE("Block cache benchmark: concurrent pins of one block and of distinct blocks", "[.][block_cache_bench]") {
	constexpr idx_t PINS = 500000;
	constexpr idx_t BLOCKS = 64;
	for (const idx_t threads : {idx_t(1), idx_t(8), idx_t(16), idx_t(32), idx_t(64)}) {
		DBConfig config;
		config.options.maximum_threads = threads;
		DuckDB db(nullptr, &config);
		BlockCache cache(*db.instance, 2 * BLOCKS * BLOCK);
		auto file = cache.CreateUnbackedFile("memory://bench_pins.bin");
		for (idx_t block = 0; block < BLOCKS; block++) {
			memset(cache.CreateBlock(*file, block).GetDataMutable(), BenchByte(block), BLOCK);
		}
		cache.SetUnbackedSize(*file, BLOCKS * BLOCK);
		for (const bool one_block : {true, false}) {
			atomic<idx_t> wrong {0};
			const auto start = std::chrono::steady_clock::now();
			TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
			for (idx_t thread = 0; thread < threads; thread++) {
				executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
				    executor,
				    [&, thread]() {
					    const auto block = one_block ? 0 : thread % BLOCKS;
					    idx_t mismatches = 0;
					    for (idx_t i = 0; i < PINS; i++) {
						    mismatches += cache.Pin(file, block).Ptr()[0] != BenchByte(block);
					    }
					    wrong += mismatches;
				    },
				    "BlockCacheBenchTask"));
			}
			executor.WorkOnTasks();
			const auto elapsed = MillisecondsSince(start);
			REQUIRE(wrong == 0);
			Printer::PrintF("pins of %s, %llu threads: %.1f M pins/s, %.1f ns per pin and unpin in a thread",
			                one_block ? "one block" : "distinct blocks", threads,
			                double(threads * PINS) / elapsed / 1000.0, elapsed * 1000000.0 / double(PINS));
		}
	}
}

TEST_CASE("Block cache benchmark: cold reads of a 16 MiB range from a slow source", "[.][block_cache_bench]") {
	constexpr idx_t BLOCKS = 64;
	DuckDB db(nullptr);
	BlockCache cache(*db.instance, 4 * BLOCKS * BLOCK);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_bench_slow.bin", BLOCKS * BLOCK);
	fs.Throttle(absl::Milliseconds(20), 50.0 * 1024.0 * 1024.0);
	const auto holds_range = [](const vector<BlockCachePin> &pins) {
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
			cache.Prefetch(cached, 0, BLOCKS * BLOCK);
			auto first = cache.Pin(cached, 0);
			const auto first_ms = MillisecondsSince(start);
			auto pins = cache.PinRange(cached, 0, BLOCKS * BLOCK);
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
			auto pins = cache.PinRange(cached, 0, BLOCKS * BLOCK);
			const auto all_ms = MillisecondsSince(start);
			REQUIRE(holds_range(pins));
			Printer::PrintF("16 MiB range pinned from a source of 20 ms and 50 MiB/s per request, round %llu: every "
			                "block in %.1f ms, %llu requests",
			                round, all_ms, idx_t(fs.TakeReads().size()));
		}
	}
}

} // namespace duckdb
