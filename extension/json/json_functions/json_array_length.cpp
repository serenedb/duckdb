#include "duckdb/formats/json_ranges.hpp"
#include "json_executors.hpp"

namespace duckdb {

static inline optional<uint64_t> GetArrayLength(const JSONNode *val, JSONFunctionLocalState &, Vector &) {
	return val->type == JSONValueType::ARRAY ? static_cast<uint64_t>(val->size) : 0;
}

static void UnaryArrayLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	UnaryExecutor::Execute<string_t, uint64_t>(args.data[0], result, [&](string_t input) -> optional<uint64_t> {
		const auto data = input.GetData();
		const auto size = input.GetSize();
		const idx_t root = JsonSkipSpace(data, size, 0);
		if (root < size && (data[root] == '{' || data[root] == '[') && lstate.parser.IsPlain(data, size)) {
			return data[root] == '[' ? JsonArrayLength(data + root, size - root) : 0;
		}
		return GetArrayLength(&JSONCommon::ReadDocument(lstate.parser, input), lstate, result);
	});
}

static void BinaryArrayLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::BinaryExecute<uint64_t>(args, state, result, GetArrayLength);
}

static void ManyArrayLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<uint64_t>(args, state, result, GetArrayLength);
}

static void GetArrayLengthFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction unary_fun({}, LogicalType::UBIGINT, UnaryArrayLengthFunction, nullptr, nullptr,
	                         JSONFunctionLocalState::Init);
	unary_fun.GetSignature().AddParameter("json", input_type);
	set.AddFunction(unary_fun);
	ScalarFunction path_fun({}, LogicalType::UBIGINT, BinaryArrayLengthFunction, JSONReadFunctionData::Bind, nullptr,
	                        JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	set.AddFunction(path_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::UBIGINT), ManyArrayLengthFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetArrayLengthFunction() {
	ScalarFunctionSet set("json_array_length");
	GetArrayLengthFunctionsInternal(set, LogicalType::VARCHAR);
	GetArrayLengthFunctionsInternal(set, LogicalType::JSON());
	set.ApplyToFunctions([](ScalarFunction &func) {
		const auto &sig = func.GetSignature();
		if (sig.GetParameterCount() == 1 && sig.GetParameter(0).GetType().IsJSONType()) {
			return;
		}
		func.SetFallible();
	});
	return set;
}

} // namespace duckdb
