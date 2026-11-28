#include "json_executors.hpp"

namespace duckdb {

//! Pretty Print a given JSON Document
static void PrettyPrintFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto json_type = args.data[0].GetType();
	D_ASSERT(json_type == LogicalType::VARCHAR || json_type == LogicalType::JSON());

	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	JSONStringWriter writer(JSONCommon::WRITE_PRETTY_FLAG);
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
		return JSONCommon::WriteDocument(lstate.parser, input, writer, result);
	});
}

static void GetPrettyPrintFunctionInternal(ScalarFunctionSet &set, const LogicalType &json) {
	ScalarFunction fun("json_pretty", {}, LogicalType::VARCHAR, PrettyPrintFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", json);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetPrettyPrintFunction() {
	ScalarFunctionSet set("json_pretty");
	GetPrettyPrintFunctionInternal(set, LogicalType::JSON());
	return set;
}

} // namespace duckdb
