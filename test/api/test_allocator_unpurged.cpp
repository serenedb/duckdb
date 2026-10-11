#include "catch.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/storage/block_allocator.hpp"

#include <cstring>

using namespace duckdb;

TEST_CASE("Freed memory counts toward the allocator's unpurged bytes until a flush", "[api][allocator]") {
	if (!Allocator::UnpurgedBytes().IsValid()) {
		return;
	}
	constexpr idx_t SIZE = 32ULL * 1024ULL * 1024ULL;
	auto &allocator = Allocator::DefaultAllocator();
	Allocator::FlushAll();
	const auto before = Allocator::UnpurgedBytes().GetIndex();
	auto data = allocator.AllocateData(SIZE);
	memset(data, 1, SIZE);
	allocator.FreeData(data, SIZE);
	const auto freed = Allocator::UnpurgedBytes().GetIndex();
	REQUIRE(freed >= before + SIZE / 2);
	Allocator::FlushAll();
	REQUIRE(Allocator::UnpurgedBytes().GetIndex() < freed);
}

TEST_CASE("A claim takes the freed count only past the threshold", "[api][block_allocator]") {
	constexpr idx_t BLOCK_SIZE = 4096;
	constexpr idx_t MEM_SIZE = 16 * 1024 * 1024;
	Allocator alloc;
	BlockAllocator block_allocator(alloc, BLOCK_SIZE, MEM_SIZE, MEM_SIZE);
	if (!block_allocator.SupportsFlush()) {
		return;
	}
	auto block = block_allocator.AllocateData(BLOCK_SIZE);
	block_allocator.FreeData(block, BLOCK_SIZE);
	REQUIRE(block_allocator.GetDeallocatedSinceFlush() == BLOCK_SIZE);
	REQUIRE(block_allocator.ClaimDeallocated(2 * BLOCK_SIZE) == 0);
	REQUIRE(block_allocator.GetDeallocatedSinceFlush() == BLOCK_SIZE);
	REQUIRE(block_allocator.ClaimDeallocated(BLOCK_SIZE) == BLOCK_SIZE);
	REQUIRE(block_allocator.GetDeallocatedSinceFlush() == 0);
	block_allocator.AddDeallocated(3 * BLOCK_SIZE);
	REQUIRE(block_allocator.GetDeallocatedSinceFlush() == 3 * BLOCK_SIZE);
}
