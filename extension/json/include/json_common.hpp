//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_common.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/json_document.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//! JSONKey / json_key_map_t speeds up mapping from JSON key to column ID
struct JSONKey {
	const char *ptr;
	size_t len;
};

struct JSONKeyHash {
	inline std::size_t operator()(const JSONKey &k) const {
		size_t result;
		if (k.len >= sizeof(size_t)) {
			memcpy(&result, k.ptr + k.len - sizeof(size_t), sizeof(size_t));
		} else {
			result = 0;
			memcpy(&result, k.ptr, k.len);
		}
		return result;
	}
};

struct JSONKeyEquality {
	inline bool operator()(const JSONKey &a, const JSONKey &b) const {
		if (a.len != b.len) {
			return false;
		}
		return memcmp(a.ptr, b.ptr, a.len) == 0;
	}
};

template <typename T>
using json_key_map_t = unordered_map<JSONKey, T, JSONKeyHash, JSONKeyEquality>;
using json_key_set_t = unordered_set<JSONKey, JSONKeyHash, JSONKeyEquality>;

struct JSONKeyCIHash {
	inline std::size_t operator()(const JSONKey &k) const {
		return StringUtil::CIHash(k.ptr, k.len);
	}
};

struct JSONKeyCIEquality {
	inline bool operator()(const JSONKey &a, const JSONKey &b) const {
		return StringUtil::CIEquals(a.ptr, a.len, b.ptr, b.len);
	}
};

template <typename T>
using json_key_ci_map_t = unordered_map<JSONKey, T, JSONKeyCIHash, JSONKeyCIEquality>;

//! The type of a single element of a JSON path
enum class JSONPathElementType : uint8_t {
	//! Object key ($.key)
	KEY,
	//! Array index ($[0])
	INDEX,
	//! Array index counting from the back ($[#-1] or $[-1])
	REVERSE_INDEX,
	//! The position after the last array element ($[#])
	APPEND,
	//! Wildcard ($.* or $[*])
	WILDCARD,
	//! Recursive wildcard ($.**)
	RECURSIVE_WILDCARD,
};

//! A single element of a JSON path
struct JSONPathElement {
	JSONPathElementType type = JSONPathElementType::KEY;
	//! Key for KEY elements
	string key;
	//! Index for INDEX and REVERSE_INDEX elements
	idx_t index = 0;
};

//! Iterates over the elements of a JSON path ($.field[index]... syntax)
class JSONPathIterator {
public:
	//! The path must start with '$' (validate before constructing)
	JSONPathIterator(const char *ptr, idx_t len, bool binder);
	//! Get the next path element, returns false when the path is exhausted. Throws on a malformed path.
	bool Next(JSONPathElement &element);
	//! The rest of the path
	const char *Position() const {
		return ptr;
	}

private:
	const char *ptr;
	const char *const end;
	const bool binder;
};

//! Common JSON functionality for most JSON functions
struct JSONCommon {
public:
	//! Maximum recursion depth for recursive functions
	static constexpr idx_t MAX_RECURSION_DEPTH = 128;
	//! Read/Write flags
	static constexpr auto READ_FLAG = JSON_TYPE_READ_FLAGS;
	static constexpr auto STRICT_READ_FLAG = JSONReadFlags::BIGNUM_AS_RAW;
	static constexpr auto WRITE_FLAG = JSONWriteFlags::ALLOW_INF_AND_NAN;
	static constexpr auto WRITE_PRETTY_FLAG = JSONWriteFlags::ALLOW_INF_AND_NAN | JSONWriteFlags::PRETTY;

public:
	//! Constant JSON type strings
	static constexpr char const *TYPE_STRING_NULL = "NULL";
	static constexpr char const *TYPE_STRING_BOOLEAN = "BOOLEAN";
	static constexpr char const *TYPE_STRING_BIGINT = "BIGINT";
	static constexpr char const *TYPE_STRING_UBIGINT = "UBIGINT";
	static constexpr char const *TYPE_STRING_DOUBLE = "DOUBLE";
	static constexpr char const *TYPE_STRING_HUGEINT = "HUGEINT";
	static constexpr char const *TYPE_STRING_VARCHAR = "VARCHAR";
	static constexpr char const *TYPE_STRING_ARRAY = "ARRAY";
	static constexpr char const *TYPE_STRING_OBJECT = "OBJECT";

	static inline const char *ValTypeToString(const JSONNode &val) {
		switch (val.type) {
		case JSONValueType::JSON_NULL:
			return TYPE_STRING_NULL;
		case JSONValueType::STRING:
			return TYPE_STRING_VARCHAR;
		case JSONValueType::ARRAY:
			return TYPE_STRING_ARRAY;
		case JSONValueType::OBJECT:
			return TYPE_STRING_OBJECT;
		case JSONValueType::BOOLEAN:
			return TYPE_STRING_BOOLEAN;
		case JSONValueType::UNSIGNED_INTEGER:
			return TYPE_STRING_UBIGINT;
		case JSONValueType::SIGNED_INTEGER:
			return TYPE_STRING_BIGINT;
		case JSONValueType::DOUBLE:
		case JSONValueType::RAW:
			return TYPE_STRING_DOUBLE;
		default:
			throw InternalException("Unexpected JSON value type in ValTypeToString");
		}
	}

	static inline string_t ValTypeToStringT(const JSONNode &val) {
		return string_t(ValTypeToString(val));
	}

	static inline LogicalTypeId ValTypeToLogicalTypeId(const JSONNode &val) {
		switch (val.type) {
		case JSONValueType::JSON_NULL:
			return LogicalTypeId::SQLNULL;
		case JSONValueType::STRING:
			return LogicalTypeId::VARCHAR;
		case JSONValueType::ARRAY:
			return LogicalTypeId::LIST;
		case JSONValueType::OBJECT:
			return LogicalTypeId::STRUCT;
		case JSONValueType::BOOLEAN:
			return LogicalTypeId::BOOLEAN;
		case JSONValueType::UNSIGNED_INTEGER:
			return LogicalTypeId::UBIGINT;
		case JSONValueType::SIGNED_INTEGER:
			return LogicalTypeId::BIGINT;
		case JSONValueType::DOUBLE:
		case JSONValueType::RAW:
			return LogicalTypeId::DOUBLE;
		default:
			throw InternalException("Unexpected JSON value type in ValTypeToLogicalTypeId");
		}
	}

public:
	//===--------------------------------------------------------------------===//
	// Document reading / writing
	//===--------------------------------------------------------------------===//
	//! Parse a document with the given parser, or nullptr with the error set
	static inline const JSONNode *ReadDocumentUnsafe(JSONTreeParser &parser, const string_t &input,
	                                                 JSONParseError &error) {
		return parser.Parse(input.GetData(), input.GetSize(), error);
	}
	//! Parse a document with the given parser - throws on malformed JSON
	static inline const JSONNode &ReadDocument(JSONTreeParser &parser, const char *data, idx_t size) {
		JSONParseError error;
		auto root = parser.Parse(data, size, error);
		if (!root) {
			ThrowParseError(data, size, error);
		}
		return *root;
	}
	static inline const JSONNode &ReadDocument(JSONTreeParser &parser, const string_t &input) {
		return ReadDocument(parser, input.GetData(), input.GetSize());
	}
	static string FormatParseError(const char *data, idx_t length, const JSONParseError &error,
	                               const string &extra = "") {
		D_ASSERT(error.HasError());
		// Truncate, so we don't print megabytes worth of JSON
		auto input = length > 50 ? string(data, 47) + "..." : string(data, length);
		// Have to replace \r, otherwise output is unreadable
		input = StringUtil::Replace(input, "\r", "\\r");
		return StringUtil::Format("Malformed JSON at byte %lld of input: %s. %s Input: \"%s\"", error.position,
		                          error.message, extra, input);
	}
	static void ThrowParseError(const char *data, idx_t length, const JSONParseError &error, const string &extra = "") {
		throw InvalidInputException(FormatParseError(data, length, error, extra));
	}
	//! Parse a document and write it with the given writer, adding the text to the string heap of the vector -
	//! throws on malformed JSON
	static inline string_t WriteDocument(JSONTreeParser &parser, const string_t &input, JSONStringWriter &writer,
	                                     Vector &vector) {
		JSONParseError error;
		if (!parser.Write(input.GetData(), input.GetSize(), writer, error)) {
			ThrowParseError(input.GetData(), input.GetSize(), error);
		}
		return StringVector::AddString(vector, writer.View().data(), writer.View().size());
	}
	//! Write a value with the given writer and add the text to the string heap of the vector
	static inline string_t WriteVal(const JSONNode &val, JSONStringWriter &writer, Vector &vector) {
		writer.Clear();
		writer.Value(val);
		return StringVector::AddString(vector, writer.View().data(), writer.View().size());
	}
	//! Slow and easy ToString for errors
	static string ValToString(const JSONNode &val, idx_t max_len = DConstants::INVALID_INDEX);
	//! Throw an error with the printed value
	static void ThrowValFormatError(string error_string, const JSONNode &val);

public:
	//===--------------------------------------------------------------------===//
	// JSON pointer / path
	//===--------------------------------------------------------------------===//
	enum class JSONPathType : uint8_t {
		//! Extract a single value
		REGULAR = 0,
		//! Extract multiple values (when we have a '*' wildcard in the JSON Path)
		WILDCARD = 1,
	};

	//! Get JSON value using JSON path query (safe, checks the path query)
	static inline const JSONNode *Get(const JSONNode *val, const string_t &path_str, bool integral_argument) {
		auto ptr = path_str.GetData();
		auto len = path_str.GetSize();
		if (len == 0) {
			return GetUnsafe(val, ptr, len);
		}
		if (integral_argument) {
			auto str = "$[" + path_str.GetString() + "]";
			return GetUnsafe(val, str.c_str(), str.length());
		}
		switch (*ptr) {
		case '/':
			return GetUnsafe(val, ptr, len);
		case '$': {
			if (ValidatePath(ptr, len, false) == JSONPathType::WILDCARD) {
				throw InvalidInputException(
				    "JSON path cannot contain wildcards if the path is not a constant parameter");
			}
			return GetUnsafe(val, ptr, len);
		}
		default: {
			string path;
			if (memchr(ptr, '"', len)) {
				path = "/" + string(ptr, len);
			} else {
				path = "$.\"" + path_str.GetString() + "\"";
			}
			return GetUnsafe(val, path.c_str(), path.length());
		}
		}
	}

	//! Get JSON value using JSON path query (unsafe)
	static inline const JSONNode *GetUnsafe(const JSONNode *val, const char *ptr, const idx_t &len) {
		if (len == 0) {
			return val;
		}
		switch (*ptr) {
		case '/':
			return GetPointer(val, ptr, len);
		case '$':
			return GetPath(val, ptr, len);
		default:
			throw InternalException("JSON pointer/path does not start with '/' or '$'");
		}
	}

	//! Get JSON value using JSON path query (unsafe)
	static void GetWildcardPath(const JSONNode *val, const char *ptr, const idx_t &len, vector<const JSONNode *> &vals);

	//! Validate JSON Path ($.field[index]... syntax), returns true if there are wildcards in the path
	static JSONPathType ValidatePath(const char *ptr, const idx_t &len, const bool binder);

	//! Convert path elements into steps for JSONTreeParser::ParsePaths; false if they have a reverse index
	static bool PathSteps(const vector<JSONPathElement> &elements, vector<JSONPathStep> &steps);
	//! Parse a document and return the value at the path - throws on malformed JSON
	static inline const JSONNode *ReadPath(JSONTreeParser &parser, const string_t &input,
	                                       const vector<JSONPathStep> &steps) {
		JSONParseError error;
		const JSONNode *value;
		if (!parser.ParsePaths(input.GetData(), input.GetSize(), &steps, 1, &value, error)) {
			ThrowParseError(input.GetData(), input.GetSize(), error);
		}
		return value;
	}
	//! Parse a JSON path ($.field[index]... syntax, no wildcards) into path elements
	static vector<JSONPathElement> ParsePathElements(const char *ptr, idx_t len, bool binder);

	//! Get JSON value by walking pre-parsed path elements
	static const JSONNode *GetPathElements(const JSONNode *val, const vector<JSONPathElement> &elements);

	//! Get JSON pointer (/field/index/... syntax, RFC 6901)
	static const JSONNode *GetPointer(const JSONNode *val, const char *ptr, idx_t len);

private:
	//! Get JSON path ($.field[index]... syntax)
	static const JSONNode *GetPath(const JSONNode *val, const char *ptr, const idx_t &len);
};

} // namespace duckdb
