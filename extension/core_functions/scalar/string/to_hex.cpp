#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "core_functions/scalar/string_functions.hpp"

namespace duckdb {

template <class T>
static void LowercaseHexFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<T, string_t>(args.data[0], result, args.size(), [&](T value) {
		using UNSIGNED = typename std::make_unsigned<T>::type;
		auto remaining = static_cast<UNSIGNED>(value);
		char buffer[sizeof(UNSIGNED) * 2];
		idx_t position = sizeof(buffer);
		do {
			buffer[--position] = "0123456789abcdef"[remaining & 0xF];
			remaining >>= 4;
		} while (remaining != 0);
		return StringVector::AddString(result, buffer + position, sizeof(buffer) - position);
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
