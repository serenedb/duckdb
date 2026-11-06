#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/memory_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <absl/time/clock.h>
#include <chrono>
#include <future>

namespace duckdb {

namespace {

constexpr idx_t BLOCK = BlockCache::BLOCK_SIZE;
constexpr idx_t PIECE_BLOCKS = BlockCache::BlockCountOf(BlockCache::PIECE_SIZE);
constexpr idx_t REQUEST_BLOCKS = BlockCache::BlockCountOf(BlockCache::MAX_REQUEST);

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

	shared_ptr<BlockCacheFile> Open(BlockCache &cache, CountingSourceFileSystem &fs, const string &version = "v1",
	                                bool on_disk = false) const {
		shared_ptr<FileHandle> handle =
		    fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_PARALLEL_ACCESS);
		BlockCacheFileInfo info;
		info.validation.file_size = handle->GetFileSize();
		info.validation.version_tag = version;
		info.on_disk = on_disk;
		return cache.OpenFile(path, info, true, std::move(handle));
	}

	const string path;
};

//! Appends [blocks] blocks to the unbacked [file], block b filled with the byte fill(b)
template <class FILL>
void AppendBlocks(const shared_ptr<BlockCacheFile> &file, idx_t blocks, FILL &&fill) {
	BlockCacheAppender appender(file);
	vector<data_t> bytes(BLOCK);
	for (idx_t block = 0; block < blocks; block++) {
		memset(bytes.data(), int(fill(block)), BLOCK);
		appender.Append(bytes.data(), BLOCK);
	}
	appender.Finish();
}

//! Whether [pin] holds the bytes of the file from [offset] on
bool HoldsExpectedBytesAt(const BlockCachePin &pin, idx_t offset) {
	for (idx_t i = 0; i < pin.Size(); i++) {
		if (pin.Ptr()[i] != ExpectedByte(offset + i)) {
			return false;
		}
	}
	return true;
}

bool HoldsExpectedBytes(const BlockCachePin &pin, idx_t block) {
	return HoldsExpectedBytesAt(pin, block * BLOCK);
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

TEST_CASE("Block cache pins hold the bytes of their block", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_pins.bin", 5 * BLOCK + 1000);
	auto cached = file.Open(cache, fs);

	REQUIRE(BlockCache::BlockCountOf(cached->GetSize()) == 6);
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

TEST_CASE("Block cache pins a byte range in one piece", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_pin_bytes.bin", 5 * BLOCK + 1000);
	auto cached = file.Open(cache, fs);

	const auto holds = [](const BlockCachePin &pin, idx_t offset) {
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

TEST_CASE("Block cache pins share their bytes and keep them after the cache drops their block", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_shared_pins.bin", 8 * BLOCK);
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

TEST_CASE("Block cache reopens of an unchanged file share its blocks", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_reopen.bin", 3 * BLOCK);
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

TEST_CASE("Block cache fetches a missing block once for concurrent readers", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
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
			    auto pin = cache.Pin(*cached, 2);
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
	constexpr idx_t RANGE = REQUEST_BLOCKS + 2 * PIECE_BLOCKS + PIECE_BLOCKS / 4;
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_prefetch.bin", 2 * RANGE * BLOCK);
	auto cached = file.Open(cache, fs);

	for (idx_t block : {10, 11, 30}) {
		cache.Pin(*cached, block);
	}
	fs.TakeReads();

	cache.Prefetch(cached, 0, RANGE * BLOCK, BlockCachePrefetch::HINT);
	for (idx_t block = 0; block < RANGE; block++) {
		auto pin = cache.Pin(*cached, block);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	auto reads = fs.TakeReads();
	idx_t covered = 0;
	for (auto &read : reads) {
		REQUIRE(read.second <= BlockCache::PIECE_SIZE);
		REQUIRE(read.first % BLOCK == 0);
		covered += read.second;
	}
	REQUIRE(reads.size() == REQUEST_BLOCKS / PIECE_BLOCKS + 3);
	REQUIRE(covered == RANGE * BLOCK);
}

TEST_CASE("Block cache prefetch of several ranges reads the ones within the merge gap of each other at once",
          "[block_cache]") {
	constexpr idx_t GAP_BLOCKS = BlockCache::MERGE_GAP / BLOCK;
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_prefetch_ranges.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	BlockCacheRange ranges[] = {{40 * BLOCK, BLOCK}, {(1 + GAP_BLOCKS) * BLOCK, BLOCK}, {BLOCK + 5, 10}};
	cache.Prefetch(cached, ranges, BlockCachePrefetch::NEED);
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

TEST_CASE("Block cache evicts the blocks a reader marked unused before the blocks in use", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_unused.bin", 8 * BLOCK);
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

TEST_CASE("Block cache drops a hinted prefetch past the bytes a file has in flight but never a needed one",
          "[block_cache]") {
	constexpr idx_t in_flight = BlockCache::BlockCountOf(BlockCache::MAX_FILE_IN_FLIGHT);
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_need.bin", (in_flight + 44) * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	cache.Prefetch(cached, 0, in_flight * BLOCK, BlockCachePrefetch::HINT);
	cache.Prefetch(cached, in_flight * BLOCK, 4 * BLOCK, BlockCachePrefetch::HINT);
	const bool hint_dropped = !cache.IsCached(*cached, in_flight * BLOCK, 4 * BLOCK);
	cache.Prefetch(cached, in_flight * BLOCK, 4 * BLOCK, BlockCachePrefetch::NEED);
	const bool need_started = cache.IsCached(*cached, in_flight * BLOCK, 4 * BLOCK);
	fs.Hold(false);
	REQUIRE(hint_dropped);
	REQUIRE(need_started);
	for (idx_t block = 0; block < in_flight + 4; block++) {
		REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, block), block));
	}
}

TEST_CASE("Block cache reads ahead of a reader that pins blocks in order", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_read_ahead.bin", 200 * BLOCK);
	auto cached = file.Open(cache, fs);

	BlockCacheReadAhead in_order;
	for (idx_t block = 0; block < 3; block++) {
		auto pin = cache.PinInOrder(cached, block, in_order, true);
		REQUIRE(HoldsExpectedBytes(pin, block));
	}
	REQUIRE(cache.IsCached(*cached, 3 * BLOCK, 5 * BLOCK));
	REQUIRE_FALSE(cache.IsCached(*cached, 8 * BLOCK, BLOCK));

	auto jump = cache.PinInOrder(cached, 100, in_order, true);
	REQUIRE(HoldsExpectedBytes(jump, 100));
	REQUIRE_FALSE(cache.IsCached(*cached, 101 * BLOCK, BLOCK));

	BlockCacheReadAhead held_back;
	cache.PinInOrder(cached, 50, held_back, false);
	cache.PinInOrder(cached, 51, held_back, false);
	REQUIRE_FALSE(cache.IsCached(*cached, 52 * BLOCK, BLOCK));
	cache.PinInOrder(cached, 52, held_back, true);
	REQUIRE(cache.IsCached(*cached, 53 * BLOCK, 2 * BLOCK));

	BlockCacheReadAhead to_end;
	cache.PinInOrder(cached, 197, to_end, true);
	cache.PinInOrder(cached, 198, to_end, true);
	REQUIRE(cache.IsCached(*cached, 199 * BLOCK, BLOCK));
}

TEST_CASE("Block cache fetches a long run in concurrent pieces and hands out each piece's blocks as it lands",
          "[block_cache]") {
	DBConfig config;
	config.options.maximum_threads = 4;
	constexpr idx_t RUN = 4 * PIECE_BLOCKS;
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_pieces.bin", RUN * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Hold(true);
	cache.Prefetch(cached, 0, RUN * BLOCK, BlockCachePrefetch::HINT);
	const bool concurrent = fs.AwaitReadsFor(4, absl::Seconds(10));
	fs.ReleaseRead(0);
	atomic<bool> right {false};
	Signal pinned;
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
	    executor,
	    [&]() {
		    absl::Cleanup done = [&]() {
			    pinned.Set();
		    };
		    right = HoldsExpectedBytes(cache.Pin(*cached, 0), 0);
	    },
	    "BlockCacheTestTask"));
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
		REQUIRE(read.second <= BlockCache::PIECE_SIZE);
		covered += read.second;
	}
	REQUIRE(covered == RUN * BLOCK);
}

TEST_CASE("Block cache range pins fetch the runs and pieces of a range concurrently", "[block_cache]") {
	DBConfig config;
	config.options.maximum_threads = 4;
	constexpr idx_t FULL = 3 * PIECE_BLOCKS;
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_concurrent_runs.bin", FULL * BLOCK + 100);
	auto cached = file.Open(cache, fs);
	for (idx_t block = PIECE_BLOCKS / 2; block < PIECE_BLOCKS; block++) {
		cache.Pin(*cached, block);
	}
	fs.TakeReads();

	fs.Hold(true);
	vector<BlockCachePin> pins;
	TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
	executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
	    executor, [&]() { pins = cache.PinRange(*cached, 0, FULL * BLOCK + 100); }, "BlockCacheTestTask"));
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
		REQUIRE(read.second <= BlockCache::PIECE_SIZE);
		covered += read.second;
	}
	REQUIRE(covered == (FULL - PIECE_BLOCKS / 2) * BLOCK + 100);
}

TEST_CASE("Block cache fetches the pieces of long runs when the scheduler has no workers", "[block_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	constexpr idx_t RUN = 2 * PIECE_BLOCKS + PIECE_BLOCKS / 2;
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_no_workers_pieces.bin", RUN * BLOCK);
	{
		auto cached = file.Open(cache, fs);
		cache.Prefetch(cached, 0, RUN * BLOCK, BlockCachePrefetch::HINT);
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

TEST_CASE("Block cache range pins fetch the missing blocks of a range in one request", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_range.bin", 20 * BLOCK + 10);
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

TEST_CASE("Block cache range read uses a response that forbids reuse once, without workers", "[block_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_no_store.bin", 8 * BLOCK);
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

TEST_CASE("Block cache readers of prefetched blocks run the fetches when the scheduler has no workers",
          "[block_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_no_workers.bin", 16 * BLOCK);
	auto cached = file.Open(cache, fs);

	cache.Prefetch(cached, 0, 8 * BLOCK, BlockCachePrefetch::HINT);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 3), 3));
	cache.Prefetch(cached, 8 * BLOCK, 4 * BLOCK, BlockCachePrefetch::HINT);
	cache.Prefetch(cached, 12 * BLOCK, 4 * BLOCK, BlockCachePrefetch::HINT);
	REQUIRE(HoldsExpectedBytes(cache.Pin(*cached, 14), 14));
	auto pins = cache.PinRange(*cached, 0, 16 * BLOCK);
	REQUIRE(pins.size() == 16);
	for (idx_t block = 0; block < 16; block++) {
		REQUIRE(HoldsExpectedBytes(pins[block], block));
	}
	REQUIRE(fs.ReadCount() == 3);
}

TEST_CASE("Block cache drops pinned blocks of a forgotten file with their last pin", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_forget.bin", 4 * BLOCK);
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
	BlockCacheFileInfo info;
	REQUIRE_FALSE(cache.TryGetFile(file.path, info));
}

namespace {

void WriteBlocks(BlockCacheAppender &appender, idx_t size) {
	vector<data_t> bytes(size);
	for (idx_t i = 0; i < size; i++) {
		bytes[i] = ExpectedByte(i);
	}
	appender.Append(bytes.data(), size);
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
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_written.bin", 2 * BLOCK + 100);
	{
		BlockCacheAppender appender(cache.CreateWrittenFile(file.path));
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

TEST_CASE("Block cache drops the blocks of a written file that is abandoned", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_abandoned.bin", 2 * BLOCK);
	{
		BlockCacheAppender appender(cache.CreateWrittenFile(file.path));
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

TEST_CASE("Block cache appender publishes a written file at the size appended, or drops it", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_appended.bin", 2 * BLOCK + 100);
	vector<uint8_t> data(2 * BLOCK + 100);
	for (idx_t i = 0; i < data.size(); i++) {
		data[i] = ExpectedByte(i);
	}
	{
		BlockCacheAppender appender(cache.CreateWrittenFile(file.path));
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

	BlockCacheAppender abandoned(cache.CreateWrittenFile(file.path));
	abandoned.Append(data.data(), BLOCK + 1);
	REQUIRE(cache.GetStatistics().used == 2 * BLOCK);
	abandoned.Abandon();
	REQUIRE(cache.GetStatistics().used == 0);
}

TEST_CASE("Block cache knows a published written file as the version its writer names", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	const string path = "block_cache_versioned.bin";
	const auto modified = Timestamp::FromEpochSeconds(1700000000);
	{
		BlockCacheAppender appender(cache.CreateWrittenFile(path));
		vector<uint8_t> data(BLOCK + 10, 7);
		appender.Append(data.data(), data.size());
		appender.Finish("\"etag-1\"", modified);
	}
	BlockCacheFileInfo found;
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
		return LocalFileSystem::TryRemoveFile(Local(filename), opener);
	}

	atomic<idx_t> versioned_opens {0};

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

//! Publishes [path] as a file the block cache knows at version "v1"
void PublishVersion(BlockCache &cache, const string &path, idx_t size) {
	BlockCacheAppender appender(cache.CreateWrittenFile(path));
	WriteBlocks(appender, size);
	appender.Finish("v1", Timestamp::FromEpochSeconds(1700000000));
}

} // namespace

TEST_CASE("The database filesystem reads remote files through the block cache and forgets the ones it changes",
          "[block_cache]") {
	DuckDB db(nullptr);
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	auto owned = make_uniq<RemoteTestFileSystem>();
	auto &remote = *owned;
	fs.RegisterSubSystem(std::move(owned));
	auto &cache = BlockCache::Get(*db.instance);
	TestFile file("database_fs_remote.bin", 2 * BLOCK + 10);
	const string path = RemoteTestFileSystem::PREFIX + file.path;
	BlockCacheFileInfo info;

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

	REQUIRE_FALSE(fs.OpenBlockFile(file.path));
	const string memory = string(MemoryFileSystem::PREFIX) + "database_fs.bin";
	auto writer = fs.OpenFile(memory, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	uint8_t byte = 1;
	writer->Write(QueryContext(), &byte, 1, 0);
	writer->Close();
	REQUIRE(fs.OpenBlockFile(memory));
}

TEST_CASE("Block cache forgets files no one opens and holds no block of", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile with_block("block_cache_sweep_cached.bin", 2 * BLOCK);
	TestFile kept_open("block_cache_sweep_open.bin", 2 * BLOCK);
	cache.Pin(*with_block.Open(cache, fs), 0);
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
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_disable.bin", 4 * BLOCK);
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

TEST_CASE("Block cache never lets the buffer pool evict a pinned block", "[block_cache]") {
	DBConfig config;
	LimitMemory(config, 16, "");
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_pinned.bin", 64 * BLOCK);
	auto cached = file.Open(cache, fs);

	vector<BlockCachePin> pins;
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

TEST_CASE("Block cache zeroes a fetched last block past the end of its file", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile partial("block_cache_partial.bin", BLOCK + 1000);
	const auto zeroed_past_end = [](const BlockCachePin &pin) {
		for (idx_t i = pin.Size(); i < BLOCK; i++) {
			if (pin.Ptr()[i] != 0) {
				return false;
			}
		}
		return true;
	};

	auto cached = partial.Open(cache, fs);
	auto single = cache.Pin(*cached, 1);
	REQUIRE(single.Size() == 1000);
	REQUIRE(HoldsExpectedBytes(single, 1));
	REQUIRE(zeroed_past_end(single));
	single.Reset();
	cached.reset();
	cache.ForgetFile(partial.path);

	cached = partial.Open(cache, fs);
	auto pins = cache.PinRange(*cached, 0, BLOCK + 1000);
	REQUIRE(pins.size() == 2);
	REQUIRE(HoldsExpectedBytes(pins[1], 1));
	REQUIRE(zeroed_past_end(pins[1]));
}

TEST_CASE("Block cache pins fail with the read error and later pins fetch again", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_failure.bin", 4 * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Fail(1);
	REQUIRE_THROWS_WITH(cache.Pin(*cached, 1), Catch::Contains("injected read failure"));
	auto pin = cache.Pin(*cached, 1);
	REQUIRE(HoldsExpectedBytes(pin, 1));

	fs.Fail(1);
	cache.Prefetch(cached, 2 * BLOCK, BLOCK, BlockCachePrefetch::HINT);
	auto prefetched = cache.Pin(*cached, 2);
	REQUIRE(HoldsExpectedBytes(prefetched, 2));
}

TEST_CASE("Block cache gives back the blocks of a fetch piece that never ran after another piece failed",
          "[block_cache]") {
	DBConfig config;
	config.options.maximum_threads = 1;
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	constexpr idx_t PIECE_BLOCKS = BlockCache::PIECE_SIZE / BLOCK;
	TestFile file("block_cache_cancelled_piece.bin", 2 * PIECE_BLOCKS * BLOCK);
	auto cached = file.Open(cache, fs);

	fs.Fail(1);
	REQUIRE_THROWS_WITH(cache.PinRange(*cached, 0, 2 * PIECE_BLOCKS * BLOCK), Catch::Contains("injected read failure"));
	REQUIRE(!cache.IsCached(*cached, PIECE_BLOCKS * BLOCK, BLOCK));
	auto pin = cache.Pin(*cached, PIECE_BLOCKS);
	REQUIRE(HoldsExpectedBytes(pin, PIECE_BLOCKS));
}

TEST_CASE("Block cache keeps evicted blocks of remote files in temporary files and reads them back", "[block_cache]") {
	const auto directory = TestCreatePath("block_cache_remote_copies");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_remote_copies.bin", 32 * BLOCK);
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

TEST_CASE("Block cache drops evicted blocks of remote files when external_file_cache_spill_size is 0",
          "[block_cache]") {
	const auto directory = TestCreatePath("block_cache_no_copies");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_spill_size = 0"));
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_no_copies.bin", 32 * BLOCK);
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

TEST_CASE("Block cache fetches evicted blocks again when the temporary files have no room or the file is local",
          "[block_cache]") {
	for (const bool local : {false, true}) {
		const auto directory = TestCreatePath("block_cache_dropped");
		DBConfig config;
		LimitMemory(config, 8, directory);
		DuckDB db(nullptr, &config);
		if (!local) {
			BufferManager::GetBufferManager(*db.instance).SetSwapLimit(2 * BLOCK);
		}
		BlockCache cache(*db.instance);
		CountingSourceFileSystem fs;
		TestFile file("block_cache_dropped.bin", 32 * BLOCK);
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

TEST_CASE("Block cache blocks of in-memory files spill to temporary files and are never dropped", "[block_cache]") {
	const auto directory = TestCreatePath("block_cache_unbacked_spill");
	DBConfig config;
	LimitMemory(config, 8, directory);
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	auto file = cache.CreateUnbackedFile("memory://unbacked.bin");
	AppendBlocks(file, 32, [](idx_t block) { return block + 1; });
	REQUIRE(cache.GetStatistics().spilled > 0);
	REQUIRE(CountFiles(directory, ".block") == 0);
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

TEST_CASE("Block cache keeps external_file_cache_min_memory of remote blocks while operator blocks can spill",
          "[block_cache]") {
	DBConfig config;
	LimitMemory(config, 64, TestCreatePath("block_cache_min_memory"));
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET external_file_cache_min_memory = '256KiB'"));
	auto setting = con.Query("SELECT current_setting('external_file_cache_min_memory')");
	REQUIRE(CHECK_COLUMN(setting, 0, {"256.0 KiB"}));
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile remote_file("block_cache_min_memory_remote.bin", 6 * BLOCK);
	TestFile local_file("block_cache_min_memory_local.bin", BLOCK);
	auto remote = remote_file.Open(cache, fs);
	auto local = local_file.Open(cache, fs, "v1", true);
	auto &buffer_manager = BufferManager::GetBufferManager(*db.instance);
	auto &pool = db.instance->GetBufferPool();
	buffer_manager.SetMemoryLimit(pool.GetUsedMemory() + 8 * BLOCK);
	const auto loaded = [&](const shared_ptr<BlockCacheFile> &file, idx_t blocks) {
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
ReadWhileParked ReadWhileParkedAt(BlockCache &cache, const shared_ptr<BlockCacheFile> &file, idx_t block,
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

TEST_CASE("Block cache reads an in-memory block while an eviction writes it to a temporary file", "[block_cache]") {
	const auto directory = TestCreatePath("block_cache_unload_window");
	DBConfig config;
	LimitMemory(config, 4, directory);
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
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

TEST_CASE("Block cache reads an in-memory block while another reader loads it back from its temporary file",
          "[block_cache]") {
	const auto directory = TestCreatePath("block_cache_load_window");
	DBConfig config;
	LimitMemory(config, 16, directory);
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
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

TEST_CASE("Block cache gives memory back to the buffer pool", "[block_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='64MiB'"));
	auto &cache = BlockCache::Get(*db.instance);
	constexpr idx_t FULL = BlockCache::BlockCountOf(16 * 1024 * 1024);
	CountingSourceFileSystem fs;
	TestFile file("block_cache_shrink.bin", FULL * BLOCK);
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

TEST_CASE("Block cache gives the memory of a dropped in-memory file back at once", "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	for (idx_t round = 0; round < 2; round++) {
		auto file = cache.CreateUnbackedFile("memory://dropped.bin");
		AppendBlocks(file, 32, [](idx_t block) { return block + 1; });
		REQUIRE(cache.GetStatistics().used == 32 * BLOCK);
		file.reset();
		REQUIRE(cache.GetStatistics().used == 0);
	}
}

TEST_CASE("Block cache concurrent pins and evictions return the right bytes", "[block_cache]") {
	constexpr idx_t BLOCKS = 1024;
	for (const bool copies : {false, true}) {
		DBConfig config;
		LimitMemory(config, 256, copies ? TestCreatePath("block_cache_stress") : "");
		DuckDB db(nullptr, &config);
		BlockCache cache(*db.instance);
		CountingSourceFileSystem fs;
		TestFile file("block_cache_stress.bin", BLOCKS * BLOCK);
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
						    const auto block = (seed >> 33) % BLOCKS;
						    if (i % 3 == 0) {
							    auto pins = cache.TryPinRange(*cached, block * BLOCK, BLOCK);
							    if (!pins.empty() && !HoldsExpectedBytes(pins[0], block)) {
								    wrong++;
							    }
							    continue;
						    }
						    if (i % 7 == 0) {
							    cache.Prefetch(cached, block * BLOCK, 4 * BLOCK, BlockCachePrefetch::HINT);
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
			    "BlockCacheTestTask"));
		}
		executor.WorkOnTasks();
		REQUIRE(wrong == 0);
	}
}

TEST_CASE("Block cache counts every hit of concurrent pins of one block and lets the block go after them",
          "[block_cache]") {
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
	CountingSourceFileSystem fs;
	TestFile hot("block_cache_hot.bin", BLOCK);
	auto file = hot.Open(cache, fs);
	cache.Pin(*file, 0);
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
				    auto pin = cache.Pin(*file, 0);
				    if (pin.Ptr()[0] != ExpectedByte(0) || pin.Ptr()[BLOCK - 1] != ExpectedByte(BLOCK - 1)) {
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
	cache.ForgetFile(hot.path);
	REQUIRE(cache.GetStatistics().used == 0);
}

TEST_CASE("Block cache pins of an in-memory file return the right bytes while it grows and spills", "[block_cache]") {
	DBConfig config;
	LimitMemory(config, 16, TestCreatePath("block_cache_growing_spill"));
	DuckDB db(nullptr, &config);
	BlockCache cache(*db.instance);
	auto file = cache.CreateUnbackedFile("memory://growing.bin");
	constexpr idx_t BLOCKS = 256;
	constexpr idx_t RECENT = 6;
	const auto fill = [](idx_t block) {
		return static_cast<uint8_t>(block % 251 + 1);
	};
	atomic<idx_t> created {0};
	BlockCacheAppender appender(file);
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
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor,
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
		    "BlockCacheTestTask"));
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

TEST_CASE("Instances sharing a temporary directory spill in-memory blocks to files of their own", "[block_cache]") {
	const auto directory = TestCreatePath("block_cache_shared_spill");
	constexpr idx_t FIT = 8;
	{
		DBConfig first_config;
		LimitMemory(first_config, FIT, directory);
		DBConfig second_config;
		LimitMemory(second_config, FIT, directory);
		DuckDB first(nullptr, &first_config);
		DuckDB second(nullptr, &second_config);
		vector<shared_ptr<BlockCacheFile>> files;
		for (auto db : {&first, &second}) {
			auto &cache = BlockCache::Get(*db->instance);
			auto file = cache.CreateUnbackedFile("memory://shared_spill.bin");
			const auto fill = files.size() * 100 + 1;
			AppendBlocks(file, 4 * FIT, [&](idx_t block) { return fill + block; });
			REQUIRE(BufferManager::GetBufferManager(*db->instance).GetUsedSwap() > 0);
			files.push_back(std::move(file));
		}
		REQUIRE(CountFiles(directory, ".tmp") >= 2);
		for (idx_t i = 0; i < files.size(); i++) {
			auto &cache = BlockCache::Get(*(i == 0 ? first : second).instance);
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

TEST_CASE("Block cache benchmark: concurrent reads of blocks evicted to temporary files", "[.][block_cache_bench]") {
	constexpr idx_t MEMORY_BLOCKS = 64;
	constexpr idx_t BLOCKS = 1024;
	constexpr idx_t READS = 256;
	vector<pair<string, string>> directories {{"the test directory", TestCreatePath("block_cache_bench_spill")}};
	LocalFileSystem local;
	if (local.DirectoryExists("/dev/shm")) {
		directories.emplace_back("tmpfs", "/dev/shm/duckdb_block_cache_bench_spill_" + to_string(getpid()));
	}
	for (auto &directory : directories) {
		DBConfig config;
		LimitMemory(config, MEMORY_BLOCKS, directory.second);
		DuckDB db(nullptr, &config);
		BlockCache cache(*db.instance);
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
					executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
					    executor,
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
					    "BlockCacheBenchTask"));
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

TEST_CASE("Block cache benchmark: releasing the blocks of a 512 MiB in-memory file", "[.][block_cache_bench]") {
	constexpr idx_t BLOCKS = 4096;
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
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

TEST_CASE("Block cache benchmark: concurrent pins of one block and of distinct blocks", "[.][block_cache_bench]") {
	constexpr idx_t PINS = 500000;
	constexpr idx_t BLOCKS = 64;
	TestFile remote_file("block_cache_bench_pins.bin", BLOCKS * BLOCK);
	CountingSourceFileSystem fs;
	for (const idx_t threads : {idx_t(1), idx_t(8), idx_t(16), idx_t(32), idx_t(64)}) {
		DBConfig config;
		config.options.maximum_threads = threads;
		DuckDB db(nullptr, &config);
		BlockCache cache(*db.instance);
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
					executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
					    executor,
					    [&, thread]() {
						    const auto block = one_block ? 0 : thread % BLOCKS;
						    const auto expected = in_memory ? BenchByte(block) : ExpectedByte(block * BLOCK);
						    idx_t mismatches = 0;
						    for (idx_t i = 0; i < PINS; i++) {
							    mismatches += cache.Pin(*file, block).Ptr()[0] != expected;
						    }
						    wrong += mismatches;
					    },
					    "BlockCacheBenchTask"));
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

TEST_CASE("Block cache benchmark: concurrent buffer manager pins of one buffer and of distinct buffers",
          "[.][block_cache_bench]") {
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
				executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
				    executor,
				    [&, thread]() {
					    const auto block = one_block ? 0 : thread % BLOCKS;
					    auto handle = handles[block];
					    idx_t mismatches = 0;
					    for (idx_t i = 0; i < PINS; i++) {
						    mismatches += buffers.Pin(handle).Ptr()[0] != BenchByte(block);
					    }
					    wrong += mismatches;
				    },
				    "BufferManagerBenchTask"));
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

TEST_CASE("Block cache benchmark: concurrent buffer manager allocations and frees", "[.][block_cache_bench]") {
	constexpr idx_t ALLOCATIONS = 20000;
	for (const idx_t threads : {idx_t(1), idx_t(8), idx_t(32), idx_t(64)}) {
		DBConfig config;
		config.options.maximum_threads = threads;
		DuckDB db(nullptr, &config);
		auto &buffers = BufferManager::GetBufferManager(*db.instance);
		const auto start = std::chrono::steady_clock::now();
		TaskExecutor executor(TaskScheduler::GetScheduler(*db.instance));
		for (idx_t thread = 0; thread < threads; thread++) {
			executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
			    executor,
			    [&]() {
				    for (idx_t i = 0; i < ALLOCATIONS; i++) {
					    auto buffer = buffers.Allocate(MemoryTag::HASH_TABLE, BLOCK, true);
					    buffer.GetDataMutable()[0] = 1;
				    }
			    },
			    "BufferManagerAllocateBenchTask"));
		}
		executor.WorkOnTasks();
		const auto elapsed = MillisecondsSince(start);
		Printer::PrintF("buffer manager allocations, %llu threads: %.2f M allocations/s, %.1f ns per allocation in a "
		                "thread",
		                threads, double(threads * ALLOCATIONS) / elapsed / 1000.0,
		                elapsed * 1000000.0 / double(ALLOCATIONS));
	}
}

TEST_CASE("Block cache benchmark: cold reads of a 16 MiB range from a slow source", "[.][block_cache_bench]") {
	constexpr idx_t BLOCKS = 64;
	DuckDB db(nullptr);
	BlockCache cache(*db.instance);
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
			cache.Prefetch(cached, 0, BLOCKS * BLOCK, BlockCachePrefetch::HINT);
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
