#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/mutex.hpp"

#include <algorithm>
#include <cstdlib>
#include <thread>

using namespace duckdb;

TEST_CASE("BlockAllocator usage and de-allocation on different threads", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 4096;
	constexpr idx_t VIRTUAL_MEM_SIZE = 256 * 1024 * 1024;
	constexpr idx_t PHYSICAL_MEM_SIZE = 256 * 1024 * 1024;

	std::thread worker;
	mutex mtx;
	bool alloc_done = false;
	bool allocator_destroyed = false;

	// Thread-A, where we construct BlockAllocator.
	{
		Allocator alloc;
		BlockAllocator ba(alloc, BLOCK_SIZE, VIRTUAL_MEM_SIZE, PHYSICAL_MEM_SIZE);

		// Thread-B, where we allocate blocks and creates thread-local state.
		worker = std::thread([&]() {
			constexpr int NUM_BLOCKS = 16;
			data_ptr_t blocks[NUM_BLOCKS];
			for (int i = 0; i < NUM_BLOCKS; i++) {
				blocks[i] = ba.AllocateData(BLOCK_SIZE);
				REQUIRE(blocks[i] != nullptr);
			}
			for (int i = 0; i < NUM_BLOCKS; i++) {
				ba.FreeData(blocks[i], BLOCK_SIZE);
			}

			{
				lock_guard<mutex> lk(mtx);
				alloc_done = true;
			}

			{
				lock_guard<mutex> lk(mtx);
				mtx.Await(absl::Condition(&allocator_destroyed));
			}
		});

		{
			lock_guard<mutex> lk(mtx);
			mtx.Await(absl::Condition(&alloc_done));
		}
	}
	// BlockAllocator destructs here, which clears thread-local allocation state in thread-B, instead of the one in
	// thread-A, which records free blocks.

	// Destroy thread-B and thread-local state, where allocation happens.
	{
		lock_guard<mutex> lk(mtx);
		allocator_destroyed = true;
	}
	worker.join();
}

TEST_CASE("BlockAllocator serves a second block size from a pool of its own", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 256 * 1024;
	constexpr idx_t POOL_BLOCK_SIZE = 128 * 1024;
	constexpr idx_t MEMORY_SIZE = 64 * 1024 * 1024;
	constexpr idx_t BLOCKS = 32;
	Allocator alloc;
	BlockAllocator ba(alloc, BLOCK_SIZE, MEMORY_SIZE, MEMORY_SIZE);
	ba.AddHugePagePool(POOL_BLOCK_SIZE, MEMORY_SIZE);

	vector<data_ptr_t> pool_blocks;
	vector<data_ptr_t> blocks;
	for (idx_t i = 0; i < BLOCKS; i++) {
		pool_blocks.push_back(ba.AllocateData(POOL_BLOCK_SIZE));
		memset(pool_blocks.back(), int(i), POOL_BLOCK_SIZE);
		blocks.push_back(ba.AllocateData(BLOCK_SIZE));
		memset(blocks.back(), int(i + 1), BLOCK_SIZE);
	}
	const auto lowest = *std::min_element(pool_blocks.begin(), pool_blocks.end());
	const auto highest = *std::max_element(pool_blocks.begin(), pool_blocks.end());
	REQUIRE(idx_t(highest - lowest) < MEMORY_SIZE);
	for (idx_t i = 0; i < BLOCKS; i++) {
		REQUIRE(idx_t(pool_blocks[i] - lowest) % POOL_BLOCK_SIZE == 0);
		REQUIRE(pool_blocks[i][POOL_BLOCK_SIZE - 1] == data_t(i));
		REQUIRE(blocks[i][BLOCK_SIZE - 1] == data_t(i + 1));
	}

	auto moved = ba.ReallocateData(pool_blocks[0], POOL_BLOCK_SIZE, POOL_BLOCK_SIZE / 2);
	REQUIRE(moved[POOL_BLOCK_SIZE / 2 - 1] == 0);
	moved = ba.ReallocateData(moved, POOL_BLOCK_SIZE / 2, POOL_BLOCK_SIZE);
	REQUIRE(idx_t(moved - lowest) % POOL_BLOCK_SIZE == 0);
	REQUIRE(moved[0] == 0);
	pool_blocks[0] = moved;

	for (idx_t i = 0; i < BLOCKS; i++) {
		ba.FreeData(pool_blocks[i], POOL_BLOCK_SIZE);
		ba.FreeData(blocks[i], BLOCK_SIZE);
	}
	ba.FlushAll(MEMORY_SIZE);
	for (idx_t i = 0; i < BLOCKS; i++) {
		pool_blocks[i] = ba.AllocateData(POOL_BLOCK_SIZE);
		REQUIRE(idx_t(std::abs(pool_blocks[i] - lowest)) < MEMORY_SIZE);
	}
	for (auto block : pool_blocks) {
		ba.FreeData(block, POOL_BLOCK_SIZE);
	}
}
