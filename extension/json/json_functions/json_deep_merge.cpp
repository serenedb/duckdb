#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

struct DeepMergeFrame {
	const JSONNode *orig;
	uint32_t remaining;
	const JSONNode *next;
};

static void StartDeepMerge(JSONStringWriter &writer, const JSONNode &orig, const JSONNode &patch,
                           vector<DeepMergeFrame> &frames) {
	writer.StartObject();
	for (auto &key : orig.Keys()) {
		auto patch_val = patch.Get(key.GetString());
		if (!patch_val || patch_val->type == JSONValueType::JSON_NULL) {
			writer.Key(key.GetString());
			writer.Value(*(&key + 1));
		}
	}
	frames.push_back({&orig, patch.size, patch.First()});
}

//! Coalescing deep merge: null in patch means "absent/unknown", keeps the original value.
//! Non-null patch values overwrite.
static void WriteDeepMerge(JSONStringWriter &writer, const JSONNode &orig, const JSONNode &patch,
                           vector<DeepMergeFrame> &frames) {
	StartDeepMerge(writer, orig, patch, frames);
	while (!frames.empty()) {
		auto &frame = frames.back();
		if (frame.remaining == 0) {
			writer.EndObject();
			frames.pop_back();
			continue;
		}
		frame.remaining--;
		auto &key = *frame.next;
		auto &patch_val = *(&key + 1);
		frame.next = patch_val.Next();
		if (patch_val.type == JSONValueType::JSON_NULL) {
			continue;
		}
		auto orig_val = frame.orig->Get(key.GetString());
		writer.Key(key.GetString());
		if (patch_val.type == JSONValueType::OBJECT && orig_val && orig_val->type == JSONValueType::OBJECT) {
			StartDeepMerge(writer, *orig_val, patch_val, frames);
		} else {
			writer.Value(patch_val);
		}
	}
}

static inline void DeepMergeReadObjects(JSONTreeParser &parser, const Vector &input, optional_idx roots[]) {
	const idx_t count = input.size();
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	JSONParseError error;
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			roots[i] = optional_idx();
			continue;
		}
		const auto &str = inputs[idx];
		roots[i] = parser.ParseAppend(str.GetData(), str.GetSize(), error);
		if (!roots[i].IsValid()) {
			JSONCommon::ThrowParseError(str.GetData(), str.GetSize(), error);
		}
	}
}

static void DeepMergeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	const auto count = args.size();
	const auto arg_count = args.data.size();

	auto roots =
	    reinterpret_cast<optional_idx *>(lstate.arena.AllocateAligned(sizeof(optional_idx) * count * arg_count));
	lstate.parser.Clear();
	for (idx_t arg_idx = 0; arg_idx < arg_count; arg_idx++) {
		DeepMergeReadObjects(lstate.parser, args.data[arg_idx], roots + arg_idx * count);
	}

	auto &writer = lstate.writer;
	vector<DeepMergeFrame> frames;
	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		idx_t first = 0;
		for (idx_t arg_idx = 0; arg_idx < arg_count; arg_idx++) {
			if (!roots[arg_idx * count + i].IsValid()) {
				first = arg_idx + 1;
			}
		}
		if (first == arg_count) {
			result_data.WriteNull();
			continue;
		}

		auto orig = lstate.parser.Node(roots[first * count + i].GetIndex());
		bool merged = false;
		for (idx_t arg_idx = first + 1; arg_idx < arg_count; arg_idx++) {
			auto patch = lstate.parser.Node(roots[arg_idx * count + i].GetIndex());
			if (patch->type == JSONValueType::JSON_NULL) {
				continue;
			}
			if (patch->type != JSONValueType::OBJECT) {
				orig = patch;
				merged = false;
				continue;
			}
			if (merged) {
				orig = &JSONCommon::ReadDocument(lstate.second_parser, writer.View().data(), writer.View().size());
				merged = false;
			}
			if (orig->type != JSONValueType::OBJECT) {
				orig = patch;
				continue;
			}
			writer.Clear();
			WriteDeepMerge(writer, *orig, *patch, frames);
			merged = true;
		}
		if (!merged) {
			writer.Clear();
			writer.Value(*orig);
		}
		result_data.WriteStringRef(StringVector::AddString(result, writer.View()));
	}
}

ScalarFunctionSet JSONFunctions::GetDeepMergeFunction() {
	ScalarFunction fun("json_deep_merge", {}, LogicalType::JSON(), DeepMergeFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json1", LogicalType::JSON()).AddParameter("json2", LogicalType::JSON());
	fun.GetSignature().AddArgs("args", LogicalType::JSON());
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();

	return ScalarFunctionSet(fun);
}

} // namespace duckdb
