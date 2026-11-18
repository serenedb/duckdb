#include "duckdb/common/json_document.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "fast_float/fast_float.h"
#include "simdutf.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <simdjson.h>

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
		parser.allocator = &buffers.callbacks;
	}

	JSONParserBuffers buffers;
	ondemand::parser parser;
	AllocatedData input;
	string cleaned;
	JSONNodes nodes;
	vector<JSONBuildFrame> frames;
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

	JSONNode MakeString(JSONValueType type, std::string_view text) {
		auto node = MakeJSONNode(type);
		node.size = NumericCast<uint32_t>(text.size());
		node.str = text.data();
		if (keep && !text.empty()) {
			auto copy = keep->Allocate(text.size());
			memcpy(copy, text.data(), text.size());
			node.str = const_char_ptr_cast(copy);
		}
		return node;
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
			return simdjson::N_ATOM_ERROR;
		}
		nodes.push_back(MakeJSONNode(JSONValueType::JSON_NULL));
		return simdjson::SUCCESS;
	}
	default:
		return simdjson::TAPE_ERROR;
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
	state->parser.allow_trailing_commas = HasFlag(flags, JSONReadFlags::ALLOW_TRAILING_COMMAS);
	state->parser.allow_inf_and_nan = HasFlag(flags, JSONReadFlags::ALLOW_INF_AND_NAN);
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
	JSONBuildContext context {flags, st.nodes, st.frames, append ? &st.strings : nullptr};
	ondemand::document document;
	auto code = st.parser.iterate(data, len, readable).get(document);
	if (code == simdjson::MEMALLOC && st.buffers.error.HasError()) {
		auto error = std::move(st.buffers.error);
		st.buffers.error = ErrorData();
		error.Throw();
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
	} else if (!code) {
		const bool trailing =
		    st.nodes[root].IsContainer() ? !document.at_end() : root_token.data() + root_token.size() < data + len;
		if (trailing) {
			code = simdjson::TRAILING_CONTENT;
		}
	}
	if (code) {
		idx_t position = 0;
		const char *location;
		if (started && !document.current_location().get(location)) {
			position = NumericCast<idx_t>(location - data);
		}
		code = DiagnoseJSONRoot(data, len, flags, code, position);
		st.nodes.resize(root);
		return JSONParseFailure(error, JSONErrorMessage(code), position);
	}
	return root;
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
	buffer.clear();
	counts.clear();
	node_frames.clear();
	build_frames.clear();
	after_key = false;
}

void JSONStringWriter::Indent(idx_t depth) {
	buffer += '\n';
	buffer.append(depth * 4, ' ');
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
		buffer += ',';
	}
	if (pretty) {
		Indent(counts.size());
	}
}

void JSONStringWriter::Null() {
	BeforeValue();
	buffer += "null";
}

void JSONStringWriter::Boolean(bool value) {
	BeforeValue();
	buffer += value ? "true" : "false";
}

void JSONStringWriter::UnsignedInteger(uint64_t value) {
	BeforeValue();
	char digits[20];
	auto end = std::to_chars(digits, digits + sizeof(digits), value).ptr;
	buffer.append(digits, NumericCast<idx_t>(end - digits));
}

void JSONStringWriter::SignedInteger(int64_t value) {
	BeforeValue();
	char digits[21];
	auto end = std::to_chars(digits, digits + sizeof(digits), value).ptr;
	buffer.append(digits, NumericCast<idx_t>(end - digits));
}

void JSONStringWriter::Double(double value) {
	BeforeValue();
	if (!WriteDouble(buffer, value, flags)) {
		throw SerializationException("Failed to write JSON: NaN and Infinity are not allowed");
	}
}

void JSONStringWriter::Raw(std::string_view value) {
	BeforeValue();
	buffer.append(value);
}

void JSONStringWriter::String(std::string_view value) {
	BeforeValue();
	if (!WriteString(buffer, value, flags)) {
		throw SerializationException("Failed to write JSON: a string is not valid UTF-8");
	}
}

void JSONStringWriter::StartObject() {
	BeforeValue();
	buffer += '{';
	counts.push_back(0);
}

void JSONStringWriter::Key(std::string_view key) {
	if (counts.back()++ > 0) {
		buffer += ',';
	}
	if (pretty) {
		Indent(counts.size());
	}
	if (!WriteString(buffer, key, flags)) {
		throw SerializationException("Failed to write JSON: a key is not valid UTF-8");
	}
	buffer += pretty ? ": " : ":";
	after_key = true;
}

void JSONStringWriter::EndObject() {
	const auto count = counts.back();
	counts.pop_back();
	if (pretty && count > 0) {
		Indent(counts.size());
	}
	buffer += '}';
}

void JSONStringWriter::StartArray() {
	BeforeValue();
	buffer += '[';
	counts.push_back(0);
}

void JSONStringWriter::EndArray() {
	const auto count = counts.back();
	counts.pop_back();
	if (pretty && count > 0) {
		Indent(counts.size());
	}
	buffer += ']';
}

void JSONStringWriter::EndContainer(bool object) {
	if (object) {
		EndObject();
	} else {
		EndArray();
	}
}

void JSONStringWriter::Value(const JSONNode &root) {
	const auto base = node_frames.size();
	auto node = &root;
	while (true) {
		switch (node->type) {
		case JSONValueType::JSON_NULL:
			Null();
			break;
		case JSONValueType::BOOLEAN:
			Boolean(node->boolean);
			break;
		case JSONValueType::UNSIGNED_INTEGER:
			UnsignedInteger(node->unsigned_integer);
			break;
		case JSONValueType::SIGNED_INTEGER:
			SignedInteger(node->signed_integer);
			break;
		case JSONValueType::DOUBLE:
			Double(node->real);
			break;
		case JSONValueType::RAW:
			Raw(node->GetString());
			break;
		case JSONValueType::STRING:
			String(node->GetString());
			break;
		case JSONValueType::ARRAY:
		case JSONValueType::OBJECT: {
			const bool object = node->type == JSONValueType::OBJECT;
			if (object) {
				StartObject();
			} else {
				StartArray();
			}
			node_frames.push_back({object, node->size, node->First()});
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
				EndContainer(frame.object);
				node_frames.pop_back();
				continue;
			}
			frame.remaining--;
			node = frame.next;
			if (frame.object) {
				Key(node->GetString());
				node++;
			}
			frame.next = node->Next();
			break;
		}
	}
}

void JSONStringWriter::Value(const JSONBuildValue &root) {
	const auto base = build_frames.size();
	auto value = &root;
	while (true) {
		switch (value->type) {
		case JSONValueType::JSON_NULL:
			Null();
			break;
		case JSONValueType::BOOLEAN:
			Boolean(value->boolean);
			break;
		case JSONValueType::UNSIGNED_INTEGER:
			UnsignedInteger(value->unsigned_integer);
			break;
		case JSONValueType::SIGNED_INTEGER:
			SignedInteger(value->signed_integer);
			break;
		case JSONValueType::DOUBLE:
			Double(value->real);
			break;
		case JSONValueType::STRING:
			String(std::string_view(value->str, value->size));
			break;
		case JSONValueType::RAW:
			Raw(std::string_view(value->str, value->size));
			break;
		case JSONValueType::ARRAY:
		case JSONValueType::OBJECT: {
			const bool object = value->type == JSONValueType::OBJECT;
			if (object) {
				StartObject();
			} else {
				StartArray();
			}
			build_frames.push_back({object, value->size, value->last ? value->last->next : nullptr});
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
				EndContainer(frame.object);
				build_frames.pop_back();
				continue;
			}
			frame.remaining--;
			value = frame.next;
			frame.next = value->next;
			if (frame.object) {
				Key(std::string_view(value->key, value->key_size));
			}
			break;
		}
	}
}

bool JSONStringWriter::WriteDouble(string &out, double value, JSONWriteFlags flags) {
	if (std::isnan(value) || std::isinf(value)) {
		if (!HasFlag(flags, JSONWriteFlags::ALLOW_INF_AND_NAN)) {
			return false;
		}
		if (std::isnan(value)) {
			out += "NaN";
		} else {
			out += value < 0 ? "-Infinity" : "Infinity";
		}
		return true;
	}
	if (std::signbit(value)) {
		out += '-';
		value = -value;
	}
	if (value == 0) {
		out += "0.0";
		return true;
	}
	char text[32];
	auto end = std::to_chars(text, text + sizeof(text), value, std::chars_format::scientific).ptr;
	char digits[20];
	idx_t digit_count = 0;
	auto pos = text;
	for (; pos < end && *pos != 'e'; pos++) {
		if (*pos != '.') {
			digits[digit_count++] = *pos;
		}
	}
	int exponent = 0;
	std::from_chars(pos + 1 + (pos[1] == '+'), end, exponent);
	const int dot = exponent + 1;
	if (-6 < dot && dot <= 21) {
		if (dot > 0) {
			const auto whole = static_cast<idx_t>(dot);
			if (digit_count <= whole) {
				out.append(digits, digit_count);
				out.append(whole - digit_count, '0');
				out += ".0";
			} else {
				out.append(digits, whole);
				out += '.';
				out.append(digits + whole, digit_count - whole);
			}
		} else {
			out += "0.";
			out.append(static_cast<idx_t>(-dot), '0');
			out.append(digits, digit_count);
		}
		return true;
	}
	out += digits[0];
	if (digit_count > 1) {
		out += '.';
		out.append(digits + 1, digit_count - 1);
	}
	out += 'e';
	char exponent_text[8];
	auto exponent_end = std::to_chars(exponent_text, exponent_text + sizeof(exponent_text), dot - 1).ptr;
	out.append(exponent_text, NumericCast<idx_t>(exponent_end - exponent_text));
	return true;
}

bool JSONStringWriter::WriteString(string &out, std::string_view value, JSONWriteFlags flags) {
	if (!HasFlag(flags, JSONWriteFlags::ALLOW_INVALID_UNICODE) && !simdutf::validate_utf8(value.data(), value.size())) {
		return false;
	}
	static constexpr char HEX[] = "0123456789ABCDEF";
	out += '"';
	idx_t start = 0;
	for (idx_t i = 0; i < value.size(); i++) {
		const auto c = static_cast<uint8_t>(value[i]);
		if (c >= 0x20 && c != '"' && c != '\\') {
			continue;
		}
		out.append(value.data() + start, i - start);
		start = i + 1;
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += "\\u00";
			out += HEX[c >> 4];
			out += HEX[c & 0xF];
			break;
		}
	}
	out.append(value.data() + start, value.size() - start);
	out += '"';
	return true;
}

JSONBuilder::JSONBuilder(ArenaAllocator &arena_p) : arena(arena_p) {
}

JSONBuildValue *JSONBuilder::Make(JSONValueType type) {
	auto value = reinterpret_cast<JSONBuildValue *>(arena.AllocateAligned(sizeof(JSONBuildValue)));
	value->type = type;
	value->boolean = false;
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

void JSONBuilder::Add(JSONBuildValue &object, std::string_view key, JSONBuildValue *value) {
	D_ASSERT(object.type == JSONValueType::OBJECT);
	value->key = key.data();
	value->key_size = NumericCast<uint32_t>(key.size());
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
