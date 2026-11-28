#include "json_executors.hpp"

namespace duckdb {

static inline optional<string_t> ExtractFromVal(const JSONNode *val, JSONFunctionLocalState &lstate, Vector &result) {
	return JSONCommon::WriteVal(*val, lstate.writer, result);
}

static inline optional<string_t> ExtractStringFromVal(const JSONNode *val, JSONFunctionLocalState &lstate,
                                                      Vector &result) {
	switch (val->type) {
	case JSONValueType::JSON_NULL:
		return {};
	case JSONValueType::STRING:
		return StringVector::AddString(result, val->GetString());
	default:
		return JSONCommon::WriteVal(*val, lstate.writer, result);
	}
}

static void ExtractFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONReadFunctionData>();
	if (info.constant && info.writable) {
		auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
		JSONParseError error;
		UnaryExecutor::Execute<string_t, string_t>(
		    args.data[0], result, args.size(), [&](string_t input) -> optional<string_t> {
			    bool found;
			    if (!lstate.parser.WritePath(input.GetData(), input.GetSize(), info.steps, lstate.writer, found,
			                                 error)) {
				    JSONCommon::ThrowParseError(input.GetData(), input.GetSize(), error);
			    }
			    if (!found) {
				    return nullopt;
			    }
			    auto text = lstate.writer.View();
			    return StringVector::AddString(result, text.data(), text.size());
		    });
		return;
	}
	JSONExecutors::BinaryExecute<string_t>(args, state, result, ExtractFromVal);
}

static void ExtractManyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<string_t>(args, state, result, ExtractFromVal);
}

static void ExtractStringFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::BinaryExecute<string_t>(args, state, result, ExtractStringFromVal);
}

static void ExtractStringManyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<string_t>(args, state, result, ExtractStringFromVal);
}

static void GetExtractFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction index_fun({}, LogicalType::JSON(), ExtractFunction, JSONReadFunctionData::Bind, nullptr,
	                         JSONFunctionLocalState::Init);
	index_fun.GetSignature().AddParameter("json", input_type).AddParameter("index", LogicalType::BIGINT);
	set.AddFunction(index_fun);
	ScalarFunction path_fun({}, LogicalType::JSON(), ExtractFunction, JSONReadFunctionData::Bind, nullptr,
	                        JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	set.AddFunction(path_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::JSON()), ExtractManyFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetExtractFunction() {
	// Generic extract function
	ScalarFunctionSet set("json_extract");
	GetExtractFunctionsInternal(set, LogicalType::VARCHAR);
	GetExtractFunctionsInternal(set, LogicalType::JSON());
	set.ApplyToFunctions([](ScalarFunction &func) {
		const auto &sig = func.GetSignature();
		if (sig.GetParameter(0).GetType().IsJSONType() && sig.GetParameter(1).GetType().IsNumeric()) {
			return;
		}
		func.SetFallible();
	});
	return set;
}

static void GetExtractStringFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction index_fun({}, LogicalType::VARCHAR, ExtractStringFunction, JSONReadFunctionData::Bind, nullptr,
	                         JSONFunctionLocalState::Init);
	index_fun.GetSignature().AddParameter("json", input_type).AddParameter("index", LogicalType::BIGINT);
	set.AddFunction(index_fun);
	ScalarFunction path_fun({}, LogicalType::VARCHAR, ExtractStringFunction, JSONReadFunctionData::Bind, nullptr,
	                        JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	set.AddFunction(path_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::VARCHAR), ExtractStringManyFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetExtractStringFunction() {
	// String extract function
	ScalarFunctionSet set("json_extract_string");
	GetExtractStringFunctionsInternal(set, LogicalType::VARCHAR);
	GetExtractStringFunctionsInternal(set, LogicalType::JSON());
	set.ApplyToFunctions([](ScalarFunction &func) {
		const auto &sig = func.GetSignature();
		if (sig.GetParameter(0).GetType().IsJSONType() && sig.GetParameter(1).GetType().IsNumeric()) {
			return;
		}
		func.SetFallible();
	});
	return set;
}

} // namespace duckdb
