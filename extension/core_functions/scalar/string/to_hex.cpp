#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "core_functions/scalar/hex_digits.hpp"
#include "core_functions/scalar/string_functions.hpp"

namespace duckdb {

template <class T>
static void LowercaseHexFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<T, string_t>(args.data[0], result, args.size(), [&](T value) {
		using UNSIGNED = typename std::make_unsigned<T>::type;
		const auto bits = static_cast<uint64_t>(static_cast<UNSIGNED>(value));
		char buffer[16];
		WriteHexDigits16<false>(bits, buffer);
		const idx_t digits = MaxValue<idx_t>(16 - CountZeros<uint64_t>::Leading(bits) / 4, 1);
		return StringVector::AddString(result, buffer + 16 - digits, digits);
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
