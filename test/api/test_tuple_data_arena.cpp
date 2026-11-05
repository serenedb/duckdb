#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_layout.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <cstring>

using namespace duckdb;

namespace {

struct ReusingAllocatorData : public PrivateAllocatorData {
	vector<pair<data_ptr_t, idx_t>> reused;
};

data_ptr_t ReusingAllocate(PrivateAllocatorData *, idx_t size) {
	return data_ptr_cast(malloc(size));
}

void ReusingFree(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) {
	memset(pointer, 0x67, size);
	static_cast<ReusingAllocatorData &>(*private_data).reused.emplace_back(pointer, size);
}

data_ptr_t ReusingReallocate(PrivateAllocatorData *, data_ptr_t pointer, idx_t, idx_t size) {
	return data_ptr_cast(realloc(pointer, size));
}

} // namespace

TEST_CASE("Segments a collection takes over keep their arena until they are destroyed", "[api][tuple_data]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &buffer_manager = BufferManager::GetBufferManager(*con.context);

	auto data = make_uniq<ReusingAllocatorData>();
	auto &reused = data->reused;
	Allocator allocator(ReusingAllocate, ReusingFree, ReusingReallocate, std::move(data));

	auto layout = make_shared_ptr<TupleDataLayout>();
	layout->Initialize({LogicalType::BIGINT}, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	auto source = make_uniq<TupleDataCollection>(buffer_manager, layout, MemoryTag::HASH_TABLE,
	                                             make_shared_ptr<ArenaAllocator>(allocator));
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT});
	chunk.SetValue(0, 0, Value::BIGINT(67));
	chunk.SetCardinality(1);
	source->Append(chunk);

	auto target = make_uniq<TupleDataCollection>(buffer_manager, layout, MemoryTag::HASH_TABLE);
	target->Combine(*source);
	source.reset();
	REQUIRE(target->Count() == 1);
	REQUIRE(reused.empty());

	target.reset();
	REQUIRE(!reused.empty());
	for (auto &block : reused) {
		idx_t intact = 0;
		for (idx_t i = 0; i < block.second; i++) {
			intact += block.first[i] == 0x67;
		}
		REQUIRE(intact == block.second);
		free(block.first);
	}
}
