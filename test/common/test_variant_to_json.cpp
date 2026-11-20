#include "catch.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

using namespace duckdb; // NOLINT

namespace {

struct RepeatedKey {
	bool Emit(idx_t, VariantBuilder &builder) {
		const string_t keys[] = {"a", "a", "b"};
		builder.EmitObject(
		    3, [&](idx_t i) { return keys[i]; },
		    [&](idx_t i) {
			    if (i == 1) {
				    builder.EmitArray(0, [](idx_t) {});
			    } else {
				    builder.EmitNull();
			    }
		    });
		return false;
	}
};

} // namespace

TEST_CASE("Casting a VARIANT object to JSON keeps the last value of a repeated key", "[variant_to_json]") {
	RepeatedKey source;
	Vector variant(LogicalType::VARIANT(), 1);
	BuildVariant(source, 1, variant);
	Vector json(LogicalType::JSON(), 1);
	VectorOperations::DefaultCast(variant, json, 1);
	REQUIRE(json.GetValue(0).ToString() == "{\"a\":[],\"b\":null}");
}
