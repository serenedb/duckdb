#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/in_memory_block_manager.hpp"

#include <absl/cleanup/cleanup.h>
#include <absl/time/clock.h>
#include <chrono>
#include <future>

namespace duckdb {

namespace {

constexpr idx_t PIN_BLOCK_SIZE = Storage::DEFAULT_BLOCK_SIZE;

data_t PinTestByte(idx_t block) {
	return data_t(block * 37 + 11);
}

shared_ptr<BlockHandle> PinTestBlock(BufferManager &buffers, idx_t block) {
	auto buffer = buffers.Allocate(MemoryTag::IN_MEMORY_TABLE, PIN_BLOCK_SIZE, false);
	memset(buffer.GetDataMutable(), PinTestByte(block), PIN_BLOCK_SIZE);
	return buffer.GetBlockHandle();
}

void LoadTestBlock(BufferManager &buffers, shared_ptr<BlockHandle> &handle, data_t byte) {
	auto &memory = handle->GetMemory();
	REQUIRE(memory.TryClaim());
	auto load = buffers.PrepareLoad(memory, PIN_BLOCK_SIZE, Storage::DEFAULT_BLOCK_HEADER_SIZE);
	memset(load.buffer->GetDataMutable(), byte, PIN_BLOCK_SIZE);
	buffers.CompleteLoad(handle, std::move(load));
	memory.FinishLoad();
}

shared_ptr<BlockHandle> PinTestCopyBlock(BufferManager &buffers, idx_t block) {
	auto handle = buffers.RegisterUnloadedMemory(MemoryTag::IN_MEMORY_TABLE, PIN_BLOCK_SIZE,
	                                             Storage::DEFAULT_BLOCK_HEADER_SIZE, DestroyBufferUpon::KEEP_COPY);
	LoadTestBlock(buffers, handle, PinTestByte(block));
	return handle;
}

void RunPinTestTasks(DatabaseInstance &db, idx_t tasks, const std::function<void(idx_t)> &task) {
	TaskExecutor executor(TaskScheduler::GetScheduler(db));
	for (idx_t i = 0; i < tasks; i++) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor, [&task, i]() { task(i); }, "PinTestTask"));
	}
	executor.WorkOnTasks();
}

class PinTestMapping final : public MemoryMappedFile {
public:
	PinTestMapping(data_ptr_t data, idx_t size) : MemoryMappedFile(string(), FileFlags::FILE_FLAGS_READ, data, size) {
	}

	void Sync() override {
	}
	bool Trim(idx_t offset, idx_t length) override {
		return false;
	}
	void Close() override {
	}
};

class PinTestMappedBlocks final : public InMemoryBlockManager {
public:
	PinTestMappedBlocks(DatabaseInstance &db, BufferManager &buffers)
	    : InMemoryBlockManager(buffers, DEFAULT_BLOCK_ALLOC_SIZE, Storage::DEFAULT_BLOCK_HEADER_SIZE),
	      allocator(BlockAllocator::Get(db)), bytes(make_unsafe_uniq_array<data_t>(DEFAULT_BLOCK_ALLOC_SIZE)),
	      mapping(bytes.get(), DEFAULT_BLOCK_ALLOC_SIZE) {
	}

	unique_ptr<Block> CreateBlock(block_id_t block_id, FileBuffer *source_buffer) override {
		return make_uniq<Block>(allocator, block_id, mapping, 0, GetBlockSize(), GetBlockHeaderSize());
	}
	void Read(QueryContext context, Block &block) override {
		block.Read(context, mapping, 0);
	}

private:
	BlockAllocator &allocator;
	unsafe_unique_array<data_t> bytes;
	PinTestMapping mapping;
};

} // namespace

TEST_CASE("Pins of loaded blocks race with their eviction and reload", "[storage][buffer_manager]") {
	constexpr idx_t BLOCKS = 64;
	constexpr idx_t THREADS = 8;
	DBConfig config;
	config.options.maximum_threads = THREADS;
	config.options.temporary_directory = TestCreatePath("buffer_manager_pins_tmp");
	DuckDB db(nullptr, &config);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	vector<shared_ptr<BlockHandle>> blocks;
	for (idx_t block = 0; block < BLOCKS; block++) {
		blocks.push_back(PinTestBlock(buffers, block));
	}
	// Room for a quarter of the blocks, so pins keep evicting blocks to the temporary directory and loading them back.
	buffers.SetMemoryLimit(buffers.GetUsedMemory() - (BLOCKS - BLOCKS / 4) * PIN_BLOCK_SIZE);

	atomic<idx_t> wrong {0};
	RunPinTestTasks(*db.instance, THREADS, [&](idx_t thread) {
		idx_t mismatches = 0;
		for (idx_t i = 0; i < 4000; i++) {
			const auto block = (thread * 13 + i * (i % 3 == 0 ? 1 : 7)) % BLOCKS;
			auto pin = buffers.Pin(blocks[block]);
			mismatches += pin.Ptr()[0] != PinTestByte(block) || pin.Ptr()[PIN_BLOCK_SIZE - 1] != PinTestByte(block);
		}
		wrong += mismatches;
	});
	REQUIRE(wrong == 0);
	for (idx_t block = 0; block < BLOCKS; block++) {
		REQUIRE(blocks[block]->GetMemory().GetReaders() == 0);
	}
}

TEST_CASE("A pinned block outlives its last owner until it is unpinned", "[storage][buffer_manager]") {
	DuckDB db(nullptr);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	auto block = PinTestBlock(buffers, 1);
	const auto used = buffers.GetUsedMemory();
	auto pin = buffers.Pin(block);
	REQUIRE(pin.GetBlockHandle() == block);

	block.reset();
	REQUIRE(!pin.GetBlockHandle());
	REQUIRE(pin.Ptr()[0] == PinTestByte(1));
	REQUIRE(pin.Ptr()[PIN_BLOCK_SIZE - 1] == PinTestByte(1));
	REQUIRE(buffers.GetUsedMemory() == used);

	pin.Destroy();
	REQUIRE(buffers.GetUsedMemory() < used);
}

TEST_CASE("A block destroyed upon unpin since it was pinned frees its memory at the unpin",
          "[storage][buffer_manager]") {
	DuckDB db(nullptr);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	auto block = PinTestBlock(buffers, 2);
	auto pin = buffers.Pin(block);
	const auto used = buffers.GetUsedMemory();
	block->GetMemory().SetDestroyBufferUpon(DestroyBufferUpon::UNPIN);
	pin.Destroy();
	REQUIRE(block->GetMemory().GetState() == BlockState::BLOCK_UNLOADED);
	REQUIRE(buffers.GetUsedMemory() < used);
}

TEST_CASE("A block that keeps a copy is written once while the copies have room, otherwise destroyed",
          "[storage][buffer_manager]") {
	DBConfig config;
	config.options.temporary_directory = TestCreatePath("buffer_manager_room_tmp");
	DuckDB db(nullptr, &config);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	const auto writes = [&]() {
		return buffers.GetMemoryUsageInfo()[uint8_t(MemoryTag::IN_MEMORY_TABLE)].temporary_writes;
	};
	const auto unload = [](BlockMemory &memory) {
		auto lock = memory.GetLock();
		REQUIRE(memory.CanUnload() == CanUnloadResult::CAN_UNLOAD);
		memory.Unload(lock);
	};
	for (const bool room : {true, false}) {
		if (!room) {
			DBConfig::GetConfig(*db.instance).options.external_file_cache_spill_size = PIN_BLOCK_SIZE - 1;
		}
		const auto written = writes();
		auto block = PinTestCopyBlock(buffers, 3);
		auto &memory = block->GetMemory();
		for (idx_t round = 0; round < 3; round++) {
			unload(memory);
			REQUIRE(memory.HasTemporaryFile() == room);
			REQUIRE((buffers.GetUsedSwap() > 0) == room);
			auto pin = buffers.Pin(block);
			REQUIRE(pin.IsValid() == room);
			if (!room) {
				break;
			}
			REQUIRE(pin.Ptr()[0] == PinTestByte(3));
			REQUIRE(pin.Ptr()[PIN_BLOCK_SIZE - 1] == PinTestByte(3));
			REQUIRE(memory.HasTemporaryFile());
		}
		REQUIRE(writes() - written == (room ? 1 : 0));
		block.reset();
		REQUIRE(buffers.GetUsedSwap() == 0);
	}
}

TEST_CASE("A destroyed buffer is loaded anew from outside, and a discarded block loses its buffer and its copy",
          "[storage][buffer_manager]") {
	DBConfig config;
	config.options.temporary_directory = TestCreatePath("buffer_manager_discard_tmp");
	DuckDB db(nullptr, &config);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	auto block = PinTestCopyBlock(buffers, 5);
	auto &memory = block->GetMemory();
	{
		auto lock = memory.GetLock();
		memory.Unload(lock);
	}
	REQUIRE(memory.HasTemporaryFile());
	REQUIRE(buffers.Pin(block).Ptr()[PIN_BLOCK_SIZE - 1] == PinTestByte(5));
	const auto used = buffers.GetUsedMemory();
	buffers.Discard(memory);
	REQUIRE(memory.GetState() == BlockState::BLOCK_UNLOADED);
	REQUIRE(!memory.HasTemporaryFile());
	REQUIRE(buffers.GetUsedSwap() == 0);
	REQUIRE(buffers.GetUsedMemory() < used);
	REQUIRE(!buffers.Pin(block).IsValid());
	REQUIRE(!memory.IsLoading());

	LoadTestBlock(buffers, block, PinTestByte(6));
	REQUIRE(memory.GetState() == BlockState::BLOCK_LOADED);
	REQUIRE(buffers.GetUsedMemory() == used);
	{
		auto again = buffers.Pin(block);
		REQUIRE(again.Ptr()[PIN_BLOCK_SIZE - 1] == PinTestByte(6));
		buffers.Discard(memory);
		REQUIRE(memory.GetState() == BlockState::BLOCK_LOADED);
	}
	REQUIRE(memory.GetState() == BlockState::BLOCK_UNLOADED);
	REQUIRE(buffers.GetUsedMemory() < used);
}

TEST_CASE("Owners dropped while other threads unpin leave no memory behind", "[storage][buffer_manager]") {
	constexpr idx_t ROUNDS = 2000;
	DBConfig config;
	config.options.maximum_threads = 2;
	DuckDB db(nullptr, &config);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	const auto used = buffers.GetUsedMemory();
	atomic<idx_t> wrong {0};
	for (idx_t round = 0; round < ROUNDS; round++) {
		auto block = PinTestBlock(buffers, round);
		auto pin = buffers.Pin(block);
		RunPinTestTasks(*db.instance, 2, [&](idx_t task) {
			if (task == 0) {
				wrong += pin.Ptr()[0] != PinTestByte(round);
				pin.Destroy();
			} else {
				block.reset();
			}
		});
	}
	REQUIRE(wrong == 0);
	REQUIRE(buffers.GetUsedMemory() == used);
}

TEST_CASE("An allocation at the memory limit evicts memory, not a block that does not own its buffer",
          "[storage][buffer_manager]") {
	DuckDB db(nullptr);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	PinTestMappedBlocks blocks(*db.instance, buffers);
	auto mapped = blocks.RegisterBlock(0);
	mapped->GetMemory().SetMemoryUsage(0);
	{
		auto pin = buffers.Pin(mapped);
		REQUIRE(pin.IsValid());
	}
	auto evictable = buffers.Allocate(MemoryTag::IN_MEMORY_TABLE, PIN_BLOCK_SIZE, true).GetBlockHandle();
	buffers.SetMemoryLimit(buffers.GetUsedMemory());
	auto pin = buffers.Allocate(MemoryTag::IN_MEMORY_TABLE, PIN_BLOCK_SIZE, false);
	REQUIRE(buffers.GetUsedMemory() <= buffers.GetMaxMemory());
	REQUIRE(evictable->GetMemory().IsUnloaded());
}

TEST_CASE("Many pins of blocks that keep being unloaded wait for each other's loads", "[storage][buffer_manager]") {
	constexpr idx_t BLOCKS = 2;
	constexpr idx_t THREADS = 32;
	DBConfig config;
	config.options.maximum_threads = THREADS;
	config.options.temporary_directory = TestCreatePath("buffer_manager_waiters_tmp");
	DuckDB db(nullptr, &config);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	vector<shared_ptr<BlockHandle>> blocks;
	for (idx_t block = 0; block < BLOCKS; block++) {
		blocks.push_back(PinTestBlock(buffers, block));
	}
	atomic<bool> done {false};
	std::thread evictor([&]() {
		for (idx_t i = 0; !done; i++) {
			auto &memory = blocks[i % BLOCKS]->GetMemory();
			auto lock = memory.GetLock();
			if (memory.CanUnload() == CanUnloadResult::CAN_UNLOAD) {
				memory.Unload(lock);
			}
		}
	});
	atomic<idx_t> wrong {0};
	RunPinTestTasks(*db.instance, THREADS, [&](idx_t thread) {
		for (idx_t i = 0; i < 2000; i++) {
			const auto block = (thread + i) % BLOCKS;
			auto pin = buffers.Pin(blocks[block]);
			wrong += pin.Ptr()[0] != PinTestByte(block);
		}
	});
	done = true;
	evictor.join();
	REQUIRE(wrong == 0);
}

#ifdef SDB_FAULT_INJECTION
TEST_CASE("Pins of a block another pin is loading wait for that load", "[storage][buffer_manager]") {
	DBConfig config;
	config.options.temporary_directory = TestCreatePath("buffer_manager_wait_tmp");
	DuckDB db(nullptr, &config);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	auto block = PinTestBlock(buffers, 7);
	auto &memory = block->GetMemory();
	{
		auto lock = memory.GetLock();
		memory.Unload(lock);
	}
	const auto reads = [&]() {
		return buffers.GetMemoryUsageInfo()[uint8_t(MemoryTag::IN_MEMORY_TABLE)].temporary_reads;
	};
	const auto before = reads();
	absl::Cleanup clear = [] {
		FaultPoints::Clear();
	};
	REQUIRE(FaultPoints::Arm("block_load_before_loaded"));
	auto loader = std::async(std::launch::async, [&] { return buffers.Pin(block).Ptr()[0]; });
	while (!FaultPoints::Armed("block_load_before_loaded:parked")) {
		absl::SleepFor(absl::Milliseconds(1));
	}
	auto waiter = std::async(std::launch::async, [&] { return buffers.Pin(block).Ptr()[0]; });
	REQUIRE(waiter.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);
	REQUIRE(memory.IsLoading());
	REQUIRE(FaultPoints::Arm("block_load_before_loaded:release"));
	REQUIRE(loader.get() == PinTestByte(7));
	REQUIRE(waiter.get() == PinTestByte(7));
	REQUIRE(reads() - before == 1);
	REQUIRE(!memory.IsLoading());
}
#endif

} // namespace duckdb
