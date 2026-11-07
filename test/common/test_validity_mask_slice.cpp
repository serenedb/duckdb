#include "catch.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

using namespace duckdb; // NOLINT

TEST_CASE("A slice of an all-valid vector longer than one vector covers every row", "[vector]") {
	const idx_t total = STANDARD_VECTOR_SIZE * 3;
	const idx_t offset = 100;
	const idx_t count = total - offset;
	Vector source(LogicalType::INTEGER, total);
	auto data = FlatVector::GetDataMutable<int32_t>(source);
	for (idx_t i = 0; i < total; i++) {
		data[i] = NumericCast<int32_t>(i);
	}
	FlatVector::SetSize(source, total);

	Vector slice(source, offset, total);
	REQUIRE(FlatVector::Validity(slice).Capacity() >= count);
	REQUIRE(FlatVector::Validity(slice).RowIsValid(count - 1));

	FlatVector::SetNull(slice, count - 1, true);
	REQUIRE(!FlatVector::Validity(slice).RowIsValid(count - 1));
	REQUIRE(FlatVector::Validity(slice).RowIsValid(count - 2));
	REQUIRE(FlatVector::GetData<int32_t>(slice)[count - 2] == NumericCast<int32_t>(total - 2));
}
