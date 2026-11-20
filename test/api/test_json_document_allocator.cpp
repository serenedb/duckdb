#include "catch.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/json_document.hpp"

using namespace duckdb;

namespace {

struct CountingAllocatorData : PrivateAllocatorData {
	CountingAllocatorData(atomic<idx_t> &allocated_p, atomic<idx_t> &peak_p) : allocated(allocated_p), peak(peak_p) {
	}

	void Add(idx_t size) {
		auto now = allocated += size;
		if (now > peak) {
			peak = now;
		}
	}

	atomic<idx_t> &allocated;
	atomic<idx_t> &peak;
};

data_ptr_t CountingAllocate(PrivateAllocatorData *data, idx_t size) {
	static_cast<CountingAllocatorData *>(data)->Add(size);
	return data_ptr_cast(malloc(size));
}

void CountingFree(PrivateAllocatorData *data, data_ptr_t pointer, idx_t size) {
	static_cast<CountingAllocatorData *>(data)->allocated -= size;
	free(pointer);
}

data_ptr_t CountingReallocate(PrivateAllocatorData *data, data_ptr_t pointer, idx_t old_size, idx_t size) {
	auto &counting = *static_cast<CountingAllocatorData *>(data);
	counting.allocated -= old_size;
	counting.Add(size);
	return data_ptr_cast(realloc(pointer, size));
}

data_ptr_t RefusingAllocate(PrivateAllocatorData *, idx_t) {
	return nullptr;
}

void RefusingFree(PrivateAllocatorData *, data_ptr_t, idx_t) {
}

data_ptr_t RefusingReallocate(PrivateAllocatorData *, data_ptr_t, idx_t, idx_t) {
	return nullptr;
}

} // namespace

TEST_CASE("A JSON tree parser allocates its parse buffers and nodes through its allocator", "[api]") {
	atomic<idx_t> allocated {0};
	atomic<idx_t> peak {0};
	Allocator allocator(CountingAllocate, CountingFree, CountingReallocate,
	                    make_uniq<CountingAllocatorData>(allocated, peak));

	const idx_t elements = 1000000;
	string document = "[";
	for (idx_t i = 0; i < elements; i++) {
		document += "\"ab\",";
	}
	document += "1]";
	{
		JSONTreeParser parser(JSONReadFlags::NONE, allocator);
		JSONParseError error;
		auto root = parser.ParseAppend(document.data(), document.size(), error);
		REQUIRE(root.IsValid());
		REQUIRE(parser.Node(root.GetIndex())->size == elements + 1);
		const idx_t structural_index = document.size() * sizeof(uint32_t);
		const idx_t nodes = (elements + 2) * sizeof(JSONNode);
		REQUIRE(peak.load() >= structural_index + nodes);
	}
	REQUIRE(allocated.load() == 0);
}

TEST_CASE("A JSON tree parser reports its allocator's out of memory error, not a parse error", "[api]") {
	Allocator allocator(RefusingAllocate, RefusingFree, RefusingReallocate, nullptr);
	JSONTreeParser parser(JSONReadFlags::NONE, allocator);

	string document = "[1, 2]";
	const idx_t length = document.size();
	document.append(JSONTreeParser::PADDING, '\0');
	JSONParseError error;
	REQUIRE_THROWS_AS(parser.ParseAppend(document.data(), length, document.size(), error), OutOfMemoryException);
	REQUIRE(!error.HasError());
}
