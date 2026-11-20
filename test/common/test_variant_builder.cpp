#include "catch.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/types/vector.hpp"

using namespace duckdb; // NOLINT

namespace {

struct KnownTypes {
	bool Emit(idx_t row, VariantBuilder &builder) {
		if (row == 1) {
			return true;
		}
		const string_t keys[] = {"flag", "n", "off", "word"};
		builder.EmitObject(
		    4, [&](idx_t i) { return keys[i]; },
		    [&](idx_t i) {
			    switch (i) {
			    case 0:
				    builder.EmitBool(true);
				    break;
			    case 1:
				    builder.EmitFixed(VariantLogicalType::UINT64, uint64_t(7));
				    break;
			    case 2:
				    builder.EmitBool(false);
				    break;
			    default:
				    builder.EmitString(VariantLogicalType::VARCHAR, "a string longer than inlined");
				    break;
			    }
		    });
		return false;
	}
};

} // namespace

TEST_CASE("A VARIANT built from values of known types reads back as those values", "[variant_builder]") {
	KnownTypes source;
	Vector result(LogicalType::VARIANT(), 2);
	BuildVariant(source, 2, result);
	REQUIRE(result.GetValue(0).ToString() ==
	        "{'flag': true, 'n': 7, 'off': false, 'word': a string longer than inlined}");
	REQUIRE(result.GetValue(1).IsNull());
}
