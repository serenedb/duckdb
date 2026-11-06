#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "test_helpers.hpp"

namespace duckdb {

TEST_CASE("A vector over borrowed data has a validity entry for every row", "[vector]") {
	vector<int64_t> rows(STANDARD_VECTOR_SIZE * 2);
	for (idx_t i = 0; i < rows.size(); i++) {
		rows[i] = NumericCast<int64_t>(i);
	}
	Vector vec(LogicalType::BIGINT, data_ptr_cast(rows.data()), rows.size());
	idx_t row = 0;
	for (auto entry : vec.Values<int64_t>()) {
		REQUIRE(entry.IsValid());
		REQUIRE(entry.GetValue() == NumericCast<int64_t>(row));
		row++;
	}
	REQUIRE(row == rows.size());
}

TEST_CASE("Slices of collected arrays without NULLs serialize every child value", "[vector]") {
	DuckDB db(nullptr);
	Connection con(db);
	const idx_t rows = STANDARD_VECTOR_SIZE + STANDARD_VECTOR_SIZE / 2;
	auto result =
	    con.Submit("SELECT [i::INTEGER, i::INTEGER + 1]::INTEGER[2] FROM range(" + to_string(rows) + ") t(i)");
	REQUIRE(!result->HasError());
	ColumnDataCollection collection(BufferManager::GetBufferManager(*db.instance), result->GetTypes());
	while (auto chunk = result->FetchRaw()) {
		collection.Append(*chunk);
	}
	REQUIRE(collection.Count() == rows);

	idx_t row = 0;
	for (auto &chunk : collection.Chunks()) {
		DataChunk slice;
		slice.InitializeEmpty(collection.Types());
		slice.Slice(chunk, 0, chunk.size());

		MemoryStream stream;
		BinarySerializer serializer(stream);
		serializer.Begin();
		slice.Serialize(serializer);
		serializer.End();
		stream.Rewind();
		BinaryDeserializer deserializer(stream);
		deserializer.Begin();
		DataChunk copy;
		copy.Deserialize(deserializer);
		deserializer.End();

		REQUIRE(copy.size() == chunk.size());
		const auto &child = ArrayVector::GetChild(copy.data[0]);
		REQUIRE(child.size() == chunk.size() * 2);
		idx_t value = 0;
		for (auto entry : child.Values<int32_t>()) {
			REQUIRE(entry.IsValid());
			REQUIRE(entry.GetValue() == NumericCast<int32_t>(row + value / 2 + value % 2));
			value++;
		}
		row += chunk.size();
	}
	REQUIRE(row == rows);
}

} // namespace duckdb
