//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/json_document.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/storage/arena_allocator.hpp"

#include <cstring>
#include <functional>
#include <string_view>

namespace duckdb {

enum class JSONValueType : uint8_t {
	INVALID,
	JSON_NULL,
	BOOLEAN,
	UNSIGNED_INTEGER,
	SIGNED_INTEGER,
	DOUBLE,
	STRING,
	ARRAY,
	OBJECT,
	RAW
};

enum class JSONReadFlags : uint32_t {
	NONE = 0,
	ALLOW_INVALID_UNICODE = 1 << 0,
	ALLOW_INF_AND_NAN = 1 << 1,
	ALLOW_TRAILING_COMMAS = 1 << 2,
	BIGNUM_AS_RAW = 1 << 3,
	NUMBER_AS_RAW = 1 << 4,
	ALLOW_COMMENTS = 1 << 5,
	//! The path functions check the whole document, not only what they read on the way to the values
	VALIDATE_PATHS = 1 << 6
};

constexpr JSONReadFlags operator|(JSONReadFlags a, JSONReadFlags b) {
	return static_cast<JSONReadFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool HasFlag(JSONReadFlags flags, JSONReadFlags flag) {
	return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

constexpr JSONReadFlags JSON_TYPE_READ_FLAGS =
    JSONReadFlags::ALLOW_INF_AND_NAN | JSONReadFlags::ALLOW_TRAILING_COMMAS | JSONReadFlags::BIGNUM_AS_RAW;

enum class JSONWriteFlags : uint32_t {
	NONE = 0,
	ALLOW_INVALID_UNICODE = 1 << 0,
	ALLOW_INF_AND_NAN = 1 << 1,
	PRETTY = 1 << 2
};

constexpr JSONWriteFlags operator|(JSONWriteFlags a, JSONWriteFlags b) {
	return static_cast<JSONWriteFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool HasFlag(JSONWriteFlags flags, JSONWriteFlags flag) {
	return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

struct JSONNode;

struct JSONNodeRange {
	struct Iterator {
		const JSONNode *node;
		uint32_t remaining;
		bool members;

		const JSONNode &operator*() const {
			return *node;
		}
		Iterator &operator++();
		bool operator!=(const Iterator &other) const {
			return remaining != other.remaining;
		}
	};

	Iterator begin() const {
		return {first, size, members};
	}
	Iterator end() const {
		return {nullptr, 0, members};
	}

	const JSONNode *first;
	uint32_t size;
	bool members;
};

struct JSONNode {
	JSONValueType type;
	bool boolean;
	uint32_t size;
	union {
		uint64_t unsigned_integer;
		int64_t signed_integer;
		double real;
		const char *str;
		uint64_t nodes;
	};

	bool IsContainer() const {
		return type == JSONValueType::ARRAY || type == JSONValueType::OBJECT;
	}
	bool IsInteger() const {
		return type == JSONValueType::UNSIGNED_INTEGER || type == JSONValueType::SIGNED_INTEGER;
	}
	bool IsNumber() const {
		return IsInteger() || type == JSONValueType::DOUBLE;
	}
	std::string_view GetString() const {
		return {str, size};
	}
	double GetNumber() const;
	const JSONNode *Next() const {
		return this + (IsContainer() ? nodes : 1);
	}
	const JSONNode *First() const {
		return this + 1;
	}
	const JSONNode *Get(std::string_view key) const;
	const JSONNode *Get(idx_t index) const;
	JSONNodeRange Elements() const {
		return {First(), size, false};
	}
	JSONNodeRange Keys() const {
		return {First(), size, true};
	}
};

static_assert(sizeof(JSONNode) == 16, "JSONNode is two words");

inline JSONNodeRange::Iterator &JSONNodeRange::Iterator::operator++() {
	node = members ? (node + 1)->Next() : node->Next();
	remaining--;
	return *this;
}

bool JSONEquals(const JSONNode &left, const JSONNode &right);
const char *JSONTypeDescription(const JSONNode &node);

struct JSONParseError {
	bool HasError() const {
		return has_error;
	}

	bool has_error = false;
	idx_t position = 0;
	string message;
};

//! One step of a path that JSONTreeParser::ParsePaths follows: an object key, or an array index when is_key is false
struct JSONPathStep {
	bool is_key;
	//! Whether key holds a backslash
	bool escaped;
	std::string_view key;
	idx_t index;
};

struct JSONTreeParserState;
class JSONStringWriter;

class JSONTreeParser {
public:
	explicit JSONTreeParser(JSONReadFlags flags = JSONReadFlags::NONE,
	                        Allocator &allocator = Allocator::DefaultAllocator());
	~JSONTreeParser();
	JSONTreeParser(const JSONTreeParser &) = delete;
	JSONTreeParser &operator=(const JSONTreeParser &) = delete;
	JSONTreeParser(JSONTreeParser &&other) noexcept;
	JSONTreeParser &operator=(JSONTreeParser &&other) noexcept;

	static constexpr idx_t PADDING = 64;

	const JSONNode *Parse(const char *data, idx_t len, JSONParseError &error);
	const JSONNode *Parse(const char *data, idx_t len);
	optional_idx ParseAppend(const char *data, idx_t len, JSONParseError &error);
	optional_idx ParseAppend(const char *data, idx_t len, idx_t readable, JSONParseError &error);
	bool Validate(const char *data, idx_t len, idx_t readable, JSONParseError &error);
	//! Parses a document as Parse does and sets values[i] to the value that paths[i] (at least one step) leads to, or
	//! to nullptr, as JSONNode::Get would; only those values are kept as nodes. Returns false, with the error set,
	//! when Parse fails.
	bool ParsePaths(const char *data, idx_t len, const vector<JSONPathStep> *paths, idx_t count,
	                const JSONNode **values, JSONParseError &error);
	//! Clears the writer and writes the document with it, as Parse and JSONStringWriter::Value would, without
	//! building nodes. Returns false, with the error set, when Parse fails.
	bool Write(const char *data, idx_t len, JSONStringWriter &writer, JSONParseError &error);
	//! Clears the writer and writes the value that the path leads to (the root when it has no step), as ParsePaths and
	//! JSONStringWriter::Value would; found is false when there is none. Returns false, with the error set, when Parse
	//! fails.
	bool WritePath(const char *data, idx_t len, const vector<JSONPathStep> &path, JSONStringWriter &writer, bool &found,
	               JSONParseError &error);
	//! Whether Parse would succeed, without keeping nodes; sets the error as Parse when not
	bool IsValid(const char *data, idx_t len, JSONParseError &error);
	const JSONNode *ParsePrefix(const char *data, idx_t len, idx_t &consumed, JSONParseError &error);
	void Clear();
	const JSONNode *Node(idx_t index) const;
	idx_t NodeCount() const;
	JSONReadFlags Flags() const {
		return flags;
	}
	void SetFlags(JSONReadFlags flags);

private:
	JSONReadFlags flags;
	unique_ptr<JSONTreeParserState> state;
};

struct JSONBuildValue {
	JSONValueType type;
	bool boolean;
	//! Whether the key is ASCII without characters to escape, written as it is
	bool key_verbatim;
	uint32_t size;
	uint32_t key_size;
	union {
		uint64_t unsigned_integer;
		int64_t signed_integer;
		double real;
		const char *str;
		JSONBuildValue *last;
	};
	JSONBuildValue *next;
	const char *key;
};

class JSONStringWriter {
public:
	explicit JSONStringWriter(JSONWriteFlags flags = JSONWriteFlags::NONE);

	void Clear();
	void Null();
	void Boolean(bool value);
	void UnsignedInteger(uint64_t value);
	void SignedInteger(int64_t value);
	void Double(double value);
	void Raw(std::string_view value);
	void String(std::string_view value);
	void StartObject();
	void Key(std::string_view key, bool verbatim = false);
	//! A string that needs no escaping and is valid UTF-8, written as it is
	void VerbatimString(std::string_view value);
	void EndObject();
	void StartArray();
	void EndArray();
	void Value(const JSONNode &node);
	void Value(const JSONBuildValue &value);

	std::string_view View() const {
		return std::string_view(buffer.data(), used);
	}
	string ToString() const {
		return string(buffer.data(), used);
	}
	static bool WriteDouble(string &out, double value, JSONWriteFlags flags);
	static bool WriteString(string &out, std::string_view value, JSONWriteFlags flags);
	//! Whether a string is ASCII without characters to escape, so that its JSON text is itself between quotes
	static bool IsVerbatim(std::string_view value);
	//! The room that WriteDouble(char *) needs: simdjson's to_chars writes in blocks
	static constexpr idx_t MAX_DOUBLE_LENGTH = 48;
	//! Writes the text of a double at out, returning its end; nullptr for NaN and Infinity unless the flags allow them
	static char *WriteDouble(char *out, double value, JSONWriteFlags flags);
	//! The most bytes that WriteString(char *) writes for a string of this length
	static constexpr idx_t MaxStringLength(idx_t length) {
		return 6 * length + 2 + 64;
	}
	//! Writes a string, quoted and escaped, at out, returning its end; nullptr when it is not UTF-8 unless the flags
	//! allow it
	static char *WriteString(char *out, std::string_view value, JSONWriteFlags flags);

private:
	struct NodeFrame {
		bool object;
		bool first;
		uint32_t remaining;
		const JSONNode *next;
	};
	struct BuildFrame {
		bool object;
		bool first;
		uint32_t remaining;
		const JSONBuildValue *next;
	};

	void BeforeValue();
	void Indent(idx_t depth);
	//! The text of a value or a key, without the separator before it
	void PutUnsignedInteger(uint64_t value);
	void PutSignedInteger(int64_t value);
	void PutDouble(double value);
	void PutString(std::string_view value, bool verbatim);
	void PutKey(std::string_view key, bool verbatim);
	//! Before an element or a member of a container that a tree writer opened, at this depth below it
	void PutSeparator(bool first, idx_t depth);
	void PutClose(bool object, bool empty, idx_t depth);
	//! Makes room for n more bytes, returning where they go
	char *Reserve(idx_t n) {
		if (used + n > buffer.size()) {
			Grow(n);
		}
		return &buffer[0] + used;
	}
	void Grow(idx_t n);
	void Put(char c) {
		*Reserve(1) = c;
		used++;
	}
	void Put(const char *text, idx_t n) {
		memcpy(Reserve(n), text, n);
		used += n;
	}
	//! Copies n bytes: short copies are fixed-size loads and stores that overlap, as a memcpy of a variable length
	//! is a call
	static void CopyShort(char *out, const char *text, idx_t n) {
		if (n >= 8 && n <= 16) {
			uint64_t first;
			uint64_t last;
			memcpy(&first, text, 8);
			memcpy(&last, text + n - 8, 8);
			memcpy(out, &first, 8);
			memcpy(out + n - 8, &last, 8);
		} else if (n >= 4 && n < 8) {
			uint32_t first;
			uint32_t last;
			memcpy(&first, text, 4);
			memcpy(&last, text + n - 4, 4);
			memcpy(out, &first, 4);
			memcpy(out + n - 4, &last, 4);
		} else if (n > 0 && n < 4) {
			out[0] = text[0];
			out[n >> 1] = text[n >> 1];
			out[n - 1] = text[n - 1];
		} else if (n > 16) {
			memcpy(out, text, n);
		}
	}
	//! Writes a string that needs no escaping between quotes
	static char *WriteQuoted(char *out, std::string_view text) {
		out[0] = '"';
		CopyShort(out + 1, text.data(), text.size());
		out[text.size() + 1] = '"';
		return out + text.size() + 2;
	}
	void PutQuoted(std::string_view text) {
		auto out = Reserve(text.size() + 2);
		used += static_cast<idx_t>(WriteQuoted(out, text) - out);
	}

private:
	JSONWriteFlags flags;
	bool pretty;
	//! The text is the first used bytes; the rest is room to write
	string buffer;
	idx_t used = 0;
	vector<uint32_t> counts;
	vector<NodeFrame> node_frames;
	vector<BuildFrame> build_frames;
	bool after_key = false;
};

class JSONBuilder {
public:
	explicit JSONBuilder(ArenaAllocator &arena);

	JSONBuildValue *Null();
	JSONBuildValue *Boolean(bool value);
	JSONBuildValue *UnsignedInteger(uint64_t value);
	JSONBuildValue *SignedInteger(int64_t value);
	JSONBuildValue *Double(double value);
	JSONBuildValue *String(std::string_view value);
	JSONBuildValue *Raw(std::string_view value);
	JSONBuildValue *Object();
	JSONBuildValue *Array();
	JSONBuildValue *Copy(const JSONNode &node);
	static void Add(JSONBuildValue &object, std::string_view key, JSONBuildValue *value, bool verbatim = false);
	static void Append(JSONBuildValue &array, JSONBuildValue *value);

private:
	struct CopyFrame {
		JSONBuildValue *container;
		uint32_t remaining;
		const JSONNode *next;
	};

	JSONBuildValue *Make(JSONValueType type);
	const char *CopyString(std::string_view value);

private:
	ArenaAllocator &arena;
	vector<CopyFrame> frames;
};

class JSONValue {
public:
	JSONValue();
	explicit JSONValue(const JSONNode *node);

	bool IsValid() const;
	JSONValueType GetType() const;
	bool IsNull() const;
	bool IsString() const;
	bool IsArray() const;
	bool IsObject() const;
	bool IsInteger() const;
	bool IsNumber() const;

	string GetString() const;
	bool GetBoolean() const;
	uint64_t GetUnsignedInteger() const;
	int64_t GetSignedInteger() const;
	double GetDouble() const;
	double GetNumber() const;

	JSONValue GetMember(std::string_view key) const;
	void IterateArray(const std::function<void(JSONValue)> &callback) const;
	void IterateObject(const std::function<void(const string &, JSONValue)> &callback) const;

	string ToString(JSONWriteFlags flags = JSONWriteFlags::NONE) const;

	const JSONNode *GetNode() const {
		return node;
	}

private:
	const JSONNode *node;
};

class JSONDocument {
public:
	JSONDocument();
	~JSONDocument();
	JSONDocument(const JSONDocument &) = delete;
	JSONDocument &operator=(const JSONDocument &) = delete;
	JSONDocument(JSONDocument &&other) noexcept;
	JSONDocument &operator=(JSONDocument &&other) noexcept;

	static unique_ptr<JSONDocument> Parse(const char *data, idx_t len, JSONReadFlags flags = JSONReadFlags::NONE);
	static unique_ptr<JSONDocument> TryParse(const char *data, idx_t len, JSONParseError &error,
	                                         JSONReadFlags flags = JSONReadFlags::NONE);

	JSONValue GetRoot() const;
	string ToString(JSONWriteFlags flags = JSONWriteFlags::NONE) const;

private:
	unique_ptr<JSONTreeParser> parser;
	const JSONNode *root = nullptr;
};

struct JSONMutableNode {
	JSONValueType type = JSONValueType::JSON_NULL;
	bool boolean = false;
	uint64_t unsigned_integer = 0;
	int64_t signed_integer = 0;
	double real = 0;
	string text;
	vector<std::pair<string, idx_t>> children;
};

struct JSONMutableDocument {
	vector<JSONMutableNode> nodes;
	optional_idx root;

	idx_t Add(JSONMutableNode node);
	void Write(JSONStringWriter &out, idx_t index) const;
};

class JSONMutableValue {
	friend class JSONWriter;

public:
	JSONMutableValue();

	bool IsValid() const;
	void Add(std::string_view key, JSONMutableValue value);
	void AddString(std::string_view key, std::string_view value);
	void Append(JSONMutableValue value);
	void AppendString(std::string_view value);

private:
	JSONMutableValue(JSONMutableDocument *doc, idx_t index);

private:
	JSONMutableDocument *doc;
	idx_t index;
};

class JSONWriter {
public:
	JSONWriter();
	~JSONWriter();
	JSONWriter(const JSONWriter &) = delete;
	JSONWriter &operator=(const JSONWriter &) = delete;
	JSONWriter(JSONWriter &&other) noexcept;
	JSONWriter &operator=(JSONWriter &&other) noexcept;

	JSONMutableValue CreateObject();
	JSONMutableValue CreateArray();
	JSONMutableValue CreateString(std::string_view value);
	JSONMutableValue CreateNull();
	JSONMutableValue CreateBoolean(bool value);
	JSONMutableValue CreateUnsignedInteger(uint64_t value);
	JSONMutableValue CreateSignedInteger(int64_t value);
	JSONMutableValue CreateDouble(double value);
	JSONMutableValue CreateCopy(const JSONValue &value);

	void SetRoot(JSONMutableValue value);
	string ToString(JSONWriteFlags flags = JSONWriteFlags::NONE) const;

private:
	JSONMutableValue Create(JSONMutableNode node);

private:
	unique_ptr<JSONMutableDocument> doc;
};

} // namespace duckdb
