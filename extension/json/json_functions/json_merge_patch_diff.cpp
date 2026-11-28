#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

struct DiffFrame {
	const JSONNode *old_object;
	uint32_t remaining;
	const JSONNode *next;
};

struct DiffState {
	JSONStringWriter &writer;
	vector<DiffFrame> frames;
	vector<std::string_view> pending;
	idx_t opened = 0;

	void OpenPending() {
		for (; opened < pending.size(); opened++) {
			writer.Key(pending[opened]);
			writer.StartObject();
		}
	}

	void Start(const JSONNode &old_object, const JSONNode &new_object) {
		for (auto &key : old_object.Keys()) {
			if (!new_object.Get(key.GetString())) {
				OpenPending();
				writer.Key(key.GetString());
				writer.Null();
			}
		}
		frames.push_back({&old_object, new_object.size, new_object.First()});
	}
};

static void WriteDiff(DiffState &state, const JSONNode &old_root, const JSONNode &new_root) {
	auto &writer = state.writer;
	auto &frames = state.frames;
	auto &pending = state.pending;
	state.opened = 0;
	state.Start(old_root, new_root);
	while (!frames.empty()) {
		auto &frame = frames.back();
		if (frame.remaining == 0) {
			frames.pop_back();
			if (!frames.empty()) {
				if (state.opened == pending.size()) {
					writer.EndObject();
					state.opened--;
				}
				pending.pop_back();
			}
			continue;
		}
		frame.remaining--;
		auto &key = *frame.next;
		auto &new_child = *(&key + 1);
		frame.next = new_child.Next();
		auto old_child = frame.old_object->Get(key.GetString());
		if (old_child && old_child->type == JSONValueType::OBJECT && new_child.type == JSONValueType::OBJECT) {
			pending.push_back(key.GetString());
			state.Start(*old_child, new_child);
		} else if (!old_child || !JSONEquals(*old_child, new_child)) {
			state.OpenPending();
			writer.Key(key.GetString());
			writer.Value(new_child);
		}
	}
}

//! Compute the RFC 7396 merge patch that transforms old into new
static void MergePatchDiffFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	const auto count = args.size();

	UnifiedVectorFormat old_data, new_data;
	args.data[0].ToUnifiedFormat(old_data);
	args.data[1].ToUnifiedFormat(new_data);
	auto old_inputs = UnifiedVectorFormat::GetData<string_t>(old_data);
	auto new_inputs = UnifiedVectorFormat::GetData<string_t>(new_data);

	auto &writer = lstate.writer;
	DiffState diff {writer, {}, {}};
	auto result_data = FlatVector::Writer<string_t>(result, count);

	for (idx_t i = 0; i < count; i++) {
		auto old_idx = old_data.sel->get_index(i);
		auto new_idx = new_data.sel->get_index(i);

		if (!new_data.validity.RowIsValid(new_idx)) {
			result_data.WriteNull();
			continue;
		}

		auto &new_root = JSONCommon::ReadDocument(lstate.parser, new_inputs[new_idx]);

		if (!old_data.validity.RowIsValid(old_idx)) {
			result_data.WriteStringRef(JSONCommon::WriteVal(new_root, writer, result));
			continue;
		}

		auto &old_root = JSONCommon::ReadDocument(lstate.second_parser, old_inputs[old_idx]);
		writer.Clear();
		if (old_root.type == JSONValueType::OBJECT && new_root.type == JSONValueType::OBJECT) {
			writer.StartObject();
			WriteDiff(diff, old_root, new_root);
			writer.EndObject();
		} else {
			writer.Value(new_root);
		}
		result_data.WriteStringRef(StringVector::AddString(result, writer.View()));
	}
}

ScalarFunctionSet JSONFunctions::GetMergePatchDiffFunction() {
	ScalarFunction fun("json_merge_patch_diff", {}, LogicalType::JSON(), MergePatchDiffFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("old", LogicalType::JSON()).AddParameter("new", LogicalType::JSON());
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();

	return ScalarFunctionSet(fun);
}

} // namespace duckdb
