#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

struct StripNullsFrame {
	bool object;
	uint32_t remaining;
	const JSONNode *next;
};

static void WriteStrippedNulls(JSONStringWriter &writer, const JSONNode &root, vector<StripNullsFrame> &frames) {
	auto node = &root;
	while (true) {
		switch (node->type) {
		case JSONValueType::OBJECT:
			writer.StartObject();
			frames.push_back({true, node->size, node->First()});
			break;
		case JSONValueType::ARRAY:
			writer.StartArray();
			frames.push_back({false, node->size, node->First()});
			break;
		default:
			writer.Value(*node);
			break;
		}
		while (true) {
			if (frames.empty()) {
				return;
			}
			auto &frame = frames.back();
			if (frame.remaining == 0) {
				if (frame.object) {
					writer.EndObject();
				} else {
					writer.EndArray();
				}
				frames.pop_back();
				continue;
			}
			frame.remaining--;
			node = frame.next;
			if (frame.object) {
				auto &value = *(node + 1);
				frame.next = value.Next();
				if (value.type == JSONValueType::JSON_NULL) {
					continue;
				}
				writer.Key(node->GetString());
				node = &value;
			} else {
				frame.next = node->Next();
			}
			break;
		}
	}
}

static void StripNullsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);

	vector<StripNullsFrame> frames;
	const auto &inputs = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(inputs, result, [&](string_t input) {
		auto &root = JSONCommon::ReadDocument(lstate.parser, input);
		lstate.writer.Clear();
		WriteStrippedNulls(lstate.writer, root, frames);
		return StringVector::AddString(result, lstate.writer.View());
	});
}

static void GetStripNullsFunctionInternal(ScalarFunctionSet &set, const LogicalType &json) {
	ScalarFunction fun("json_strip_nulls", {}, LogicalType::JSON(), StripNullsFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", json);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetStripNullsFunction() {
	ScalarFunctionSet set("json_strip_nulls");
	GetStripNullsFunctionInternal(set, LogicalType::JSON());
	return set;
}

} // namespace duckdb
