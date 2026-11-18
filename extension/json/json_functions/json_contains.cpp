#include "json_executors.hpp"

namespace duckdb {

static bool JSONFuzzyEquals(const JSONNode &haystack, const JSONNode &needle) {
	// Strict equality
	if (JSONEquals(haystack, needle)) {
		return true;
	}

	if (needle.type != haystack.type) {
		return false;
	}

	// Fuzzy equality (contained in)
	switch (needle.type) {
	case JSONValueType::ARRAY:
		for (auto &needle_child : needle.Elements()) {
			bool found = false;
			for (auto &haystack_child : haystack.Elements()) {
				if (JSONFuzzyEquals(haystack_child, needle_child)) {
					found = true;
					break;
				}
			}
			if (!found) {
				return false;
			}
		}
		return true;
	case JSONValueType::OBJECT:
		for (auto &key : needle.Keys()) {
			auto haystack_child = haystack.Get(key.GetString());
			if (!haystack_child || !JSONFuzzyEquals(*haystack_child, *(&key + 1))) {
				return false;
			}
		}
		return true;
	default:
		return false;
	}
}

static bool JSONContains(const JSONNode &haystack, const JSONNode &needle) {
	if (JSONFuzzyEquals(haystack, needle)) {
		return true;
	}

	switch (haystack.type) {
	case JSONValueType::ARRAY:
		for (auto &child_haystack : haystack.Elements()) {
			if (JSONContains(child_haystack, needle)) {
				return true;
			}
		}
		return false;
	case JSONValueType::OBJECT:
		for (auto &key : haystack.Keys()) {
			if (JSONContains(*(&key + 1), needle)) {
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}

static void JSONContainsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.data.size() == 2);
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);

	const auto &haystacks = args.data[0];
	const auto &needles = args.data[1];

	if (needles.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(needles)) {
			throw InternalException("JSON Contains called with constant NULL needles");
		}
		auto &needle_str = *ConstantVector::GetData<string_t>(needles);
		auto &needle = JSONCommon::ReadDocument(lstate.second_parser, needle_str);
		UnaryExecutor::Execute<string_t, bool>(haystacks, result, [&](string_t haystack_str) {
			auto &haystack = JSONCommon::ReadDocument(lstate.parser, haystack_str);
			return JSONContains(haystack, needle);
		});
	} else {
		BinaryExecutor::Execute<string_t, string_t, bool>(
		    haystacks, needles, result, [&](string_t haystack_str, string_t needle_str) {
			    auto &needle = JSONCommon::ReadDocument(lstate.second_parser, needle_str);
			    auto &haystack = JSONCommon::ReadDocument(lstate.parser, haystack_str);
			    return JSONContains(haystack, needle);
		    });
	}
}

static void GetContainsFunctionInternal(ScalarFunctionSet &set, const LogicalType &lhs, const LogicalType &rhs) {
	ScalarFunction fun({}, LogicalType::BOOLEAN, JSONContainsFunction, nullptr, nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("haystack", lhs).AddParameter("needle", rhs);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetContainsFunction() {
	ScalarFunctionSet set("json_contains");
	GetContainsFunctionInternal(set, LogicalType::VARCHAR, LogicalType::VARCHAR);
	GetContainsFunctionInternal(set, LogicalType::VARCHAR, LogicalType::JSON());
	GetContainsFunctionInternal(set, LogicalType::JSON(), LogicalType::VARCHAR);
	set.SetFallible();
	GetContainsFunctionInternal(set, LogicalType::JSON(), LogicalType::JSON());
	// TODO: implement json_contains that accepts path argument as well

	return set;
}

} // namespace duckdb
