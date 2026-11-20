#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

struct MergePatchFrame {
	const JSONNode *orig;
	uint32_t remaining;
	const JSONNode *next;
};

static void StartMergePatch(JSONStringWriter &writer, const JSONNode *orig, const JSONNode &patch,
                            vector<MergePatchFrame> &frames) {
	if (orig && orig->type != JSONValueType::OBJECT) {
		orig = nullptr;
	}
	writer.StartObject();
	if (orig) {
		for (auto &key : orig->Keys()) {
			if (!patch.Get(key.GetString())) {
				writer.Key(key.GetString());
				writer.Value(*(&key + 1));
			}
		}
	}
	frames.push_back({orig, patch.size, patch.First()});
}

//! RFC 7396 merge patch; a missing or non-object orig is treated as an empty object
static void WriteMergePatch(JSONStringWriter &writer, const JSONNode *orig, const JSONNode &patch,
                            vector<MergePatchFrame> &frames) {
	StartMergePatch(writer, orig, patch, frames);
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
		writer.Key(key.GetString());
		if (patch_val.type == JSONValueType::OBJECT) {
			auto orig_val = frame.orig ? frame.orig->Get(key.GetString()) : nullptr;
			StartMergePatch(writer, orig_val, patch_val, frames);
		} else {
			writer.Value(patch_val);
		}
	}
}

static inline void ReadObjects(JSONTreeParser &parser, const Vector &input, optional_idx roots[]) {
	auto entries = input.Values<string_t>();

	// Read the documents
	JSONParseError error;
	for (idx_t i = 0; i < input.size(); i++) {
		auto entry = entries[i];
		if (!entry.IsValid()) {
			roots[i] = optional_idx();
			continue;
		}
		const auto &str = entry.GetValue();
		roots[i] = parser.ParseAppend(str.GetData(), str.GetSize(), error);
		if (!roots[i].IsValid()) {
			JSONCommon::ThrowParseError(str.GetData(), str.GetSize(), error);
		}
	}
}

//! Follows MySQL behaviour
static void MergePatchFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	const auto count = args.size();
	const auto arg_count = args.data.size();

	auto roots =
	    reinterpret_cast<optional_idx *>(lstate.arena.AllocateAligned(sizeof(optional_idx) * count * arg_count));
	lstate.parser.Clear();
	for (idx_t arg_idx = 0; arg_idx < arg_count; arg_idx++) {
		ReadObjects(lstate.parser, args.data[arg_idx], roots + arg_idx * count);
	}

	// Write to result vector
	auto &writer = lstate.writer;
	vector<MergePatchFrame> frames;
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
			WriteMergePatch(writer, orig, *patch, frames);
			merged = true;
		}
		if (!merged) {
			writer.Clear();
			writer.Value(*orig);
		}
		result_data.WriteStringRef(StringVector::AddString(result, writer.View()));
	}
}

ScalarFunctionSet JSONFunctions::GetMergePatchFunction() {
	ScalarFunction fun("json_merge_patch", {}, LogicalType::JSON(), MergePatchFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json1", LogicalType::JSON()).AddParameter("json2", LogicalType::JSON());
	fun.GetSignature().AddArgs("args", LogicalType::JSON());
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);

	return ScalarFunctionSet(fun);
}

} // namespace duckdb
