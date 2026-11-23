#include "catch.hpp"
#include "duckdb/main/result_unit.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

unique_ptr<DataChunk> MakeChunk(idx_t rows) {
	auto chunk = make_uniq<DataChunk>();
	chunk->Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT, LogicalType::VARCHAR},
	                  MaxValue<idx_t>(rows, 1));
	for (idx_t i = 0; i < rows; i++) {
		chunk->data[0].SetValue(i, Value::BIGINT(NumericCast<int64_t>(i)));
		chunk->data[1].SetValue(i, Value(string(i, 'x')));
	}
	chunk->SetChildCardinality(rows);
	return chunk;
}

class HeldText : public AuxiliaryDataHolder {
public:
	explicit HeldText(string text_p) : text(std::move(text_p)) {
	}

	string text;
};

} // namespace

TEST_CASE("A vector's data size counts its strings' bytes wherever they are held", "[api][result_unit]") {
	const string text(100000, 'x');
	const idx_t expected = 3 * sizeof(string_t) + text.size();

	Vector heap(LogicalType::VARCHAR, 3);
	heap.SetValue(0, Value(text));
	heap.SetValue(1, Value("short"));
	heap.SetValue(2, Value());
	FlatVector::SetSize(heap, 3);
	REQUIRE(heap.GetDataSize() == expected);

	Vector held(LogicalType::VARCHAR, 3);
	auto holder = make_uniq<HeldText>(text);
	const string_t reference(holder->text.data(), NumericCast<uint32_t>(holder->text.size()));
	StringVector::AddAuxiliaryData(held, std::move(holder));
	{
		auto writer = FlatVector::Writer<string_t>(held, 3);
		writer.WriteStringRef(reference);
		writer.WriteStringRef(string_t("short"));
		writer.WriteNull();
	}
	REQUIRE(held.GetDataSize() == expected);

	const Vector constant(Value(text), count_t(4));
	REQUIRE(constant.GetDataSize() == 4 * (sizeof(string_t) + text.size()));

	SelectionVector sel(4);
	for (idx_t i = 0; i < 4; i++) {
		sel.set_index(i, i % 2);
	}
	const Vector dictionary(held, sel, 4);
	REQUIRE(dictionary.GetDataSize() == 4 * sizeof(string_t) + 2 * text.size());

	auto chunk = make_uniq<DataChunk>();
	chunk->Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR}, 3);
	chunk->data[0].Reference(held);
	const ChunkUnit unit(std::move(chunk));
	REQUIRE(unit.byte_size == expected);
}

TEST_CASE("A chunk unit takes its rows and bytes from the chunk it holds", "[api][result_unit]") {
	auto chunk = MakeChunk(100);
	const auto bytes = chunk->GetDataSize();
	unique_ptr<ResultUnit> unit = make_uniq<ChunkUnit>(std::move(chunk));
	REQUIRE(unit->row_count == 100);
	REQUIRE(unit->byte_size == bytes);
	REQUIRE(unit->Cast<ChunkUnit>().chunk->size() == 100);
	const ResultUnit &const_unit = *unit;
	REQUIRE(const_unit.Cast<ChunkUnit>().chunk->size() == 100);

	unique_ptr<ResultUnit> empty = make_uniq<ChunkUnit>(MakeChunk(0));
	REQUIRE(empty->row_count == 0);
	REQUIRE(empty->Cast<ChunkUnit>().chunk->size() == 0);
}
