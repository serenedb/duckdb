#include "json_executors.hpp"

namespace duckdb {

static void ValidFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	const auto &inputs = args.data[0];
	JSONParseError error;
	UnaryExecutor::Execute<string_t, bool>(inputs, result, [&](string_t input) {
		return JSONCommon::ReadDocumentUnsafe(lstate.parser, input, error) != nullptr;
	});
}

static void GetValidFunctionInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction fun("json_valid", {}, LogicalType::BOOLEAN, ValidFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", input_type);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetValidFunction() {
	ScalarFunctionSet set("json_valid");
	GetValidFunctionInternal(set, LogicalType::VARCHAR);
	GetValidFunctionInternal(set, LogicalType::JSON());

	return set;
}

} // namespace duckdb
