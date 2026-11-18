#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

struct NormalizeFrame {
	bool object;
	uint32_t remaining;
	idx_t begin;
	idx_t next_key;
	const JSONNode *next_element;
};

static void WriteSortedKeys(JSONStringWriter &writer, const JSONNode &root, vector<const JSONNode *> &keys,
                            vector<NormalizeFrame> &frames) {
	auto node = &root;
	while (true) {
		switch (node->type) {
		case JSONValueType::OBJECT: {
			const auto begin = keys.size();
			for (auto &key : node->Keys()) {
				keys.push_back(&key);
			}
			std::sort(keys.begin() + NumericCast<int64_t>(begin), keys.end(),
			          [](const JSONNode *a, const JSONNode *b) { return a->GetString() < b->GetString(); });
			writer.StartObject();
			frames.push_back({true, node->size, begin, begin, nullptr});
			break;
		}
		case JSONValueType::ARRAY:
			writer.StartArray();
			frames.push_back({false, node->size, 0, 0, node->First()});
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
					keys.resize(frame.begin);
				} else {
					writer.EndArray();
				}
				frames.pop_back();
				continue;
			}
			frame.remaining--;
			if (frame.object) {
				const auto key = keys[frame.next_key++];
				writer.Key(key->GetString());
				node = key + 1;
			} else {
				node = frame.next_element;
				frame.next_element = node->Next();
			}
			break;
		}
	}
}

//! Normalize JSON: sort all object keys recursively, serialize compact
static void NormalizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);

	const auto count = args.size();
	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	vector<const JSONNode *> keys;
	vector<NormalizeFrame> frames;
	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			result_data.WriteNull();
			continue;
		}

		auto &root = JSONCommon::ReadDocument(lstate.parser, inputs[idx]);
		lstate.writer.Clear();
		WriteSortedKeys(lstate.writer, root, keys, frames);
		result_data.WriteStringRef(StringVector::AddString(result, lstate.writer.View()));
	}
}

ScalarFunctionSet JSONFunctions::GetNormalizeFunction() {
	ScalarFunctionSet set("json_normalize");

	ScalarFunction func({}, LogicalType::VARCHAR, NormalizeFunction, nullptr, nullptr, JSONFunctionLocalState::Init);

	func.GetSignature().AddParameter("json", LogicalType::JSON());

	set.AddFunction(std::move(func));

	return set;
}

} // namespace duckdb
