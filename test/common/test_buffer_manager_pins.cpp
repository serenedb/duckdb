#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"

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

void RunPinTestTasks(DatabaseInstance &db, idx_t tasks, const std::function<void(idx_t)> &task) {
	TaskExecutor executor(TaskScheduler::GetScheduler(db));
	for (idx_t i = 0; i < tasks; i++) {
		executor.ScheduleTask(make_uniq<FunctionExecutorTask>(
		    executor, [&task, i]() { task(i); }, "PinTestTask"));
	}
	executor.WorkOnTasks();
}

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
		auto block = PinTestBlock(buffers, 3);
		auto &memory = block->GetMemory();
		{
			auto lock = memory.GetLock();
			memory.SetDestroyBufferUpon(lock, DestroyBufferUpon::KEEP_COPY);
		}
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

TEST_CASE("A destroyed buffer is pinned anew empty, and a discarded block loses its buffer and its copy",
          "[storage][buffer_manager]") {
	DBConfig config;
	config.options.temporary_directory = TestCreatePath("buffer_manager_discard_tmp");
	DuckDB db(nullptr, &config);
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	auto block = PinTestBlock(buffers, 5);
	auto &memory = block->GetMemory();
	{
		auto lock = memory.GetLock();
		memory.SetDestroyBufferUpon(lock, DestroyBufferUpon::KEEP_COPY);
		memory.Unload(lock);
	}
	REQUIRE(memory.HasTemporaryFile());
	REQUIRE(!buffers.PinEmpty(block, PIN_BLOCK_SIZE, Storage::DEFAULT_BLOCK_HEADER_SIZE).IsValid());
	REQUIRE(buffers.Pin(block).Ptr()[PIN_BLOCK_SIZE - 1] == PinTestByte(5));
	const auto used = buffers.GetUsedMemory();
	buffers.Discard(block);
	REQUIRE(memory.GetState() == BlockState::BLOCK_UNLOADED);
	REQUIRE(!memory.HasTemporaryFile());
	REQUIRE(buffers.GetUsedSwap() == 0);
	REQUIRE(buffers.GetUsedMemory() < used);
	REQUIRE(!buffers.Pin(block).IsValid());

	auto pin = buffers.PinEmpty(block, PIN_BLOCK_SIZE, Storage::DEFAULT_BLOCK_HEADER_SIZE);
	REQUIRE(memory.GetState() == BlockState::BLOCK_LOADED);
	REQUIRE(buffers.GetUsedMemory() == used);
	memset(pin.GetDataMutable(), PinTestByte(6), PIN_BLOCK_SIZE);
	pin.Destroy();
	{
		auto again = buffers.Pin(block);
		REQUIRE(again.Ptr()[PIN_BLOCK_SIZE - 1] == PinTestByte(6));
		buffers.Discard(block);
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

} // namespace duckdb
