#include "json_executors.hpp"

namespace duckdb {

static inline optional<list_entry_t> GetJSONKeys(const JSONNode *val, JSONFunctionLocalState &, Vector &result) {
	const idx_t num_keys = val->type == JSONValueType::OBJECT ? val->size : 0;
	auto current_size = ListVector::GetListSize(result);
	auto new_size = current_size + num_keys;

	// Grow list if needed
	if (ListVector::GetListCapacity(result) < new_size) {
		ListVector::Reserve(result, new_size);
	}

	// Write the strings to the child vector
	auto &child = ListVector::GetChildMutable(result);
	auto keys = FlatVector::GetDataMutable<string_t>(child);
	if (num_keys != 0) {
		idx_t idx = current_size;
		for (auto &key : val->Keys()) {
			keys[idx++] = StringVector::AddString(child, key.GetString());
		}
	}

	// Update size
	ListVector::SetListSize(result, current_size + num_keys);

	return list_entry_t {current_size, num_keys};
}

static void UnaryJSONKeysFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::UnaryExecute<list_entry_t>(args, state, result, GetJSONKeys);
}

static void BinaryJSONKeysFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::BinaryExecute<list_entry_t>(args, state, result, GetJSONKeys);
}

static void ManyJSONKeysFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<list_entry_t>(args, state, result, GetJSONKeys);
}

static void GetJSONKeysFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction unary_fun({}, LogicalType::LIST(LogicalType::VARCHAR), UnaryJSONKeysFunction, nullptr, nullptr,
	                         JSONFunctionLocalState::Init);
	unary_fun.GetSignature().AddParameter("json", input_type);
	set.AddFunction(unary_fun);
	ScalarFunction path_fun({}, LogicalType::LIST(LogicalType::VARCHAR), BinaryJSONKeysFunction,
	                        JSONReadFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	set.AddFunction(path_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::LIST(LogicalType::VARCHAR)), ManyJSONKeysFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetKeysFunction() {
	ScalarFunctionSet set("json_keys");
	GetJSONKeysFunctionsInternal(set, LogicalType::VARCHAR);
	GetJSONKeysFunctionsInternal(set, LogicalType::JSON());
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
