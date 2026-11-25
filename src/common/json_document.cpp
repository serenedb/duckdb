#include "duckdb/common/json_document.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "fast_float/fast_float.h"
#include "simdutf.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <simdjson.h>
#include <simdjson/internal/json_escape.h>

namespace duckdb {

namespace ondemand = simdjson::ondemand;

double JSONNode::GetNumber() const {
	switch (type) {
	case JSONValueType::UNSIGNED_INTEGER:
		return static_cast<double>(unsigned_integer);
	case JSONValueType::SIGNED_INTEGER:
		return static_cast<double>(signed_integer);
	case JSONValueType::DOUBLE:
		return real;
	default:
		return 0;
	}
}

const JSONNode *JSONNode::Get(std::string_view key) const {
	if (type != JSONValueType::OBJECT) {
		return nullptr;
	}
	auto member = First();
	for (uint32_t i = 0; i < size; i++) {
		if (member->GetString() == key) {
			return member + 1;
		}
		member = (member + 1)->Next();
	}
	return nullptr;
}

const JSONNode *JSONNode::Get(idx_t index) const {
	if (type != JSONValueType::ARRAY || index >= size) {
		return nullptr;
	}
	auto element = First();
	for (idx_t i = 0; i < index; i++) {
		element = element->Next();
	}
	return element;
}

bool JSONEquals(const JSONNode &left, const JSONNode &right) {
	if (left.type != right.type) {
		const bool left_int = left.IsInteger();
		if (!left_int || !right.IsInteger()) {
			return false;
		}
		const auto &signed_node = left.type == JSONValueType::SIGNED_INTEGER ? left : right;
		const auto &unsigned_node = left.type == JSONValueType::SIGNED_INTEGER ? right : left;
		return signed_node.signed_integer >= 0 && signed_node.unsigned_integer == unsigned_node.unsigned_integer;
	}
	switch (left.type) {
	case JSONValueType::OBJECT: {
		if (left.size != right.size) {
			return false;
		}
		auto cursor = right.First();
		uint32_t cursor_index = 0;
		auto member = left.First();
		for (uint32_t i = 0; i < left.size; i++) {
			const auto key = member->GetString();
			const JSONNode *other = nullptr;
			for (uint32_t step = 0; step < right.size; step++) {
				if (cursor_index == right.size) {
					cursor = right.First();
					cursor_index = 0;
				}
				auto candidate = cursor;
				cursor = (cursor + 1)->Next();
				cursor_index++;
				if (candidate->GetString() == key) {
					other = candidate + 1;
					break;
				}
			}
			if (!other || !JSONEquals(*(member + 1), *other)) {
				return false;
			}
			member = (member + 1)->Next();
		}
		return true;
	}
	case JSONValueType::ARRAY: {
		if (left.size != right.size) {
			return false;
		}
		auto l = left.First();
		auto r = right.First();
		for (uint32_t i = 0; i < left.size; i++) {
			if (!JSONEquals(*l, *r)) {
				return false;
			}
			l = l->Next();
			r = r->Next();
		}
		return true;
	}
	case JSONValueType::STRING:
	case JSONValueType::RAW:
		return left.GetString() == right.GetString();
	case JSONValueType::BOOLEAN:
		return left.boolean == right.boolean;
	case JSONValueType::UNSIGNED_INTEGER:
	case JSONValueType::SIGNED_INTEGER:
	case JSONValueType::DOUBLE:
		return left.unsigned_integer == right.unsigned_integer;
	default:
		return true;
	}
}

const char *JSONTypeDescription(const JSONNode &node) {
	switch (node.type) {
	case JSONValueType::RAW:
		return "raw";
	case JSONValueType::JSON_NULL:
		return "null";
	case JSONValueType::STRING:
		return "string";
	case JSONValueType::ARRAY:
		return "array";
	case JSONValueType::OBJECT:
		return "object";
	case JSONValueType::BOOLEAN:
		return node.boolean ? "true" : "false";
	case JSONValueType::UNSIGNED_INTEGER:
		return "uint";
	case JSONValueType::SIGNED_INTEGER:
		return "sint";
	case JSONValueType::DOUBLE:
		return "real";
	default:
		return "unknown";
	}
}

struct JSONBuildFrame {
	idx_t index;
	uint32_t count;
	bool object;
	ondemand::array_iterator element;
	ondemand::object_iterator member;
};

static_assert(JSONTreeParser::PADDING >= simdjson::SIMDJSON_PADDING, "JSONTreeParser::PADDING covers simdjson's");

template <class T>
struct JSONStateAllocator {
	using value_type = T;

	explicit JSONStateAllocator(Allocator &allocator_p) : allocator(&allocator_p) {
	}
	template <class U>
	JSONStateAllocator(const JSONStateAllocator<U> &other) : allocator(other.allocator) {
	}

	T *allocate(size_t count) {
		return reinterpret_cast<T *>(allocator->AllocateData(count * sizeof(T)));
	}
	void deallocate(T *pointer, size_t count) {
		allocator->FreeData(data_ptr_cast(pointer), count * sizeof(T));
	}
	template <class U>
	bool operator==(const JSONStateAllocator<U> &other) const {
		return allocator == other.allocator;
	}
	//! Leaves the nodes that resize() adds uninitialized: the builders write every node they keep
	template <class U>
	void construct(U *pointer) {
		::new (static_cast<void *>(pointer)) U;
	}
	template <class U, class... ARGS>
	void construct(U *pointer, ARGS &&... args) {
		::new (static_cast<void *>(pointer)) U(std::forward<ARGS>(args)...);
	}

	Allocator *allocator;
};

using JSONNodes = vector<JSONNode, true, JSONStateAllocator<JSONNode>>;

struct JSONParserBuffers {
	explicit JSONParserBuffers(Allocator &allocator_p)
	    : allocator(allocator_p), callbacks {Allocate, Deallocate, this} {
	}

	static void *Allocate(void *context, size_t size) noexcept {
		auto &buffers = *static_cast<JSONParserBuffers *>(context);
		try {
			return buffers.allocator.AllocateData(size);
		} catch (std::exception &ex) {
			buffers.error = ErrorData(ex);
			return nullptr;
		}
	}
	static void Deallocate(void *context, void *pointer, size_t size) noexcept {
		static_cast<JSONParserBuffers *>(context)->allocator.FreeData(static_cast<data_ptr_t>(pointer), size);
	}

	Allocator &allocator;
	simdjson::buffer_allocator callbacks;
	ErrorData error;
};

struct JSONTreeParserState {
	explicit JSONTreeParserState(Allocator &allocator)
	    : buffers(allocator), nodes(JSONStateAllocator<JSONNode>(allocator)), strings(allocator) {
		parser.set_allocator(&buffers.callbacks);
	}

	JSONParserBuffers buffers;
	ondemand::parser parser;
	AllocatedData input;
	string cleaned;
	//! A document that simdjson rejected, without its trailing commas
	string uncommaed;
	JSONNodes nodes;
	vector<JSONBuildFrame> frames;
	//! The open containers of the visitors' JSONNodeWriter
	vector<idx_t> open;
	//! The node of each value that ParsePaths finds
	vector<optional_idx> path_roots;
	ArenaAllocator strings;
};

static std::string_view TrimJSONToken(std::string_view token) {
	while (!token.empty() && StringUtil::CharacterIsSpace(token.back())) {
		token.remove_suffix(1);
	}
	return token;
}

static bool ParseJSONInfOrNan(std::string_view token, double &result) {
	bool negative = false;
	if (!token.empty() && (token[0] == '-' || token[0] == '+')) {
		negative = token[0] == '-';
		token.remove_prefix(1);
	}
	if (StringUtil::CIEquals(token, "nan")) {
		result = std::numeric_limits<double>::quiet_NaN();
		return true;
	}
	if (StringUtil::CIEquals(token, "inf") || StringUtil::CIEquals(token, "infinity")) {
		result = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
		return true;
	}
	return false;
}

static simdjson::error_code JSONRawToken(ondemand::value &value, std::string_view &token) {
	token = value.raw_json_token();
	return simdjson::SUCCESS;
}

static simdjson::error_code JSONRawToken(ondemand::document &document, std::string_view &token) {
	return document.raw_json_token().get(token);
}

static JSONNode MakeJSONNode(JSONValueType type) {
	JSONNode node;
	node.type = type;
	node.boolean = false;
	node.size = 0;
	node.unsigned_integer = 0;
	return node;
}

struct JSONBuildContext {
	JSONReadFlags flags;
	JSONNodes &nodes;
	vector<JSONBuildFrame> &frames;
	optional_ptr<ArenaAllocator> keep;
	//! The document that simdjson reads
	const char *input;
	const char *input_end;

	simdjson_inline JSONNode MakeString(JSONValueType type, std::string_view text) {
		auto node = MakeJSONNode(type);
		node.size = NumericCast<uint32_t>(text.size());
		// A string that simdjson leaves in the document had no escape: it holds no character that JSON escapes, and it
		// is valid UTF-8, so it is written as it is (see JSONStringWriter::Value)
		node.boolean = type == JSONValueType::STRING && text.data() >= input && text.data() < input_end;
		node.str = keep && !text.empty() ? Keep(text) : text.data();
		return node;
	}
	DUCKDB_NOINLINE const char *Keep(std::string_view text) {
		auto copy = keep->Allocate(text.size());
		memcpy(copy, text.data(), text.size());
		return const_char_ptr_cast(copy);
	}
};

//! Appends nodes to a JSONBuildContext, for the visitors of ondemand::document::visit
class JSONNodeWriter {
public:
	JSONNodeWriter(JSONBuildContext &context_p, vector<idx_t> &open_p, idx_t reserve)
	    : context(context_p), open(open_p), first(nullptr), out(nullptr), end(nullptr) {
		open.clear();
		Grow(context.nodes.size(), reserve);
	}

	//! Drops the nodes reserved but not written
	simdjson_inline void Finish() {
		context.nodes.resize(Size());
	}
	simdjson_inline idx_t Size() const {
		return NumericCast<idx_t>(out - first);
	}

	simdjson_inline void Open(JSONValueType type) {
		open.push_back(Size());
		Add() = MakeJSONNode(type);
	}
	simdjson_inline void Close(uint32_t count) {
		auto &node = first[open.back()];
		open.pop_back();
		node.size = count;
		node.nodes = NumericCast<uint64_t>(out - &node);
	}
	simdjson_inline void String(std::string_view value) {
		Add() = context.MakeString(JSONValueType::STRING, value);
	}
	simdjson_inline void SignedInteger(int64_t value, const char *raw) {
		// as the On-Demand builder: -0 is signed
		auto &node = Add();
		node =
		    MakeJSONNode(value >= 0 && raw[0] != '-' ? JSONValueType::UNSIGNED_INTEGER : JSONValueType::SIGNED_INTEGER);
		node.signed_integer = value;
	}
	simdjson_inline void UnsignedInteger(uint64_t value) {
		auto &node = Add();
		node = MakeJSONNode(JSONValueType::UNSIGNED_INTEGER);
		node.unsigned_integer = value;
	}
	simdjson_inline void Double(double value) {
		auto &node = Add();
		node = MakeJSONNode(JSONValueType::DOUBLE);
		node.real = value;
	}
	simdjson_inline void Boolean(bool value) {
		auto &node = Add();
		node = MakeJSONNode(JSONValueType::BOOLEAN);
		node.boolean = value;
	}
	simdjson_inline void Null() {
		Add() = MakeJSONNode(JSONValueType::JSON_NULL);
	}

private:
	simdjson_inline JSONNode &Add() {
		if (simdjson_unlikely(out == end)) {
			GrowFull();
		}
		return *out++;
	}
	DUCKDB_NOINLINE void GrowFull() {
		const auto used = Size();
		Grow(used, MaxValue<idx_t>(used, 16));
	}
	//! Makes room for extra nodes after the used ones
	void Grow(idx_t used, idx_t extra) {
		context.nodes.resize(used + extra);
		first = context.nodes.data();
		out = first + used;
		end = first + context.nodes.size();
	}

	JSONBuildContext &context;
	vector<idx_t> &open;
	JSONNode *first;
	JSONNode *out;
	JSONNode *end;
};

//! Appends the nodes of a document as ondemand::document::visit walks it. simdjson reads strict JSON: for anything
//! else, visit() fails and the On-Demand builder reads the document.
class JSONNodeVisitor {
public:
	JSONNodeVisitor(JSONBuildContext &context, vector<idx_t> &open, idx_t len) : writer(context, open, len / 8 + 16) {
	}

	void Finish() {
		writer.Finish();
	}

	simdjson_inline simdjson::error_code on_object_start() {
		writer.Open(JSONValueType::OBJECT);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_object_end(uint32_t count) {
		writer.Close(count);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_array_start() {
		writer.Open(JSONValueType::ARRAY);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_array_end(uint32_t count) {
		writer.Close(count);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_key(std::string_view key) {
		writer.String(key);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_string(std::string_view value) {
		writer.String(value);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_int64(int64_t value, const char *raw) {
		writer.SignedInteger(value, raw);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_uint64(uint64_t value, const char *) {
		writer.UnsignedInteger(value);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_double(double value, const char *) {
		writer.Double(value);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_bool(bool value) {
		writer.Boolean(value);
		return simdjson::SUCCESS;
	}
	simdjson_inline simdjson::error_code on_null() {
		writer.Null();
		return simdjson::SUCCESS;
	}

private:
	JSONNodeWriter writer;
};

//! Writes a document as ondemand::document::visit walks it: the same text as building its nodes and writing them
class JSONWriteVisitor {
public:
	JSONWriteVisitor(JSONStringWriter &writer_p, const char *input_p, idx_t len)
	    : writer(writer_p), input(input_p), input_end(input_p + len) {
	}

	simdjson::error_code on_object_start() {
		writer.StartObject();
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_object_end(uint32_t) {
		writer.EndObject();
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_array_start() {
		writer.StartArray();
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_array_end(uint32_t) {
		writer.EndArray();
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_key(std::string_view key) {
		writer.Key(key, InDocument(key));
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_string(std::string_view value) {
		if (InDocument(value)) {
			writer.VerbatimString(value);
		} else {
			writer.String(value);
		}
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_int64(int64_t value, const char *raw) {
		// as JSONNodeWriter: -0 is a signed integer, written as 0
		if (value >= 0 && raw[0] != '-') {
			writer.UnsignedInteger(static_cast<uint64_t>(value));
		} else {
			writer.SignedInteger(value);
		}
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_uint64(uint64_t value, const char *) {
		writer.UnsignedInteger(value);
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_double(double value, const char *) {
		writer.Double(value);
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_bool(bool value) {
		writer.Boolean(value);
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_null() {
		writer.Null();
		return simdjson::SUCCESS;
	}

private:
	//! A string that simdjson leaves in the document had no escape (see JSONBuildContext::MakeString)
	bool InDocument(std::string_view text) const {
		return text.data() >= input && text.data() < input_end;
	}

	JSONStringWriter &writer;
	const char *input;
	const char *input_end;
};

//! Writes nothing: visit() with it checks the document
struct JSONCheckVisitor {
	simdjson::error_code on_object_start() {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_object_end(uint32_t) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_array_start() {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_array_end(uint32_t) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_key(std::string_view) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_string(std::string_view) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_int64(int64_t, const char *) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_uint64(uint64_t, const char *) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_double(double, const char *) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_bool(bool) {
		return simdjson::SUCCESS;
	}
	simdjson::error_code on_null() {
		return simdjson::SUCCESS;
	}
};

template <class VALUE>
static simdjson::error_code BuildJSONNumber(VALUE &value, JSONBuildContext &context) {
	const auto flags = context.flags;
	auto &nodes = context.nodes;
	std::string_view token;
	if (auto error = JSONRawToken(value, token)) {
		return error;
	}
	token = TrimJSONToken(token);
	if (HasFlag(flags, JSONReadFlags::ALLOW_INF_AND_NAN)) {
		double special;
		if (ParseJSONInfOrNan(token, special)) {
			auto node = MakeJSONNode(JSONValueType::DOUBLE);
			node.real = special;
			nodes.push_back(node);
			return simdjson::SUCCESS;
		}
	}
	ondemand::number number;
	auto error = value.get_number().get(number);
	if (error == simdjson::BIGINT_ERROR || error == simdjson::NUMBER_ERROR) {
		double real;
		const auto end = token.data() + token.size();
		const auto result =
		    duckdb_fast_float::from_chars(token.data(), end, real, duckdb_fast_float::chars_format::json);
		if (result.ptr != end || (result.ec != std::errc() && result.ec != std::errc::result_out_of_range)) {
			return simdjson::NUMBER_ERROR;
		}
		const bool overflow = std::isinf(real);
		const bool big = error == simdjson::BIGINT_ERROR || overflow;
		if (HasFlag(flags, JSONReadFlags::NUMBER_AS_RAW) || (big && HasFlag(flags, JSONReadFlags::BIGNUM_AS_RAW))) {
			nodes.push_back(context.MakeString(JSONValueType::RAW, token));
			return simdjson::SUCCESS;
		}
		if (overflow && !HasFlag(flags, JSONReadFlags::ALLOW_INF_AND_NAN)) {
			return simdjson::NUMBER_ERROR;
		}
		auto node = MakeJSONNode(JSONValueType::DOUBLE);
		node.real = real;
		nodes.push_back(node);
		return simdjson::SUCCESS;
	}
	if (error) {
		return error;
	}
	if (HasFlag(flags, JSONReadFlags::NUMBER_AS_RAW)) {
		nodes.push_back(context.MakeString(JSONValueType::RAW, token));
		return simdjson::SUCCESS;
	}
	switch (number.get_number_type()) {
	case ondemand::number_type::signed_integer: {
		const auto integer = number.get_int64();
		auto node = MakeJSONNode(integer >= 0 && token[0] != '-' ? JSONValueType::UNSIGNED_INTEGER
		                                                         : JSONValueType::SIGNED_INTEGER);
		node.signed_integer = integer;
		nodes.push_back(node);
		return simdjson::SUCCESS;
	}
	case ondemand::number_type::unsigned_integer: {
		auto node = MakeJSONNode(JSONValueType::UNSIGNED_INTEGER);
		node.unsigned_integer = number.get_uint64();
		nodes.push_back(node);
		return simdjson::SUCCESS;
	}
	default: {
		auto node = MakeJSONNode(JSONValueType::DOUBLE);
		node.real = number.get_double();
		nodes.push_back(node);
		return simdjson::SUCCESS;
	}
	}
}

//! simdjson reads NaN and Infinity only when built with SIMDJSON_ENABLE_NAN_INF, which would apply to every parser in
//! the binary: the builder reads the token itself
template <class VALUE>
static simdjson::error_code BuildJSONInfOrNan(VALUE &value, JSONBuildContext &context, simdjson::error_code error) {
	std::string_view token;
	double special;
	if (!HasFlag(context.flags, JSONReadFlags::ALLOW_INF_AND_NAN) || JSONRawToken(value, token)) {
		return error;
	}
	token = TrimJSONToken(token);
	if (token.empty() || token[0] == '+' || !ParseJSONInfOrNan(token, special)) {
		return error;
	}
	auto node = MakeJSONNode(JSONValueType::DOUBLE);
	node.real = special;
	context.nodes.push_back(node);
	return simdjson::SUCCESS;
}

template <class VALUE>
static simdjson::error_code StartJSONValue(VALUE &value, JSONBuildContext &context) {
	auto &nodes = context.nodes;
	const bool replace = HasFlag(context.flags, JSONReadFlags::ALLOW_INVALID_UNICODE);
	ondemand::json_type type;
	if (value.type().get(type)) {
		return simdjson::INCORRECT_TYPE;
	}
	switch (type) {
	case ondemand::json_type::object: {
		ondemand::object object;
		if (auto error = value.get_object().get(object)) {
			return error;
		}
		JSONBuildFrame frame {nodes.size(), 0, true, {}, {}};
		if (auto error = std::move(object).begin().get(frame.member)) {
			return error;
		}
		nodes.push_back(MakeJSONNode(JSONValueType::OBJECT));
		context.frames.push_back(frame);
		return simdjson::SUCCESS;
	}
	case ondemand::json_type::array: {
		ondemand::array array;
		if (auto error = value.get_array().get(array)) {
			return error;
		}
		JSONBuildFrame frame {nodes.size(), 0, false, {}, {}};
		if (auto error = std::move(array).begin().get(frame.element)) {
			return error;
		}
		nodes.push_back(MakeJSONNode(JSONValueType::ARRAY));
		context.frames.push_back(frame);
		return simdjson::SUCCESS;
	}
	case ondemand::json_type::string: {
		std::string_view text;
		if (auto error = value.get_string(replace).get(text)) {
			return error;
		}
		nodes.push_back(context.MakeString(JSONValueType::STRING, text));
		return simdjson::SUCCESS;
	}
	case ondemand::json_type::number:
		return BuildJSONNumber(value, context);
	case ondemand::json_type::boolean: {
		bool boolean;
		if (auto error = value.get_bool().get(boolean)) {
			std::string_view token;
			if (error != simdjson::INCORRECT_TYPE || JSONRawToken(value, token) || token.empty()) {
				return error;
			}
			return token[0] == 't' ? simdjson::T_ATOM_ERROR : simdjson::F_ATOM_ERROR;
		}
		auto node = MakeJSONNode(JSONValueType::BOOLEAN);
		node.boolean = boolean;
		nodes.push_back(node);
		return simdjson::SUCCESS;
	}
	case ondemand::json_type::null: {
		bool is_null = false;
		auto error = value.is_null().get(is_null);
		if (error && error != simdjson::INCORRECT_TYPE) {
			return error;
		}
		if (!is_null) {
			return BuildJSONInfOrNan(value, context, simdjson::N_ATOM_ERROR);
		}
		nodes.push_back(MakeJSONNode(JSONValueType::JSON_NULL));
		return simdjson::SUCCESS;
	}
	default:
		return BuildJSONInfOrNan(value, context, simdjson::TAPE_ERROR);
	}
}

static void AdvanceJSONFrame(JSONBuildFrame &frame) {
	if (frame.object) {
		++frame.member;
	} else {
		++frame.element;
	}
}

template <class VALUE>
static simdjson::error_code BuildJSONValue(VALUE &root, JSONBuildContext &context) {
	auto &nodes = context.nodes;
	auto &frames = context.frames;
	const bool replace = HasFlag(context.flags, JSONReadFlags::ALLOW_INVALID_UNICODE);
	if (auto error = StartJSONValue(root, context)) {
		return error;
	}
	while (!frames.empty()) {
		auto &frame = frames.back();
		const bool more =
		    frame.object ? frame.member != ondemand::object_iterator {} : frame.element != ondemand::array_iterator {};
		if (!more) {
			auto &node = nodes[frame.index];
			node.size = frame.count;
			node.nodes = nodes.size() - frame.index;
			frames.pop_back();
			if (!frames.empty()) {
				AdvanceJSONFrame(frames.back());
			}
			continue;
		}
		frame.count++;
		const auto depth = frames.size();
		if (frame.object) {
			ondemand::field field;
			if (auto error = (*frame.member).get(field)) {
				return error;
			}
			std::string_view key;
			if (auto error = field.unescaped_key(replace).get(key)) {
				return error;
			}
			nodes.push_back(context.MakeString(JSONValueType::STRING, key));
			if (auto error = StartJSONValue(field.value(), context)) {
				return error;
			}
		} else {
			ondemand::value child;
			if (auto error = (*frame.element).get(child)) {
				return error;
			}
			if (auto error = StartJSONValue(child, context)) {
				return error;
			}
		}
		if (frames.size() == depth) {
			AdvanceJSONFrame(frames.back());
		}
	}
	return simdjson::SUCCESS;
}

static const char *JSONErrorMessage(simdjson::error_code error) {
	switch (error) {
	case simdjson::EMPTY:
		return "input data is empty";
	case simdjson::UTF8_ERROR:
		return "invalid utf-8 encoding in string";
	case simdjson::STRING_ERROR:
		return "invalid escaped sequence in string";
	case simdjson::UNESCAPED_CHARS:
		return "unexpected control character in string";
	case simdjson::UNCLOSED_STRING:
	case simdjson::INCOMPLETE_ARRAY_OR_OBJECT:
		return "unexpected end of data";
	case simdjson::NUMBER_ERROR:
	case simdjson::BIGINT_ERROR:
		return "invalid number";
	case simdjson::T_ATOM_ERROR:
		return "invalid literal, expected 'true'";
	case simdjson::F_ATOM_ERROR:
		return "invalid literal, expected 'false'";
	case simdjson::N_ATOM_ERROR:
		return "invalid literal, expected 'null'";
	case simdjson::INCORRECT_TYPE:
		return "unexpected character, expected a JSON value";
	case simdjson::DEPTH_ERROR:
		return "the document is nested too deeply";
	case simdjson::CAPACITY:
		return "the document is too large";
	case simdjson::TRAILING_CONTENT:
		return "unexpected content after document";
	default:
		return "unexpected character";
	}
}

static optional_idx StripJSONComments(const char *data, idx_t len, string &out) {
	out.clear();
	out.reserve(len);
	idx_t i = 0;
	while (i < len) {
		const char c = data[i];
		if (c == '"') {
			const idx_t start = i++;
			while (i < len && data[i] != '"') {
				i += data[i] == '\\' ? 2 : 1;
			}
			i = MinValue(i + 1, len);
			out.append(data + start, i - start);
			continue;
		}
		if (c == '/' && i + 1 < len && data[i + 1] == '/') {
			while (i < len && data[i] != '\n') {
				i++;
			}
			out += ' ';
			continue;
		}
		if (c == '/' && i + 1 < len && data[i + 1] == '*') {
			const idx_t start = i;
			i += 2;
			while (i + 1 < len && !(data[i] == '*' && data[i + 1] == '/')) {
				i++;
			}
			if (i + 1 >= len) {
				return start;
			}
			i += 2;
			out += ' ';
			continue;
		}
		out += c;
		i++;
	}
	return optional_idx();
}

static void ReplaceInvalidJSONUnicode(const char *data, idx_t len, string &out) {
	out.clear();
	out.reserve(len);
	idx_t i = 0;
	while (i < len) {
		const auto result = simdutf::validate_utf8_with_errors(data + i, len - i);
		if (result.error == simdutf::SUCCESS) {
			out.append(data + i, len - i);
			return;
		}
		out.append(data + i, result.count);
		out += "\xEF\xBF\xBD";
		i += result.count + 1;
		while (i < len && (static_cast<uint8_t>(data[i]) & 0xC0) == 0x80) {
			i++;
		}
	}
}

static optional_idx ParseInto(JSONTreeParserState &st, JSONReadFlags flags, const char *data, idx_t len, idx_t readable,
                              bool append, JSONParseError &error, optional_ptr<idx_t> consumed = nullptr);

JSONTreeParser::JSONTreeParser(JSONReadFlags flags_p, Allocator &allocator)
    : flags(flags_p), state(make_uniq<JSONTreeParserState>(allocator)) {
	SetFlags(flags_p);
}

JSONTreeParser::~JSONTreeParser() = default;
JSONTreeParser::JSONTreeParser(JSONTreeParser &&other) noexcept = default;
JSONTreeParser &JSONTreeParser::operator=(JSONTreeParser &&other) noexcept = default;

void JSONTreeParser::SetFlags(JSONReadFlags flags_p) {
	flags = flags_p;
}

idx_t JSONTreeParser::NodeCount() const {
	return state->nodes.size();
}

void JSONTreeParser::Clear() {
	state->nodes.clear();
	state->strings.Reset();
}

const JSONNode *JSONTreeParser::Node(idx_t index) const {
	return state->nodes.data() + index;
}

const JSONNode *JSONTreeParser::Parse(const char *data, idx_t len, JSONParseError &error) {
	state->nodes.clear();
	auto root = ParseInto(*state, flags, data, len, len, false, error);
	return root.IsValid() ? state->nodes.data() + root.GetIndex() : nullptr;
}

optional_idx JSONTreeParser::ParseAppend(const char *data, idx_t len, JSONParseError &error) {
	return ParseInto(*state, flags, data, len, len, true, error);
}

optional_idx JSONTreeParser::ParseAppend(const char *data, idx_t len, idx_t readable, JSONParseError &error) {
	return ParseInto(*state, flags, data, len, readable, true, error);
}

bool JSONTreeParser::Validate(const char *data, idx_t len, idx_t readable, JSONParseError &error) {
	constexpr auto rewritten =
	    static_cast<uint32_t>(JSONReadFlags::ALLOW_COMMENTS | JSONReadFlags::ALLOW_INVALID_UNICODE);
	const bool plain = (static_cast<uint32_t>(flags) & rewritten) == 0 && readable >= len + PADDING &&
	                   !(len >= 3 && memcmp(data, "\xEF\xBB\xBF", 3) == 0);
	if (plain) {
		JSONCheckVisitor checker;
		const auto code = state->parser.visit(simdjson::padded_string_view(data, len, readable), checker);
		if (code == simdjson::MEMALLOC && state->buffers.error.HasError()) {
			auto failure = std::move(state->buffers.error);
			state->buffers.error = ErrorData();
			failure.Throw();
		}
		if (!code) {
			return true;
		}
	}
	const idx_t mark = state->nodes.size();
	const auto root = ParseInto(*state, flags, data, len, readable, false, error);
	state->nodes.resize(mark);
	return root.IsValid();
}

const JSONNode *JSONTreeParser::ParsePrefix(const char *data, idx_t len, idx_t &consumed, JSONParseError &error) {
	state->nodes.clear();
	auto root = ParseInto(*state, flags, data, len, len, false, error, &consumed);
	return root.IsValid() ? state->nodes.data() + root.GetIndex() : nullptr;
}

static simdjson::error_code DiagnoseJSONRoot(const char *data, idx_t len, JSONReadFlags flags,
                                             simdjson::error_code code, idx_t &position) {
	idx_t start = 0;
	while (start < len && StringUtil::CharacterIsSpace(data[start])) {
		start++;
	}
	if (start == len) {
		return code;
	}
	const std::string_view rest(data + start, len - start);
	const auto literal = [&](std::string_view keyword, simdjson::error_code atom_error) {
		const bool complete = rest.size() >= keyword.size() && rest.substr(0, keyword.size()) == keyword &&
		                      (rest.size() == keyword.size() || !StringUtil::CharacterIsAlpha(rest[keyword.size()]));
		if (complete) {
			return code;
		}
		position = start;
		return atom_error;
	};
	switch (rest[0]) {
	case 't':
		return literal("true", simdjson::T_ATOM_ERROR);
	case 'f':
		return literal("false", simdjson::F_ATOM_ERROR);
	case 'n':
		return literal("null", simdjson::N_ATOM_ERROR);
	case '{':
	case '[':
	case '"':
	case '-':
		return code;
	case 'N':
	case 'I':
	case '+':
		if (HasFlag(flags, JSONReadFlags::ALLOW_INF_AND_NAN)) {
			return code;
		}
		break;
	default:
		if (StringUtil::CharacterIsDigit(rest[0])) {
			return code;
		}
		break;
	}
	position = start;
	return simdjson::INCORRECT_TYPE;
}

static optional_idx JSONParseFailure(JSONParseError &error, const char *message, idx_t position) {
	error.has_error = true;
	error.message = message;
	error.position = position;
	return optional_idx();
}

static optional_idx RootContainerEnd(const char *data, idx_t len) {
	idx_t pos = 0;
	while (pos < len && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\n' || data[pos] == '\r')) {
		pos++;
	}
	if (pos == len || (data[pos] != '[' && data[pos] != '{')) {
		return optional_idx();
	}
	idx_t depth = 0;
	for (; pos < len; pos++) {
		const char c = data[pos];
		if (c == '"') {
			for (pos++; pos < len && data[pos] != '"'; pos++) {
				pos += data[pos] == '\\';
			}
		} else if (c == '[' || c == '{') {
			depth++;
		} else if ((c == ']' || c == '}') && --depth == 0) {
			return pos + 1;
		}
	}
	return optional_idx();
}

static bool IsJSONSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

//! Copies a document without its trailing commas (a comma after a value and before a closing bracket); false when it
//! has none
static bool RemoveTrailingCommas(const char *data, idx_t len, string &out) {
	bool found = false;
	char previous = '[';
	for (idx_t i = 0; i < len; i++) {
		const char c = data[i];
		if (IsJSONSpace(c)) {
			continue;
		}
		if (c == '"') {
			for (i++; i < len && data[i] != '"'; i++) {
				i += data[i] == '\\';
			}
		} else if (c == ',' && previous != '[' && previous != '{' && previous != ',' && previous != ':') {
			idx_t next = i + 1;
			while (next < len && IsJSONSpace(data[next])) {
				next++;
			}
			if (next < len && (data[next] == ']' || data[next] == '}')) {
				if (!found) {
					out.assign(data, len);
					found = true;
				}
				out[i] = ' ';
			}
		}
		previous = c;
	}
	return found;
}

static optional_idx ParseCleanJSON(JSONTreeParserState &st, JSONReadFlags flags, const char *data, idx_t len,
                                   idx_t readable, bool append, JSONParseError &error, optional_ptr<idx_t> consumed);

static optional_idx ParseInto(JSONTreeParserState &st, JSONReadFlags flags, const char *data, idx_t len, idx_t readable,
                              bool append, JSONParseError &error, optional_ptr<idx_t> consumed) {
	if (len >= 3 && memcmp(data, "\xEF\xBB\xBF", 3) == 0) {
		return JSONParseFailure(error, "byte order mark (BOM) is not supported", 0);
	}
	if (HasFlag(flags, JSONReadFlags::ALLOW_COMMENTS)) {
		auto unclosed = StripJSONComments(data, len, st.cleaned);
		if (unclosed.IsValid()) {
			return JSONParseFailure(error, "unclosed multiline comment", unclosed.GetIndex());
		}
		data = st.cleaned.data();
		len = st.cleaned.size();
		readable = len;
	}
	if (HasFlag(flags, JSONReadFlags::ALLOW_INVALID_UNICODE) && !simdutf::validate_utf8(data, len)) {
		string replaced;
		ReplaceInvalidJSONUnicode(data, len, replaced);
		st.cleaned = std::move(replaced);
		data = st.cleaned.data();
		len = st.cleaned.size();
		readable = len;
	}
	auto root = ParseCleanJSON(st, flags, data, len, readable, append, error, consumed);
	// simdjson accepts trailing commas only when built with SIMDJSON_ALLOW_TRAILING_COMMAS, which would apply to every
	// parser in the binary: a rejected document is parsed again without them
	if (!root.IsValid() && HasFlag(flags, JSONReadFlags::ALLOW_TRAILING_COMMAS) &&
	    RemoveTrailingCommas(data, len, st.uncommaed)) {
		JSONParseError retry;
		root = ParseCleanJSON(st, flags, st.uncommaed.data(), len, len, append, retry, consumed);
		error = retry;
	}
	return root;
}

static optional_idx ParseCleanJSON(JSONTreeParserState &st, JSONReadFlags flags, const char *data, idx_t len,
                                   idx_t readable, bool append, JSONParseError &error, optional_ptr<idx_t> consumed) {
	if (readable < len + JSONTreeParser::PADDING) {
		readable = len + JSONTreeParser::PADDING;
		if (readable > st.input.GetSize()) {
			st.input = st.buffers.allocator.Allocate(MaxValue<idx_t>(readable, st.input.GetSize() * 2));
		}
		auto input = char_ptr_cast(st.input.get());
		memcpy(input, data, len);
		memset(input + len, 0, JSONTreeParser::PADDING);
		data = input;
	}

	const idx_t root = st.nodes.size();
	st.frames.clear();
	JSONBuildContext context {flags, st.nodes, st.frames, append ? &st.strings : nullptr, data, data + len};
	const bool visitable = !consumed && !HasFlag(flags, JSONReadFlags::NUMBER_AS_RAW);
	if (visitable && len <= ondemand::parser::MAX_ONE_PASS_VISIT) {
		// simdjson reads a small document in one pass, without stage 1
		JSONNodeVisitor visitor(context, st.open, len);
		const auto code = st.parser.visit(simdjson::padded_string_view(data, len, readable), visitor);
		if (code == simdjson::MEMALLOC && st.buffers.error.HasError()) {
			auto error = std::move(st.buffers.error);
			st.buffers.error = ErrorData();
			error.Throw();
		}
		if (!code) {
			visitor.Finish();
			return root;
		}
		st.nodes.resize(root);
		st.frames.clear();
	}
	ondemand::document document;
	auto code = st.parser.iterate(data, len, readable).get(document);
	if (code == simdjson::MEMALLOC && st.buffers.error.HasError()) {
		auto error = std::move(st.buffers.error);
		st.buffers.error = ErrorData();
		error.Throw();
	}
	if (!code && visitable && len > ondemand::parser::MAX_ONE_PASS_VISIT) {
		JSONNodeVisitor visitor(context, st.open, len);
		if (!document.visit(visitor)) {
			visitor.Finish();
			return root;
		}
		// visit() rewinds the document
		st.nodes.resize(root);
	}
	const bool started = !code;
	std::string_view root_token;
	if (!code) {
		code = document.raw_json_token().get(root_token);
	}
	if (!code) {
		code = BuildJSONValue(document, context);
	}
	if (!code && consumed) {
		const char *end = root_token.data() + root_token.size();
		if (st.nodes[root].IsContainer()) {
			const char *location;
			end = document.at_end() || document.current_location().get(location) ? data + len : location;
		}
		*consumed = NumericCast<idx_t>(end - data);
	}
	if (!code && !consumed) {
		const bool trailing =
		    st.nodes[root].IsContainer() ? !document.at_end() : root_token.data() + root_token.size() < data + len;
		if (trailing) {
			code = simdjson::TRAILING_CONTENT;
		}
	}
	if (code == simdjson::INCOMPLETE_ARRAY_OR_OBJECT && !consumed) {
		const auto root_end = RootContainerEnd(data, len);
		if (root_end.IsValid() && root_end.GetIndex() < len) {
			st.nodes.resize(root);
			const auto cleaned =
			    static_cast<uint32_t>(JSONReadFlags::ALLOW_COMMENTS | JSONReadFlags::ALLOW_INVALID_UNICODE);
			const auto prefix_flags = static_cast<JSONReadFlags>(static_cast<uint32_t>(flags) & ~cleaned);
			auto prefix = ParseInto(st, prefix_flags, data, root_end.GetIndex(), readable, append, error, nullptr);
			if (!prefix.IsValid()) {
				return prefix;
			}
			st.nodes.resize(root);
			idx_t next = root_end.GetIndex();
			while (next < len &&
			       (data[next] == ' ' || data[next] == '\t' || data[next] == '\n' || data[next] == '\r')) {
				next++;
			}
			return JSONParseFailure(error, JSONErrorMessage(simdjson::TRAILING_CONTENT), next);
		}
	}
	if (code) {
		idx_t position = 0;
		const char *location;
		const bool scalar_root = !root_token.empty() && root_token[0] != '[' && root_token[0] != '{';
		if (code == simdjson::TRAILING_CONTENT && scalar_root) {
			const char *after_root = root_token.data() + root_token.size();
			while (after_root < data + len &&
			       (*after_root == ' ' || *after_root == '\t' || *after_root == '\n' || *after_root == '\r')) {
				after_root++;
			}
			position = NumericCast<idx_t>(after_root - data);
		} else if (started && !document.current_location().get(location)) {
			position = NumericCast<idx_t>(location - data);
		}
		code = DiagnoseJSONRoot(data, len, flags, code, position);
		st.nodes.resize(root);
		return JSONParseFailure(error, JSONErrorMessage(code), position);
	}
	return root;
}

//! Whether visit() may read the document: the inputs that ParseInto rewrites or reads as raw numbers go to it
static bool JSONVisitable(JSONReadFlags flags, const char *data, idx_t len) {
	constexpr auto rewritten = static_cast<uint32_t>(
	    JSONReadFlags::ALLOW_COMMENTS | JSONReadFlags::ALLOW_INVALID_UNICODE | JSONReadFlags::NUMBER_AS_RAW);
	return (static_cast<uint32_t>(flags) & rewritten) == 0 && !(len >= 3 && memcmp(data, "\xEF\xBB\xBF", 3) == 0);
}

//! Copies the document into the input buffer, followed by simdjson's padding
static const char *PadJSONInput(JSONTreeParserState &st, const char *data, idx_t len) {
	const idx_t readable = len + JSONTreeParser::PADDING;
	if (readable > st.input.GetSize()) {
		st.input = st.buffers.allocator.Allocate(MaxValue<idx_t>(readable, st.input.GetSize() * 2));
	}
	auto input = char_ptr_cast(st.input.get());
	memcpy(input, data, len);
	memset(input + len, 0, JSONTreeParser::PADDING);
	return input;
}

//! Starts simdjson on a padded document; rethrows the allocation failures of the buffer allocator
static simdjson::error_code IterateJSON(JSONTreeParserState &st, const char *input, idx_t len,
                                        ondemand::document &document) {
	const auto code = st.parser.iterate(input, len, len + JSONTreeParser::PADDING).get(document);
	if (code == simdjson::MEMALLOC && st.buffers.error.HasError()) {
		auto error = std::move(st.buffers.error);
		st.buffers.error = ErrorData();
		error.Throw();
	}
	return code;
}

//! Visits a padded document; simdjson reads documents of at most 32 bytes in one pass, without stage 1
template <class V>
static simdjson::error_code VisitJSON(JSONTreeParserState &st, const char *input, idx_t len, V &visitor) {
	const auto code = st.parser.visit(simdjson::padded_string_view(input, len, len + JSONTreeParser::PADDING), visitor);
	if (code == simdjson::MEMALLOC && st.buffers.error.HasError()) {
		auto error = std::move(st.buffers.error);
		st.buffers.error = ErrorData();
		error.Throw();
	}
	return code;
}

//! Starts On-Demand on a padded document. With VALIDATE_PATHS (the json_strict_paths setting), the whole document is
//! checked first; otherwise, as with On-Demand, only what the path functions read on the way to the values is.
static simdjson::error_code CheckAndIterateJSON(JSONTreeParserState &st, JSONReadFlags flags, const char *input,
                                                idx_t len, ondemand::document &document) {
	if (!HasFlag(flags, JSONReadFlags::VALIDATE_PATHS)) {
		return IterateJSON(st, input, len, document);
	}
	JSONCheckVisitor checker;
	if (len <= ondemand::parser::MAX_ONE_PASS_VISIT) {
		if (auto error = VisitJSON(st, input, len, checker)) {
			return error;
		}
		return IterateJSON(st, input, len, document);
	}
	if (auto error = IterateJSON(st, input, len, document)) {
		return error;
	}
	return document.visit(checker);
}

template <class VALUE>
static simdjson::error_code StepJSONPath(VALUE &value, const JSONPathStep &step, ondemand::value &child, bool &found) {
	found = false;
	ondemand::json_type type;
	if (auto error = value.type().get(type)) {
		return error;
	}
	if (step.is_key) {
		if (type != ondemand::json_type::object) {
			return simdjson::SUCCESS;
		}
		ondemand::object object;
		if (auto error = value.get_object().get(object)) {
			return error;
		}
		for (auto member : object) {
			ondemand::field field;
			if (auto error = std::move(member).get(field)) {
				return error;
			}
			// Unescaping shortens a key: a raw key of the same length matches only as it is, and a shorter one never
			const auto raw = field.escaped_key();
			bool equal = raw.size() == step.key.size() && raw == step.key && !step.escaped;
			if (raw.size() > step.key.size() && memchr(raw.data(), '\\', raw.size())) {
				std::string_view key;
				if (auto error = field.unescaped_key().get(key)) {
					return error;
				}
				equal = key == step.key;
			}
			if (equal) {
				child = field.value();
				found = true;
				return simdjson::SUCCESS;
			}
		}
		return simdjson::SUCCESS;
	}
	if (type != ondemand::json_type::array) {
		return simdjson::SUCCESS;
	}
	ondemand::array array;
	if (auto error = value.get_array().get(array)) {
		return error;
	}
	idx_t index = 0;
	for (auto element : array) {
		if (index++ == step.index) {
			found = true;
			return std::move(element).get(child);
		}
	}
	return simdjson::SUCCESS;
}

//! Builds the nodes of the value a path leads to, with the visitor that builds whole documents
static simdjson::error_code BuildJSONPathValue(ondemand::value &value, JSONBuildContext &context, vector<idx_t> &open) {
	JSONNodeVisitor visitor(context, open, 0);
	const auto error = value.visit(visitor);
	visitor.Finish();
	return error;
}

//! Moves through a document along a path that has steps; the On-Demand iterators skip the values that it does not go
//! through
static simdjson::error_code FindJSONPath(ondemand::document &document, const vector<JSONPathStep> &steps,
                                         ondemand::value &value, bool &found) {
	if (auto error = StepJSONPath(document, steps[0], value, found)) {
		return error;
	}
	for (idx_t i = 1; found && i < steps.size(); i++) {
		ondemand::value child;
		if (auto error = StepJSONPath(value, steps[i], child, found)) {
			return error;
		}
		value = child;
	}
	return simdjson::SUCCESS;
}

static const JSONNode *FollowJSONPath(const JSONNode *node, const vector<JSONPathStep> &steps) {
	for (idx_t i = 0; node && i < steps.size(); i++) {
		const auto &step = steps[i];
		if (step.is_key) {
			node = node->Get(step.key);
		} else {
			node = node->type == JSONValueType::ARRAY ? node->Get(step.index) : nullptr;
		}
	}
	return node;
}

bool JSONTreeParser::ParsePaths(const char *data, idx_t len, const vector<JSONPathStep> *paths, idx_t count,
                                const JSONNode **values, JSONParseError &error) {
	auto &st = *state;
	st.nodes.clear();
	if (JSONVisitable(flags, data, len)) {
		// On-Demand skips to the values and builds only them; with VALIDATE_PATHS, visit() first checks the whole
		// document. On any error, the document goes to Parse, for the exact result or error.
		const char *input = PadJSONInput(st, data, len);
		ondemand::document document;
		if (!CheckAndIterateJSON(st, flags, input, len, document)) {
			// rewinding resets the string buffer: the strings of earlier paths are copied
			const bool copy = count > 1;
			if (copy) {
				st.strings.Reset();
			}
			st.frames.clear();
			st.path_roots.resize(count);
			JSONBuildContext context {flags, st.nodes, st.frames, copy ? &st.strings : nullptr, input, input + len};
			bool built = true;
			for (idx_t i = 0; built && i < count; i++) {
				if (i > 0) {
					document.rewind();
				}
				ondemand::value value;
				bool found;
				st.path_roots[i] = optional_idx();
				if (FindJSONPath(document, paths[i], value, found)) {
					built = false;
				} else if (found) {
					st.path_roots[i] = st.nodes.size();
					built = !BuildJSONPathValue(value, context, st.open);
				}
			}
			if (built) {
				for (idx_t i = 0; i < count; i++) {
					const auto root = st.path_roots[i];
					values[i] = root.IsValid() ? st.nodes.data() + root.GetIndex() : nullptr;
				}
				return true;
			}
			st.nodes.clear();
		}
	}
	auto root = Parse(data, len, error);
	if (!root) {
		return false;
	}
	for (idx_t i = 0; i < count; i++) {
		values[i] = FollowJSONPath(root, paths[i]);
	}
	return true;
}

bool JSONTreeParser::Write(const char *data, idx_t len, JSONStringWriter &writer, JSONParseError &error) {
	auto &st = *state;
	writer.Clear();
	if (JSONVisitable(flags, data, len)) {
		const char *input = PadJSONInput(st, data, len);
		JSONWriteVisitor visitor(writer, input, len);
		if (!VisitJSON(st, input, len, visitor)) {
			return true;
		}
		writer.Clear();
	}
	auto root = Parse(data, len, error);
	if (!root) {
		return false;
	}
	writer.Value(*root);
	return true;
}

bool JSONTreeParser::WritePath(const char *data, idx_t len, const vector<JSONPathStep> &path, JSONStringWriter &writer,
                               bool &found, JSONParseError &error) {
	found = true;
	if (path.empty()) {
		return Write(data, len, writer, error);
	}
	auto &st = *state;
	writer.Clear();
	if (JSONVisitable(flags, data, len)) {
		// as ParsePaths, but the value goes to the writer instead of nodes
		const char *input = PadJSONInput(st, data, len);
		ondemand::document document;
		if (!CheckAndIterateJSON(st, flags, input, len, document)) {
			ondemand::value value;
			if (!FindJSONPath(document, path, value, found)) {
				if (!found) {
					return true;
				}
				JSONWriteVisitor visitor(writer, input, len);
				if (!value.visit(visitor)) {
					return true;
				}
			}
			writer.Clear();
		}
	}
	auto root = Parse(data, len, error);
	if (!root) {
		return false;
	}
	auto node = FollowJSONPath(root, path);
	found = node != nullptr;
	if (found) {
		writer.Value(*node);
	}
	return true;
}

bool JSONTreeParser::IsValid(const char *data, idx_t len, JSONParseError &error) {
	auto &st = *state;
	if (JSONVisitable(flags, data, len)) {
		const char *input = PadJSONInput(st, data, len);
		JSONCheckVisitor checker;
		if (!VisitJSON(st, input, len, checker)) {
			return true;
		}
	}
	return Parse(data, len, error) != nullptr;
}

const JSONNode *JSONTreeParser::Parse(const char *data, idx_t len) {
	JSONParseError error;
	auto root = Parse(data, len, error);
	if (!root) {
		throw InvalidInputException("Malformed JSON at byte %llu of input: %s", error.position, error.message);
	}
	return root;
}

JSONStringWriter::JSONStringWriter(JSONWriteFlags flags_p)
    : flags(flags_p), pretty(HasFlag(flags_p, JSONWriteFlags::PRETTY)) {
}

void JSONStringWriter::Clear() {
	used = 0;
	counts.clear();
	node_frames.clear();
	build_frames.clear();
	after_key = false;
}

void JSONStringWriter::Grow(idx_t n) {
	buffer.resize(MaxValue<idx_t>(MaxValue<idx_t>(buffer.size() * 2, used + n), 1024));
}

void JSONStringWriter::Indent(idx_t depth) {
	static constexpr char SPACES[] = "                                                                ";
	const idx_t spaces = depth * 4;
	auto out = Reserve(1 + spaces);
	*out = '\n';
	if (spaces <= sizeof(SPACES) - 1) {
		CopyShort(out + 1, SPACES, spaces);
	} else {
		memset(out + 1, ' ', spaces);
	}
	used += 1 + spaces;
}

void JSONStringWriter::BeforeValue() {
	if (after_key) {
		after_key = false;
		return;
	}
	if (counts.empty()) {
		return;
	}
	if (counts.back()++ > 0) {
		Put(',');
	}
	if (pretty) {
		Indent(counts.size());
	}
}

void JSONStringWriter::PutUnsignedInteger(uint64_t value) {
	auto out = Reserve(20);
	used += NumericCast<idx_t>(std::to_chars(out, out + 20, value).ptr - out);
}

void JSONStringWriter::PutSignedInteger(int64_t value) {
	auto out = Reserve(21);
	used += NumericCast<idx_t>(std::to_chars(out, out + 21, value).ptr - out);
}

void JSONStringWriter::PutDouble(double value) {
	auto out = Reserve(MAX_DOUBLE_LENGTH);
	auto end = WriteDouble(out, value, flags);
	if (!end) {
		throw SerializationException("Failed to write JSON: NaN and Infinity are not allowed");
	}
	used += NumericCast<idx_t>(end - out);
}

void JSONStringWriter::PutString(std::string_view value, bool verbatim) {
	if (verbatim) {
		PutQuoted(value);
		return;
	}
	auto out = Reserve(MaxStringLength(value.size()));
	auto end = WriteString(out, value, flags);
	if (!end) {
		throw SerializationException("Failed to write JSON: a string is not valid UTF-8");
	}
	used += NumericCast<idx_t>(end - out);
}

void JSONStringWriter::PutKey(std::string_view key, bool verbatim) {
	auto out = Reserve(MaxStringLength(key.size()) + 2);
	char *end;
	if (verbatim) {
		end = WriteQuoted(out, key);
	} else {
		end = WriteString(out, key, flags);
		if (!end) {
			throw SerializationException("Failed to write JSON: a key is not valid UTF-8");
		}
	}
	*end++ = ':';
	if (pretty) {
		*end++ = ' ';
	}
	used += NumericCast<idx_t>(end - out);
}

void JSONStringWriter::PutSeparator(bool first, idx_t depth) {
	if (!first) {
		Put(',');
	}
	if (pretty) {
		Indent(counts.size() + depth);
	}
}

void JSONStringWriter::PutClose(bool object, bool empty, idx_t depth) {
	if (pretty && !empty) {
		Indent(counts.size() + depth);
	}
	Put(object ? '}' : ']');
}

void JSONStringWriter::Null() {
	BeforeValue();
	Put("null", 4);
}

void JSONStringWriter::Boolean(bool value) {
	BeforeValue();
	if (value) {
		Put("true", 4);
	} else {
		Put("false", 5);
	}
}

void JSONStringWriter::UnsignedInteger(uint64_t value) {
	BeforeValue();
	PutUnsignedInteger(value);
}

void JSONStringWriter::SignedInteger(int64_t value) {
	BeforeValue();
	PutSignedInteger(value);
}

void JSONStringWriter::Double(double value) {
	BeforeValue();
	PutDouble(value);
}

void JSONStringWriter::Raw(std::string_view value) {
	BeforeValue();
	Put(value.data(), value.size());
}

void JSONStringWriter::String(std::string_view value) {
	BeforeValue();
	PutString(value, false);
}

void JSONStringWriter::VerbatimString(std::string_view value) {
	BeforeValue();
	PutQuoted(value);
}

void JSONStringWriter::StartObject() {
	BeforeValue();
	Put('{');
	counts.push_back(0);
}

void JSONStringWriter::Key(std::string_view key, bool verbatim) {
	if (counts.back()++ > 0) {
		Put(',');
	}
	if (pretty) {
		Indent(counts.size());
	}
	PutKey(key, verbatim);
	after_key = true;
}

void JSONStringWriter::EndObject() {
	const auto count = counts.back();
	counts.pop_back();
	if (pretty && count > 0) {
		Indent(counts.size());
	}
	Put('}');
}

void JSONStringWriter::StartArray() {
	BeforeValue();
	Put('[');
	counts.push_back(0);
}

void JSONStringWriter::EndArray() {
	const auto count = counts.back();
	counts.pop_back();
	if (pretty && count > 0) {
		Indent(counts.size());
	}
	Put(']');
}

void JSONStringWriter::Value(const JSONNode &root) {
	// The containers of the tree keep their own frames: the separators and indentation follow the depth below the
	// root, on top of the containers that the writer already has open
	BeforeValue();
	const auto base = node_frames.size();
	auto node = &root;
	while (true) {
		switch (node->type) {
		case JSONValueType::JSON_NULL:
			Put("null", 4);
			break;
		case JSONValueType::BOOLEAN:
			if (node->boolean) {
				Put("true", 4);
			} else {
				Put("false", 5);
			}
			break;
		case JSONValueType::UNSIGNED_INTEGER:
			PutUnsignedInteger(node->unsigned_integer);
			break;
		case JSONValueType::SIGNED_INTEGER:
			PutSignedInteger(node->signed_integer);
			break;
		case JSONValueType::DOUBLE:
			PutDouble(node->real);
			break;
		case JSONValueType::RAW:
			Put(node->str, node->size);
			break;
		case JSONValueType::STRING:
			PutString(node->GetString(), node->boolean);
			break;
		case JSONValueType::ARRAY:
		case JSONValueType::OBJECT: {
			const bool object = node->type == JSONValueType::OBJECT;
			Put(object ? '{' : '[');
			node_frames.push_back({object, true, node->size, node->First()});
			break;
		}
		default:
			throw InternalException("Cannot write an invalid JSON value");
		}
		while (true) {
			if (node_frames.size() == base) {
				return;
			}
			auto &frame = node_frames.back();
			if (frame.remaining == 0) {
				const bool object = frame.object;
				const bool empty = frame.first;
				node_frames.pop_back();
				PutClose(object, empty, node_frames.size() - base);
				continue;
			}
			frame.remaining--;
			PutSeparator(frame.first, node_frames.size() - base);
			frame.first = false;
			node = frame.next;
			if (frame.object) {
				PutKey(node->GetString(), node->boolean);
				node++;
			}
			frame.next = node->Next();
			break;
		}
	}
}

void JSONStringWriter::Value(const JSONBuildValue &root) {
	// as Value(const JSONNode &)
	BeforeValue();
	const auto base = build_frames.size();
	auto value = &root;
	while (true) {
		switch (value->type) {
		case JSONValueType::JSON_NULL:
			Put("null", 4);
			break;
		case JSONValueType::BOOLEAN:
			if (value->boolean) {
				Put("true", 4);
			} else {
				Put("false", 5);
			}
			break;
		case JSONValueType::UNSIGNED_INTEGER:
			PutUnsignedInteger(value->unsigned_integer);
			break;
		case JSONValueType::SIGNED_INTEGER:
			PutSignedInteger(value->signed_integer);
			break;
		case JSONValueType::DOUBLE:
			PutDouble(value->real);
			break;
		case JSONValueType::STRING:
			PutString(std::string_view(value->str, value->size), false);
			break;
		case JSONValueType::RAW:
			Put(value->str, value->size);
			break;
		case JSONValueType::ARRAY:
		case JSONValueType::OBJECT: {
			const bool object = value->type == JSONValueType::OBJECT;
			Put(object ? '{' : '[');
			build_frames.push_back({object, true, value->size, value->last ? value->last->next : nullptr});
			break;
		}
		default:
			throw InternalException("Cannot write an invalid JSON value");
		}
		while (true) {
			if (build_frames.size() == base) {
				return;
			}
			auto &frame = build_frames.back();
			if (frame.remaining == 0) {
				const bool object = frame.object;
				const bool empty = frame.first;
				build_frames.pop_back();
				PutClose(object, empty, build_frames.size() - base);
				continue;
			}
			frame.remaining--;
			PutSeparator(frame.first, build_frames.size() - base);
			frame.first = false;
			value = frame.next;
			frame.next = value->next;
			if (frame.object) {
				PutKey(std::string_view(value->key, value->key_size), value->key_verbatim);
			}
			break;
		}
	}
}

char *JSONStringWriter::WriteDouble(char *out, double value, JSONWriteFlags flags) {
	if (std::isnan(value) || std::isinf(value)) {
		if (!HasFlag(flags, JSONWriteFlags::ALLOW_INF_AND_NAN)) {
			return nullptr;
		}
		const std::string_view text = std::isnan(value) ? "NaN" : value < 0 ? "-Infinity" : "Infinity";
		memcpy(out, text.data(), text.size());
		return out + text.size();
	}
	if (std::signbit(value)) {
		*out++ = '-';
		value = -value;
	}
	if (value == 0) {
		memcpy(out, "0.0", 3);
		return out + 3;
	}
	// simdjson's to_chars writes the shortest digits that round-trip (as Ryu). In fixed notation, from 1e-4 to 1e15,
	// its text is ours: digits, a decimal point, and at least one digit after it
	if (value >= 1e-4 && value < 1e15) {
		return simdjson::internal::to_chars(out, out + simdjson::internal::to_chars_buffer_size, value);
	}
	// elsewhere, take the digits and the position of the decimal point, then lay them out as JavaScript does
	char text[simdjson::internal::to_chars_buffer_size];
	const char *end = simdjson::internal::to_chars(text, text + sizeof(text), value);
	char digits[24];
	idx_t digit_count = 0;
	int before_point = -1;
	const char *pos = text;
	for (; pos < end && *pos != 'e'; pos++) {
		if (*pos == '.') {
			before_point = static_cast<int>(digit_count);
		} else {
			digits[digit_count++] = *pos;
		}
	}
	if (before_point < 0) {
		before_point = static_cast<int>(digit_count);
	}
	int exponent = 0;
	if (pos < end) {
		pos++;
		const bool negative_exponent = *pos == '-';
		pos += *pos == '-' || *pos == '+';
		for (; pos < end; pos++) {
			exponent = exponent * 10 + (*pos - '0');
		}
		exponent = negative_exponent ? -exponent : exponent;
	}
	idx_t first = 0;
	while (first < digit_count && digits[first] == '0') {
		first++;
		before_point--;
	}
	while (digit_count > first && digits[digit_count - 1] == '0') {
		digit_count--;
	}
	const char *significant = digits + first;
	digit_count -= first;
	const int dot = before_point + exponent;
	if (-6 < dot && dot <= 21) {
		if (dot > 0) {
			const auto whole = static_cast<idx_t>(dot);
			if (digit_count <= whole) {
				memcpy(out, significant, digit_count);
				out += digit_count;
				memset(out, '0', whole - digit_count);
				out += whole - digit_count;
				memcpy(out, ".0", 2);
				return out + 2;
			}
			memcpy(out, significant, whole);
			out += whole;
			*out++ = '.';
			memcpy(out, significant + whole, digit_count - whole);
			return out + (digit_count - whole);
		}
		memcpy(out, "0.", 2);
		out += 2;
		memset(out, '0', static_cast<idx_t>(-dot));
		out += -dot;
		memcpy(out, significant, digit_count);
		return out + digit_count;
	}
	*out++ = significant[0];
	if (digit_count > 1) {
		*out++ = '.';
		memcpy(out, significant + 1, digit_count - 1);
		out += digit_count - 1;
	}
	*out++ = 'e';
	return std::to_chars(out, out + 8, dot - 1).ptr;
}

bool JSONStringWriter::WriteDouble(string &out, double value, JSONWriteFlags flags) {
	char text[MAX_DOUBLE_LENGTH];
	auto end = WriteDouble(text, value, flags);
	if (!end) {
		return false;
	}
	out.append(text, NumericCast<idx_t>(end - text));
	return true;
}

//! Writes one byte of a string, escaped as JSON needs: \u00XX in uppercase where there is no short escape
static inline char *EscapeJSONByte(char *out, uint8_t c) {
	static constexpr char HEX[] = "0123456789ABCDEF";
	if (c >= 0x20 && c != '"' && c != '\\') {
		*out++ = static_cast<char>(c);
		return out;
	}
	*out++ = '\\';
	switch (c) {
	case '"':
		*out++ = '"';
		break;
	case '\\':
		*out++ = '\\';
		break;
	case '\b':
		*out++ = 'b';
		break;
	case '\f':
		*out++ = 'f';
		break;
	case '\n':
		*out++ = 'n';
		break;
	case '\r':
		*out++ = 'r';
		break;
	case '\t':
		*out++ = 't';
		break;
	default:
		memcpy(out, "u00", 3);
		out[3] = HEX[c >> 4];
		out[4] = HEX[c & 0xF];
		out += 5;
		break;
	}
	return out;
}

static constexpr uint64_t JSON_ONES = 0x0101010101010101ULL;
static constexpr uint64_t JSON_HIGH_BITS = 0x8080808080808080ULL;

//! The high bit of each byte of the word that is below 0x20, a quote or a backslash (and maybe of a byte after one)
static inline uint64_t SpecialJSONBytes(uint64_t word) {
	const uint64_t quote = word ^ (JSON_ONES * '"');
	const uint64_t backslash = word ^ (JSON_ONES * '\\');
	return (((word - JSON_ONES * 0x20) & ~word) | ((quote - JSON_ONES) & ~quote) |
	        ((backslash - JSON_ONES) & ~backslash)) &
	       JSON_HIGH_BITS;
}

//! Escapes a short string eight bytes at a time, and tells whether it holds bytes beyond ASCII. Writes up to eight
//! bytes past its end.
static char *EscapeShortJSONString(char *out, std::string_view value, bool &ascii) {
	const auto data = reinterpret_cast<const uint8_t *>(value.data());
	const idx_t size = value.size();
	if (size < 8 && std::endian::native == std::endian::little) {
		// padded with a letter, which needs no escape; fixed-size loads, as a memcpy of a variable length is a call
		uint64_t word = JSON_ONES * 'a';
		if (size >= 4) {
			uint32_t low;
			uint32_t high;
			memcpy(&low, data, 4);
			memcpy(&high, data + size - 4, 4);
			word = (word << (8 * size)) | (uint64_t(high) << (8 * (size - 4))) | low;
		} else if (size > 0) {
			const uint64_t bytes = uint64_t(data[0]) | uint64_t(data[size >> 1]) << (8 * (size >> 1)) |
			                       uint64_t(data[size - 1]) << (8 * (size - 1));
			word = (word << (8 * size)) | bytes;
		}
		ascii = (word & JSON_HIGH_BITS) == 0;
		if (!SpecialJSONBytes(word)) {
			memcpy(out, &word, 8);
			return out + size;
		}
		for (idx_t i = 0; i < size; i++) {
			out = EscapeJSONByte(out, data[i]);
		}
		return out;
	}
	uint64_t high = 0;
	idx_t i = 0;
	for (; i + 8 <= size; i += 8) {
		uint64_t word;
		memcpy(&word, data + i, 8);
		memcpy(out, &word, 8);
		high |= word;
		if (!SpecialJSONBytes(word)) {
			out += 8;
			continue;
		}
		for (idx_t k = i; k < i + 8; k++) {
			out = EscapeJSONByte(out, data[k]);
		}
	}
	if (i < size && size >= 8) {
		// the last eight bytes overlap the ones written: when none needs an escape, those were copied as they are
		const idx_t rest = size - i;
		uint64_t word;
		memcpy(&word, data + size - 8, 8);
		high |= word;
		if (!SpecialJSONBytes(word)) {
			memcpy(out + rest - 8, &word, 8);
			out += rest;
		} else {
			for (; i < size; i++) {
				out = EscapeJSONByte(out, data[i]);
			}
		}
	}
	for (; i < size && size < 8; i++) {
		high |= data[i];
		out = EscapeJSONByte(out, data[i]);
	}
	ascii = (high & JSON_HIGH_BITS) == 0;
	return out;
}

bool JSONStringWriter::IsVerbatim(std::string_view value) {
	for (auto c : value) {
		const auto byte = static_cast<uint8_t>(c);
		if (byte < 0x20 || byte >= 0x80 || byte == '"' || byte == '\\') {
			return false;
		}
	}
	return true;
}

char *JSONStringWriter::WriteString(char *out, std::string_view value, JSONWriteFlags flags) {
	const bool validate = !HasFlag(flags, JSONWriteFlags::ALLOW_INVALID_UNICODE);
	*out++ = '"';
	if (value.size() < 64) {
		bool ascii;
		out = EscapeShortJSONString(out, value, ascii);
		if (validate && !ascii && !simdutf::validate_utf8(value.data(), value.size())) {
			return nullptr;
		}
		*out++ = '"';
		return out;
	}
	if (validate && !simdutf::validate_utf8(value.data(), value.size())) {
		return nullptr;
	}
	const auto written = simdjson::internal::escape_best::write_string_escaped(value, out);
	if (written != value.size()) {
		// simdjson writes the \u00XX escapes of control characters in lowercase, DuckDB in uppercase
		for (idx_t i = 0; i < written; i++) {
			if (out[i] != '\\') {
				continue;
			}
			if (out[i + 1] == 'u') {
				for (idx_t h = i + 2; h < i + 6; h++) {
					if (out[h] >= 'a' && out[h] <= 'f') {
						out[h] = static_cast<char>(out[h] - 'a' + 'A');
					}
				}
				i += 5;
			} else {
				i++;
			}
		}
	}
	out += written;
	*out++ = '"';
	return out;
}

bool JSONStringWriter::WriteString(string &out, std::string_view value, JSONWriteFlags flags) {
	const auto start = out.size();
	out.resize(start + MaxStringLength(value.size()));
	auto end = WriteString(&out[start], value, flags);
	if (!end) {
		out.resize(start);
		return false;
	}
	out.resize(NumericCast<idx_t>(end - out.data()));
	return true;
}

JSONBuilder::JSONBuilder(ArenaAllocator &arena_p) : arena(arena_p) {
}

JSONBuildValue *JSONBuilder::Make(JSONValueType type) {
	auto value = reinterpret_cast<JSONBuildValue *>(arena.AllocateAligned(sizeof(JSONBuildValue)));
	value->type = type;
	value->boolean = false;
	value->key_verbatim = false;
	value->size = 0;
	value->key_size = 0;
	value->unsigned_integer = 0;
	value->next = nullptr;
	value->key = nullptr;
	return value;
}

const char *JSONBuilder::CopyString(std::string_view value) {
	if (value.empty()) {
		return "";
	}
	auto copy = arena.Allocate(value.size());
	memcpy(copy, value.data(), value.size());
	return const_char_ptr_cast(copy);
}

JSONBuildValue *JSONBuilder::Null() {
	return Make(JSONValueType::JSON_NULL);
}

JSONBuildValue *JSONBuilder::Boolean(bool value) {
	auto result = Make(JSONValueType::BOOLEAN);
	result->boolean = value;
	return result;
}

JSONBuildValue *JSONBuilder::UnsignedInteger(uint64_t value) {
	auto result = Make(JSONValueType::UNSIGNED_INTEGER);
	result->unsigned_integer = value;
	return result;
}

JSONBuildValue *JSONBuilder::SignedInteger(int64_t value) {
	auto result = Make(JSONValueType::SIGNED_INTEGER);
	result->signed_integer = value;
	return result;
}

JSONBuildValue *JSONBuilder::Double(double value) {
	auto result = Make(JSONValueType::DOUBLE);
	result->real = value;
	return result;
}

JSONBuildValue *JSONBuilder::String(std::string_view value) {
	auto result = Make(JSONValueType::STRING);
	result->size = NumericCast<uint32_t>(value.size());
	result->str = CopyString(value);
	return result;
}

JSONBuildValue *JSONBuilder::Raw(std::string_view value) {
	auto result = Make(JSONValueType::RAW);
	result->size = NumericCast<uint32_t>(value.size());
	result->str = CopyString(value);
	return result;
}

JSONBuildValue *JSONBuilder::Object() {
	auto result = Make(JSONValueType::OBJECT);
	result->last = nullptr;
	return result;
}

JSONBuildValue *JSONBuilder::Array() {
	auto result = Make(JSONValueType::ARRAY);
	result->last = nullptr;
	return result;
}

static void AppendJSONChild(JSONBuildValue &container, JSONBuildValue *value) {
	if (container.last) {
		value->next = container.last->next;
		container.last->next = value;
	} else {
		value->next = value;
	}
	container.last = value;
	container.size++;
}

void JSONBuilder::Add(JSONBuildValue &object, std::string_view key, JSONBuildValue *value, bool verbatim) {
	D_ASSERT(object.type == JSONValueType::OBJECT);
	value->key = key.data();
	value->key_size = NumericCast<uint32_t>(key.size());
	value->key_verbatim = verbatim;
	AppendJSONChild(object, value);
}

void JSONBuilder::Append(JSONBuildValue &array, JSONBuildValue *value) {
	D_ASSERT(array.type == JSONValueType::ARRAY);
	AppendJSONChild(array, value);
}

JSONBuildValue *JSONBuilder::Copy(const JSONNode &root) {
	const auto base = frames.size();
	JSONBuildValue *result = nullptr;
	std::string_view key;
	auto node = &root;
	while (true) {
		JSONBuildValue *value;
		switch (node->type) {
		case JSONValueType::JSON_NULL:
			value = Null();
			break;
		case JSONValueType::BOOLEAN:
			value = Boolean(node->boolean);
			break;
		case JSONValueType::UNSIGNED_INTEGER:
			value = UnsignedInteger(node->unsigned_integer);
			break;
		case JSONValueType::SIGNED_INTEGER:
			value = SignedInteger(node->signed_integer);
			break;
		case JSONValueType::DOUBLE:
			value = Double(node->real);
			break;
		case JSONValueType::STRING:
			value = String(node->GetString());
			break;
		case JSONValueType::RAW:
			value = Raw(node->GetString());
			break;
		case JSONValueType::ARRAY:
			value = Array();
			break;
		case JSONValueType::OBJECT:
			value = Object();
			break;
		default:
			frames.resize(base);
			throw InternalException("Cannot copy an invalid JSON value");
		}
		if (frames.size() == base) {
			result = value;
		} else if (frames.back().container->type == JSONValueType::OBJECT) {
			Add(*frames.back().container, key, value);
		} else {
			Append(*frames.back().container, value);
		}
		if (node->IsContainer()) {
			frames.push_back({value, node->size, node->First()});
		}
		while (true) {
			if (frames.size() == base) {
				return result;
			}
			auto &frame = frames.back();
			if (frame.remaining == 0) {
				frames.pop_back();
				continue;
			}
			frame.remaining--;
			node = frame.next;
			if (frame.container->type == JSONValueType::OBJECT) {
				key = std::string_view(CopyString(node->GetString()), node->size);
				node++;
			}
			frame.next = node->Next();
			break;
		}
	}
}

JSONValue::JSONValue() : node(nullptr) {
}

JSONValue::JSONValue(const JSONNode *node_p) : node(node_p) {
}

bool JSONValue::IsValid() const {
	return node != nullptr;
}

JSONValueType JSONValue::GetType() const {
	return node ? node->type : JSONValueType::INVALID;
}

bool JSONValue::IsNull() const {
	return node && node->type == JSONValueType::JSON_NULL;
}

bool JSONValue::IsString() const {
	return node && node->type == JSONValueType::STRING;
}

bool JSONValue::IsArray() const {
	return node && node->type == JSONValueType::ARRAY;
}

bool JSONValue::IsObject() const {
	return node && node->type == JSONValueType::OBJECT;
}

bool JSONValue::IsInteger() const {
	return node && node->IsInteger();
}

bool JSONValue::IsNumber() const {
	return node && node->IsNumber();
}

string JSONValue::GetString() const {
	if (!node || (node->type != JSONValueType::STRING && node->type != JSONValueType::RAW)) {
		return string();
	}
	return string(node->GetString());
}

bool JSONValue::GetBoolean() const {
	return node && node->type == JSONValueType::BOOLEAN && node->boolean;
}

uint64_t JSONValue::GetUnsignedInteger() const {
	return node->unsigned_integer;
}

int64_t JSONValue::GetSignedInteger() const {
	return node->signed_integer;
}

double JSONValue::GetDouble() const {
	return node->real;
}

double JSONValue::GetNumber() const {
	return node ? node->GetNumber() : 0;
}

JSONValue JSONValue::GetMember(std::string_view key) const {
	return JSONValue(node ? node->Get(key) : nullptr);
}

void JSONValue::IterateArray(const std::function<void(JSONValue)> &callback) const {
	if (!IsArray()) {
		return;
	}
	auto element = node->First();
	for (uint32_t i = 0; i < node->size; i++) {
		callback(JSONValue(element));
		element = element->Next();
	}
}

void JSONValue::IterateObject(const std::function<void(const string &, JSONValue)> &callback) const {
	if (!IsObject()) {
		return;
	}
	auto member = node->First();
	for (uint32_t i = 0; i < node->size; i++) {
		callback(string(member->GetString()), JSONValue(member + 1));
		member = (member + 1)->Next();
	}
}

string JSONValue::ToString(JSONWriteFlags flags) const {
	if (!node) {
		throw SerializationException("Failed to write JSON string");
	}
	JSONStringWriter writer(flags);
	writer.Value(*node);
	return writer.ToString();
}

JSONDocument::JSONDocument() = default;
JSONDocument::~JSONDocument() = default;
JSONDocument::JSONDocument(JSONDocument &&other) noexcept = default;
JSONDocument &JSONDocument::operator=(JSONDocument &&other) noexcept = default;

unique_ptr<JSONDocument> JSONDocument::TryParse(const char *data, idx_t len, JSONParseError &error,
                                                JSONReadFlags flags) {
	auto parser = make_uniq<JSONTreeParser>(flags);
	auto root = parser->Parse(data, len, error);
	if (!root) {
		return nullptr;
	}
	auto result = make_uniq<JSONDocument>();
	result->parser = std::move(parser);
	result->root = root;
	return result;
}

unique_ptr<JSONDocument> JSONDocument::Parse(const char *data, idx_t len, JSONReadFlags flags) {
	JSONParseError error;
	auto result = TryParse(data, len, error, flags);
	if (!result) {
		throw InvalidInputException("Failed to parse JSON at byte %llu of input: %s", error.position, error.message);
	}
	return result;
}

JSONValue JSONDocument::GetRoot() const {
	return JSONValue(root);
}

string JSONDocument::ToString(JSONWriteFlags flags) const {
	return GetRoot().ToString(flags);
}

idx_t JSONMutableDocument::Add(JSONMutableNode node) {
	nodes.push_back(std::move(node));
	return nodes.size() - 1;
}

void JSONMutableDocument::Write(JSONStringWriter &out, idx_t index) const {
	auto &node = nodes[index];
	switch (node.type) {
	case JSONValueType::JSON_NULL:
		out.Null();
		return;
	case JSONValueType::BOOLEAN:
		out.Boolean(node.boolean);
		return;
	case JSONValueType::UNSIGNED_INTEGER:
		out.UnsignedInteger(node.unsigned_integer);
		return;
	case JSONValueType::SIGNED_INTEGER:
		out.SignedInteger(node.signed_integer);
		return;
	case JSONValueType::DOUBLE:
		out.Double(node.real);
		return;
	case JSONValueType::RAW:
		out.Raw(node.text);
		return;
	case JSONValueType::STRING:
		out.String(node.text);
		return;
	case JSONValueType::ARRAY:
		out.StartArray();
		for (auto &child : node.children) {
			Write(out, child.second);
		}
		out.EndArray();
		return;
	case JSONValueType::OBJECT:
		out.StartObject();
		for (auto &child : node.children) {
			out.Key(child.first);
			Write(out, child.second);
		}
		out.EndObject();
		return;
	default:
		throw InternalException("Cannot write an invalid JSON value");
	}
}

JSONMutableValue::JSONMutableValue() : doc(nullptr), index(0) {
}

JSONMutableValue::JSONMutableValue(JSONMutableDocument *doc_p, idx_t index_p) : doc(doc_p), index(index_p) {
}

bool JSONMutableValue::IsValid() const {
	return doc != nullptr;
}

void JSONMutableValue::Add(std::string_view key, JSONMutableValue value) {
	doc->nodes[index].children.emplace_back(string(key), value.index);
}

void JSONMutableValue::AddString(std::string_view key, std::string_view value) {
	JSONMutableNode node;
	node.type = JSONValueType::STRING;
	node.text = string(value);
	const auto child = doc->Add(std::move(node));
	doc->nodes[index].children.emplace_back(string(key), child);
}

void JSONMutableValue::Append(JSONMutableValue value) {
	doc->nodes[index].children.emplace_back(string(), value.index);
}

void JSONMutableValue::AppendString(std::string_view value) {
	JSONMutableNode node;
	node.type = JSONValueType::STRING;
	node.text = string(value);
	const auto child = doc->Add(std::move(node));
	doc->nodes[index].children.emplace_back(string(), child);
}

JSONWriter::JSONWriter() : doc(make_uniq<JSONMutableDocument>()) {
}

JSONWriter::~JSONWriter() = default;
JSONWriter::JSONWriter(JSONWriter &&other) noexcept = default;
JSONWriter &JSONWriter::operator=(JSONWriter &&other) noexcept = default;

JSONMutableValue JSONWriter::Create(JSONMutableNode node) {
	return JSONMutableValue(doc.get(), doc->Add(std::move(node)));
}

JSONMutableValue JSONWriter::CreateObject() {
	JSONMutableNode node;
	node.type = JSONValueType::OBJECT;
	return Create(std::move(node));
}

JSONMutableValue JSONWriter::CreateArray() {
	JSONMutableNode node;
	node.type = JSONValueType::ARRAY;
	return Create(std::move(node));
}

JSONMutableValue JSONWriter::CreateString(std::string_view value) {
	JSONMutableNode node;
	node.type = JSONValueType::STRING;
	node.text = string(value);
	return Create(std::move(node));
}

JSONMutableValue JSONWriter::CreateNull() {
	return Create(JSONMutableNode());
}

JSONMutableValue JSONWriter::CreateBoolean(bool value) {
	JSONMutableNode node;
	node.type = JSONValueType::BOOLEAN;
	node.boolean = value;
	return Create(std::move(node));
}

JSONMutableValue JSONWriter::CreateUnsignedInteger(uint64_t value) {
	JSONMutableNode node;
	node.type = JSONValueType::UNSIGNED_INTEGER;
	node.unsigned_integer = value;
	return Create(std::move(node));
}

JSONMutableValue JSONWriter::CreateSignedInteger(int64_t value) {
	JSONMutableNode node;
	node.type = JSONValueType::SIGNED_INTEGER;
	node.signed_integer = value;
	return Create(std::move(node));
}

JSONMutableValue JSONWriter::CreateDouble(double value) {
	JSONMutableNode node;
	node.type = JSONValueType::DOUBLE;
	node.real = value;
	return Create(std::move(node));
}

static idx_t CopyJSONNode(JSONMutableDocument &doc, const JSONNode &source) {
	JSONMutableNode node;
	node.type = source.type;
	switch (source.type) {
	case JSONValueType::BOOLEAN:
		node.boolean = source.boolean;
		break;
	case JSONValueType::UNSIGNED_INTEGER:
		node.unsigned_integer = source.unsigned_integer;
		break;
	case JSONValueType::SIGNED_INTEGER:
		node.signed_integer = source.signed_integer;
		break;
	case JSONValueType::DOUBLE:
		node.real = source.real;
		break;
	case JSONValueType::STRING:
	case JSONValueType::RAW:
		node.text = string(source.GetString());
		break;
	default:
		break;
	}
	const auto index = doc.Add(std::move(node));
	if (source.type == JSONValueType::ARRAY) {
		auto element = source.First();
		for (uint32_t i = 0; i < source.size; i++) {
			const auto child = CopyJSONNode(doc, *element);
			doc.nodes[index].children.emplace_back(string(), child);
			element = element->Next();
		}
	} else if (source.type == JSONValueType::OBJECT) {
		auto member = source.First();
		for (uint32_t i = 0; i < source.size; i++) {
			const auto child = CopyJSONNode(doc, *(member + 1));
			doc.nodes[index].children.emplace_back(string(member->GetString()), child);
			member = (member + 1)->Next();
		}
	}
	return index;
}

JSONMutableValue JSONWriter::CreateCopy(const JSONValue &value) {
	if (!value.IsValid()) {
		return CreateNull();
	}
	return JSONMutableValue(doc.get(), CopyJSONNode(*doc, *value.GetNode()));
}

void JSONWriter::SetRoot(JSONMutableValue value) {
	doc->root = value.index;
}

string JSONWriter::ToString(JSONWriteFlags flags) const {
	if (!doc->root.IsValid()) {
		throw SerializationException("Failed to write JSON string");
	}
	JSONStringWriter out(flags);
	doc->Write(out, doc->root.GetIndex());
	return out.ToString();
}

} // namespace duckdb
