#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

namespace {

constexpr idx_t PIN_BLOCK_SIZE = 256 * 1024;

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
