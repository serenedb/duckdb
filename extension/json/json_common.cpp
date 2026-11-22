#include "json_common.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

using JSONPathType = JSONCommon::JSONPathType;

string JSONCommon::ValToString(const JSONNode &val, idx_t max_len) {
	JSONStringWriter writer(WRITE_FLAG);
	writer.Value(val);
	auto text = writer.View();
	if (max_len < text.size()) {
		return string(text.substr(0, max_len)) + "...";
	}
	return string(text);
}

void JSONCommon::ThrowValFormatError(string error_string, const JSONNode &val) {
	error_string = StringUtil::Format(error_string, JSONCommon::ValToString(val));
	throw InvalidInputException(error_string);
}

string ThrowPathError(const char *ptr, const char *end, const bool binder) {
	ptr--;
	auto msg = StringUtil::Format("JSON path error near '%s'", string(ptr, end - ptr));
	if (binder) {
		throw BinderException(msg);
	} else {
		throw InvalidInputException(msg);
	}
}

struct JSONKeyReadResult {
public:
	static inline JSONKeyReadResult Empty() {
		return {idx_t(0), false, string()};
	}

	static inline JSONKeyReadResult WildCard() {
		return {1, false, "*"};
	}

	static inline JSONKeyReadResult RecWildCard() {
		return {2, true, "*"};
	}

	static inline JSONKeyReadResult RecWildCardShortcut() {
		return {1, true, "*"};
	}

	inline bool IsValid() {
		return (chars_read != 0);
	}

	inline bool IsWildCard() {
		return key == "*";
	}

public:
	idx_t chars_read;
	bool recursive;
	string key;
};

static inline JSONKeyReadResult ReadString(const char *ptr, const char *const end, const bool escaped) {
	const char *const before = ptr;
	if (escaped) {
		auto key = make_unsafe_uniq_array_uninitialized<char>(end - ptr);
		idx_t key_len = 0;

		bool backslash = false;
		while (ptr != end) {
			if (backslash) {
				if (*ptr != '"' && *ptr != '\\') {
					key[key_len++] = '\\';
				}
				backslash = false;
			} else {
				if (*ptr == '"') {
					break;
				} else if (*ptr == '\\') {
					backslash = true;
					ptr++;
					continue;
				}
			}
			key[key_len++] = *ptr++;
		}
		if (ptr == end || backslash) {
			return JSONKeyReadResult::Empty();
		} else {
			return {idx_t(ptr - before), false, string(key.get(), key_len)};
		}
	} else {
		while (ptr != end) {
			if (*ptr == '.' || *ptr == '[') {
				break;
			}
			ptr++;
		}
		return {idx_t(ptr - before), false, string(before, ptr - before)};
	}
}

static inline idx_t ReadInteger(const char *ptr, const char *const end, idx_t &idx) {
	static constexpr auto IDX_T_SAFE_DIG = 19;
	static constexpr auto IDX_T_MAX = ((idx_t)(~(idx_t)0));

	const char *const before = ptr;
	idx = 0;
	for (idx_t i = 0; i < IDX_T_SAFE_DIG; i++) {
		if (ptr == end) {
			// No closing ']'
			return 0;
		}
		if (*ptr == ']') {
			break;
		}
		uint8_t add = (uint8_t)(*ptr - '0');
		if (add <= 9) {
			idx = add + idx * 10;
		} else {
			// Not a digit
			return 0;
		}
		ptr++;
	}
	// Invalid if overflow
	return idx >= (idx_t)IDX_T_MAX ? 0 : ptr - before;
}

static inline JSONKeyReadResult ReadKey(const char *ptr, const char *const end) {
	D_ASSERT(ptr != end);
	if (*ptr == '*') { // Wildcard
		if (ptr + 1 != end && *(ptr + 1) == '*') {
			return JSONKeyReadResult::RecWildCard();
		}
		return JSONKeyReadResult::WildCard();
	}
	bool recursive = false;
	if (*ptr == '.') {
		const char next = ptr + 1 == end ? '\0' : *(ptr + 1);
		if (next == '*') {
			return JSONKeyReadResult::RecWildCard();
		}
		if (next == '[') {
			return JSONKeyReadResult::RecWildCardShortcut();
		}
		ptr++;
		recursive = true;
	}
	if (ptr == end) {
		// recursive '.' with no key following it
		return JSONKeyReadResult::Empty();
	}
	bool escaped = false;
	if (*ptr == '"') {
		ptr++; // Skip past opening '"'
		escaped = true;
	}
	auto result = ReadString(ptr, end, escaped);
	if (!result.IsValid()) {
		return result;
	}
	if (escaped) {
		result.chars_read += 2; // Account for surrounding quotes
	}
	if (recursive) {
		result.chars_read += 1;
		result.recursive = true;
	}
	return result;
}

static inline bool ReadArrayIndex(const char *&ptr, const char *const end, JSONPathElement &element) {
	D_ASSERT(ptr != end);
	if (*ptr == '*') { // Wildcard
		ptr++;
		if (ptr == end || *ptr != ']') {
			return false;
		}
		element.type = JSONPathElementType::WILDCARD;
	} else {
		element.type = JSONPathElementType::INDEX;
		if (*ptr == '#') { // SQLite syntax to index from back of array
			ptr++;         // Skip over '#'
			if (ptr == end) {
				return false;
			}
			if (*ptr == ']') {
				element.type = JSONPathElementType::APPEND;
				ptr++;
				return true;
			}
			if (*ptr != '-') {
				return false;
			}
		}
		if (*ptr == '-') {
			ptr++; // Skip over '-'
			element.type = JSONPathElementType::REVERSE_INDEX;
		}
		auto idx_len = ReadInteger(ptr, end, element.index);
		if (idx_len == 0) {
			return false;
		}
		ptr += idx_len;
	}
	ptr++; // Skip past closing ']'
	return true;
}

JSONPathIterator::JSONPathIterator(const char *ptr_p, idx_t len, bool binder_p)
    : ptr(ptr_p), end(ptr_p + len), binder(binder_p) {
	D_ASSERT(len >= 1 && *ptr == '$');
	ptr++; // Skip past '$'
}

bool JSONPathIterator::Next(JSONPathElement &element) {
	if (ptr == end) {
		return false;
	}
	const auto &c = *ptr++;
	if (ptr == end) {
		ThrowPathError(ptr, end, binder);
	}
	switch (c) {
	case '.': { // Object field
		auto key = ReadKey(ptr, end);
		if (!key.IsValid()) {
			ThrowPathError(ptr, end, binder);
		}
		ptr += key.chars_read;
		if (key.recursive) {
			element.type = JSONPathElementType::RECURSIVE_WILDCARD;
		} else if (key.IsWildCard()) {
			element.type = JSONPathElementType::WILDCARD;
		} else {
			element.type = JSONPathElementType::KEY;
			element.key = std::move(key.key);
		}
		break;
	}
	case '[': { // Array index
		if (!ReadArrayIndex(ptr, end, element)) {
			ThrowPathError(ptr, end, binder);
		}
		break;
	}
	default:
		ThrowPathError(ptr, end, binder);
	}
	return true;
}

JSONPathType JSONCommon::ValidatePath(const char *ptr, const idx_t &len, const bool binder) {
	D_ASSERT(len >= 1 && *ptr == '$');
	JSONPathType path_type = JSONPathType::REGULAR;
	JSONPathIterator iterator(ptr, len, binder);
	JSONPathElement element;
	while (iterator.Next(element)) {
		if (element.type == JSONPathElementType::WILDCARD || element.type == JSONPathElementType::RECURSIVE_WILDCARD) {
			path_type = JSONPathType::WILDCARD;
		}
	}
	return path_type;
}

//! Resolve a single path element against a JSON value
static inline const JSONNode *GetPathElement(const JSONNode *val, const JSONPathElement &element) {
	switch (element.type) {
	case JSONPathElementType::KEY:
		return val->Get(std::string_view(element.key));
	case JSONPathElementType::INDEX:
	case JSONPathElementType::REVERSE_INDEX: {
		if (val->type != JSONValueType::ARRAY) {
			return nullptr;
		}
		auto array_index = element.index;
		if (element.type == JSONPathElementType::REVERSE_INDEX && array_index != 0) {
			array_index = val->size - array_index;
		}
		return val->Get(array_index);
	}
	case JSONPathElementType::APPEND:
		// [#] always returns NULL in SQLite
		return nullptr;
	default: // LCOV_EXCL_START
		throw InternalException("Invalid JSON path element encountered, call JSONCommon::ValidatePath first!");
	} // LCOV_EXCL_STOP
}

const JSONNode *JSONCommon::GetPath(const JSONNode *val, const char *ptr, const idx_t &len) {
	// Path has been validated at this point
	JSONPathIterator iterator(ptr, len, false);
	JSONPathElement element;
	while (val != nullptr && iterator.Next(element)) {
		val = GetPathElement(val, element);
	}
	return val;
}

vector<JSONPathElement> JSONCommon::ParsePathElements(const char *ptr, idx_t len, bool binder) {
	vector<JSONPathElement> elements;
	JSONPathIterator iterator(ptr, len, binder);
	JSONPathElement element;
	while (iterator.Next(element)) {
		elements.push_back(element);
	}
	return elements;
}

bool JSONCommon::PathSteps(const vector<JSONPathElement> &elements, vector<JSONPathStep> &steps) {
	steps.clear();
	if (elements.empty()) {
		return false;
	}
	for (auto &element : elements) {
		switch (element.type) {
		case JSONPathElementType::KEY:
			steps.push_back({true, element.key.find('\\') != string::npos, std::string_view(element.key), 0});
			break;
		case JSONPathElementType::INDEX:
			steps.push_back({false, false, std::string_view(), element.index});
			break;
		case JSONPathElementType::APPEND:
			// [#] leads nowhere: no array has this index
			steps.push_back({false, false, std::string_view(), NumericLimits<idx_t>::Maximum()});
			break;
		default:
			return false;
		}
	}
	return true;
}

const JSONNode *JSONCommon::GetPathElements(const JSONNode *val, const vector<JSONPathElement> &elements) {
	for (idx_t i = 0; val != nullptr && i < elements.size(); i++) {
		val = GetPathElement(val, elements[i]);
	}
	return val;
}

static void GetWildcardPathInternal(const JSONNode *val, const char *ptr, const char *const end,
                                    vector<const JSONNode *> &vals) {
	while (val != nullptr && ptr != end) {
		const auto &c = *ptr++;
		D_ASSERT(ptr != end);
		switch (c) {
		case '.': { // Object field
			auto key_result = ReadKey(ptr, end);
			D_ASSERT(key_result.IsValid());
			if (key_result.recursive) {
				if (key_result.IsWildCard()) {
					ptr += key_result.chars_read;
				}
				vector<const JSONNode *> rec_vals;
				rec_vals.emplace_back(val);
				for (idx_t i = 0; i < rec_vals.size(); i++) {
					const JSONNode *rec_val = rec_vals[i];
					if (rec_val->type == JSONValueType::ARRAY) {
						for (auto &element : rec_val->Elements()) {
							rec_vals.emplace_back(&element);
						}
					} else if (rec_val->type == JSONValueType::OBJECT) {
						for (auto &key : rec_val->Keys()) {
							rec_vals.emplace_back(&key + 1);
						}
					}
					if (i > 0 || ptr != end) {
						GetWildcardPathInternal(rec_val, ptr, end, vals);
					}
				}
				return;
			}
			ptr += key_result.chars_read;
			if (val->type != JSONValueType::OBJECT) {
				return;
			}
			if (key_result.IsWildCard()) { // Wildcard
				for (auto &key : val->Keys()) {
					GetWildcardPathInternal(&key + 1, ptr, end, vals);
				}
				return;
			}
			val = val->Get(std::string_view(key_result.key));
			break;
		}
		case '[': { // Array index
			if (val->type != JSONValueType::ARRAY) {
				return;
			}
			JSONPathElement element;
#ifdef DEBUG
			bool success =
#endif
			    ReadArrayIndex(ptr, end, element);
#ifdef DEBUG
			D_ASSERT(success);
#endif

			if (element.type == JSONPathElementType::WILDCARD) {
				for (auto &arr_val : val->Elements()) {
					GetWildcardPathInternal(&arr_val, ptr, end, vals);
				}
				return;
			}
			if (element.type == JSONPathElementType::APPEND) {
				// [#] always returns NULL in SQLite
				return;
			}
			auto array_index = element.index;
			if (element.type == JSONPathElementType::REVERSE_INDEX && array_index != 0) {
				array_index = val->size - array_index;
			}
			val = val->Get(array_index);
			break;
		}
		default: // LCOV_EXCL_START
			throw InternalException(
			    "Invalid JSON Path encountered in GetWildcardPathInternal, call JSONCommon::ValidatePath first!");
		} // LCOV_EXCL_STOP
	}
	if (val != nullptr) {
		vals.emplace_back(val);
	}
	return;
}

void JSONCommon::GetWildcardPath(const JSONNode *val, const char *ptr, const idx_t &len,
                                 vector<const JSONNode *> &vals) {
	// Path has been validated at this point
	const char *const end = ptr + len;
	ptr++; // Skip past '$'
	GetWildcardPathInternal(val, ptr, end, vals);
}

const JSONNode *JSONCommon::GetPointer(const JSONNode *val, const char *ptr, idx_t len) {
	const char *const end = ptr + len;
	string token;
	while (val && ptr < end) {
		if (*ptr != '/') {
			return nullptr;
		}
		ptr++;
		token.clear();
		while (ptr < end && *ptr != '/') {
			if (*ptr == '~') {
				if (ptr + 1 >= end || (ptr[1] != '0' && ptr[1] != '1')) {
					return nullptr;
				}
				token += ptr[1] == '0' ? '~' : '/';
				ptr += 2;
				continue;
			}
			token += *ptr++;
		}
		if (val->type == JSONValueType::OBJECT) {
			val = val->Get(std::string_view(token));
		} else if (val->type == JSONValueType::ARRAY) {
			if (token.empty() || (token.size() > 1 && token[0] == '0')) {
				return nullptr;
			}
			idx_t index = 0;
			for (auto c : token) {
				if (!StringUtil::CharacterIsDigit(c) || index > NumericLimits<uint32_t>::Maximum()) {
					return nullptr;
				}
				index = index * 10 + static_cast<idx_t>(c - '0');
			}
			val = val->Get(index);
		} else {
			return nullptr;
		}
	}
	return val;
}

} // namespace duckdb
