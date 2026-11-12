#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>
#include <cstring>

using namespace duckdb;

namespace {

struct FreedBlocks : public PrivateAllocatorData {
	vector<pair<data_ptr_t, idx_t>> blocks;
};

data_ptr_t AllocateBlock(PrivateAllocatorData *, idx_t size) {
	return data_ptr_cast(malloc(size));
}

void KeepFreedBlock(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
	memset(pointer, 0x5a, size);
	static_cast<FreedBlocks &>(*private_data).blocks.emplace_back(pointer, size);
}

data_ptr_t ReallocateBlock(PrivateAllocatorData *, data_ptr_t pointer, idx_t, idx_t size) {
	return data_ptr_cast(realloc(pointer, size));
}

} // namespace

TEST_CASE("A segment taken over by Combine is destroyed before the arena it lives in is freed", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);

	auto freed_data = make_uniq<FreedBlocks>();
	auto &freed = freed_data->blocks;
	Allocator allocator(AllocateBlock, KeepFreedBlock, ReallocateBlock, std::move(freed_data));

	auto layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize({LogicalType::BIGINT}, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	auto source = make_uniq<TupleDataCollection>(buffer_manager, layout, MemoryTag::HASH_TABLE,
	                                             make_shared_ptr<ArenaAllocator>(allocator));
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	chunk.SetValue(0, 0, Value::BIGINT(42));
	chunk.SetCardinality(1);
	source->Append(chunk);

	auto target = make_uniq<TupleDataCollection>(buffer_manager, layout, MemoryTag::HASH_TABLE);
	target->Combine(*source);
	source.reset();
	REQUIRE(target->Count() == 1);
	REQUIRE(freed.empty());

	target.reset();
	REQUIRE(!freed.empty());
	for (auto &block : freed) {
		REQUIRE(std::all_of(block.first, block.first + block.second, [](data_t byte) { return byte == 0x5a; }));
		free(block.first);
	}
}
