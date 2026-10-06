#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "core_functions/scalar/string_functions.hpp"

#include <array>
#include <cstring>

namespace duckdb {

static const char *LowercaseHexPairs() {
	static const auto pairs = [] {
		std::array<char, 512> table {};
		for (idx_t byte = 0; byte < 256; byte++) {
			table[2 * byte] = "0123456789abcdef"[byte >> 4];
			table[2 * byte + 1] = "0123456789abcdef"[byte & 0x0F];
		}
		return table;
	}();
	return pairs.data();
}

template <class T>
static void LowercaseHexFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto pairs = LowercaseHexPairs();
	UnaryExecutor::Execute<T, string_t>(args.data[0], result, args.size(), [&](T value) {
		using UNSIGNED = typename std::make_unsigned<T>::type;
		auto remaining = static_cast<UNSIGNED>(value);
		constexpr idx_t SIZE = sizeof(UNSIGNED) * 2;
		char buffer[SIZE];
		for (idx_t i = sizeof(UNSIGNED); i > 0; i--) {
			memcpy(buffer + 2 * (i - 1), pairs + 2 * (remaining & 0xFF), 2);
			remaining = static_cast<UNSIGNED>(remaining >> 4 >> 4);
		}
		auto leading = CountZeros<uint64_t>::Leading(static_cast<uint64_t>(static_cast<UNSIGNED>(value))) -
		               (64 - 8 * sizeof(UNSIGNED));
		idx_t digits = SIZE - leading / 4;
		digits = MaxValue<idx_t>(digits, 1);
		return StringVector::AddString(result, buffer + SIZE - digits, digits);
	});
}

ScalarFunctionSet ToHexFun::GetFunctions() {
	ScalarFunctionSet to_hex;
	auto hex = HexFun::GetFunctions();
	for (auto &function : hex.functions) {
		if (function->GetSignature().GetParameter(0).GetType() != LogicalType::BIGINT) {
			to_hex.AddFunction(function);
		}
	}

	ScalarFunction integer_fun({}, LogicalType::VARCHAR, LowercaseHexFunction<int32_t>);
	integer_fun.GetSignature().AddParameter("value", LogicalType::INTEGER);
	to_hex.AddFunction(integer_fun);

	ScalarFunction bigint_fun({}, LogicalType::VARCHAR, LowercaseHexFunction<int64_t>);
	bigint_fun.GetSignature().AddParameter("value", LogicalType::BIGINT);
	to_hex.AddFunction(bigint_fun);

	return to_hex;
}

} // namespace duckdb
