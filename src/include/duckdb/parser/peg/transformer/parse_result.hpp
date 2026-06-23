#pragma once
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/arena_linked_list.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/peg/special_string_utils.hpp"
#include "duckdb/common/windows_undefs.hpp"
#include "fast_float/fast_float.h"

#include <span>

namespace duckdb {

enum class TokenType {
	INVALID,
	KEYWORD,
	STRING_LITERAL,
	NUMBER_LITERAL,
	OPERATOR,
	IDENTIFIER,
	COMMENT,
	TERMINATOR,
	CATALOG_NAME,
	SCHEMA_NAME,
	TABLE_NAME,
	TYPE_NAME,
	COLUMN_NAME,
	SCALAR_FUNCTION,
	TABLE_FUNCTION,
	PRAGMA_FUNCTION,
	SETTING_NAME,
	ERROR
};

inline string TokenTypeToString(TokenType type) {
	switch (type) {
	case TokenType::KEYWORD:
		return "KEYWORD";
	case TokenType::STRING_LITERAL:
		return "STRING_LITERAL";
	case TokenType::NUMBER_LITERAL:
		return "NUMBER_LITERAL";
	case TokenType::OPERATOR:
		return "OPERATOR";
	case TokenType::IDENTIFIER:
		return "IDENTIFIER";
	case TokenType::COMMENT:
		return "COMMENT";
	case TokenType::TERMINATOR:
		return "TERMINATOR";
	case TokenType::ERROR:
		return "ERROR";
	case TokenType::CATALOG_NAME:
		return "CATALOG_NAME";
	case TokenType::SCHEMA_NAME:
		return "SCHEMA_NAME";
	case TokenType::TABLE_NAME:
		return "TABLE_NAME";
	case TokenType::TYPE_NAME:
		return "TYPE_NAME";
	case TokenType::COLUMN_NAME:
		return "COLUMN_NAME";
	case TokenType::SCALAR_FUNCTION:
		return "SCALAR_FUNCTION";
	case TokenType::TABLE_FUNCTION:
		return "TABLE_FUNCTION";
	case TokenType::PRAGMA_FUNCTION:
		return "PRAGMA_FUNCTION";
	case TokenType::SETTING_NAME:
		return "SETTING_NAME";
	default:
		return "UNKNOWN";
	}
}

class PEGTransformer; // Forward declaration

enum class ParseResultType : uint8_t {
	LIST,
	OPTIONAL,
	REPEAT,
	CHOICE,
	EXPRESSION,
	IDENTIFIER,
	KEYWORD,
	OPERATOR,
	STATEMENT,
	EXTENSION,
	NUMBER,
	STRING,
	INVALID
};

inline const char *ParseResultToString(ParseResultType type) {
	switch (type) {
	case ParseResultType::LIST:
		return "LIST";
	case ParseResultType::OPTIONAL:
		return "OPTIONAL";
	case ParseResultType::REPEAT:
		return "REPEAT";
	case ParseResultType::CHOICE:
		return "CHOICE";
	case ParseResultType::EXPRESSION:
		return "EXPRESSION";
	case ParseResultType::IDENTIFIER:
		return "IDENTIFIER";
	case ParseResultType::KEYWORD:
		return "KEYWORD";
	case ParseResultType::OPERATOR:
		return "OPERATOR";
	case ParseResultType::STATEMENT:
		return "STATEMENT";
	case ParseResultType::EXTENSION:
		return "EXTENSION";
	case ParseResultType::NUMBER:
		return "NUMBER";
	case ParseResultType::STRING:
		return "STRING";
	case ParseResultType::INVALID:
		return "INVALID";
	}
	return "INVALID";
}

class ParseResult {
public:
	explicit ParseResult(ParseResultType type, optional_idx offset) : type(type), offset(offset) {
	}
	virtual ~ParseResult() = default;

	ParseResult(const ParseResult &) = delete;
	ParseResult &operator=(const ParseResult &) = delete;

	template <class TARGET>
	TARGET &Cast() {
		if (TARGET::TYPE != ParseResultType::INVALID && type != TARGET::TYPE) {
			throw InternalException("Failed to cast parse result of type %s to type %s for rule %s",
			                        ParseResultToString(TARGET::TYPE), ParseResultToString(type), name);
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	ParseResultType type;
	std::string_view name;
	optional_idx offset;

	virtual void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                              const std::string &indent, bool is_last) const {
		ss << indent << (is_last ? "└─" : "├─") << " " << ParseResultToString(type);
		if (!name.empty()) {
			ss << " (" << name << ")";
		}
	}

	// The public entry point
	std::string ToString() const {
		std::stringstream ss;
		std::unordered_set<const ParseResult *> visited;
		// The root is always the "last" element at its level
		ToStringInternal(ss, visited, "", true);
		return ss.str();
	}
};

struct IdentifierParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::IDENTIFIER;
	std::string_view identifier;

	explicit IdentifierParseResult(std::string_view identifier_p, optional_idx offset)
	    : ParseResult(TYPE, offset), identifier(identifier_p) {
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": " << identifier << "\n";
	}
};

struct KeywordParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::KEYWORD;
	std::string_view keyword;

	explicit KeywordParseResult(std::string_view keyword_p, optional_idx offset)
	    : ParseResult(TYPE, offset), keyword(keyword_p) {
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": \"" << keyword << "\"\n";
	}
};

struct ListParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::LIST;

public:
	explicit ListParseResult(std::span<reference<ParseResult>> children_p, std::string_view name_p, optional_idx offset)
	    : ParseResult(TYPE, offset), children(children_p) {
		name = name_p;
	}

	std::span<reference<ParseResult>> GetChildren() const {
		return children;
	}

	ParseResult &GetChild(idx_t index) {
		if (index >= children.size()) {
			throw InternalException("Child index out of bounds");
		}
		return children[index].get();
	}

	template <class T>
	T &Child(idx_t index) {
		return GetChild(index).Cast<T>();
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ss << indent << (is_last ? "└─" : "├─");

		if (visited.count(this)) {
			ss << " List (" << name << ") [... already printed ...]\n";
			return;
		}
		visited.insert(this);

		ss << " " << ParseResultToString(type);
		if (!name.empty()) {
			ss << " (" << name << ")";
		}
		ss << " [" << children.size() << " children]\n";

		std::string child_indent = indent + (is_last ? "   " : "│  ");
		for (size_t i = 0; i < children.size(); ++i) {
			children[i].get().ToStringInternal(ss, visited, child_indent, i == children.size() - 1);
		}
	}

private:
	std::span<reference<ParseResult>> children;
};

struct RepeatParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::REPEAT;

	explicit RepeatParseResult(std::span<reference<ParseResult>> children_p, optional_idx offset)
	    : ParseResult(TYPE, offset), children(children_p) {
	}

	std::span<reference<ParseResult>> GetChildren() const {
		return children;
	}

	template <class T>
	T &Child(idx_t index) {
		if (index >= children.size()) {
			throw InternalException("Child index out of bounds");
		}
		return children[index].get().Cast<T>();
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ss << indent << (is_last ? "└─" : "├─");

		if (visited.count(this)) {
			ss << " Repeat (" << name << ") [... already printed ...]\n";
			return;
		}
		visited.insert(this);

		ss << " " << ParseResultToString(type);
		if (!name.empty()) {
			ss << " (" << name << ")";
		}
		ss << " [" << children.size() << " children]\n";

		std::string child_indent = indent + (is_last ? "   " : "│  ");
		for (size_t i = 0; i < children.size(); ++i) {
			children[i].get().ToStringInternal(ss, visited, child_indent, i == children.size() - 1);
		}
	}

private:
	std::span<reference<ParseResult>> children;
};

struct OptionalParseResult : ParseResult {
	static constexpr ParseResultType TYPE = ParseResultType::OPTIONAL;

	explicit OptionalParseResult() : ParseResult(TYPE, optional_idx()), optional_result(nullptr) {
	}
	explicit OptionalParseResult(optional_ptr<ParseResult> result_p, optional_idx offset)
	    : ParseResult(TYPE, offset), optional_result(result_p) {
		name = result_p->name;
	}

	bool HasResult() const {
		return optional_result != nullptr;
	}

	ParseResult &GetResultUnsafe() {
		D_ASSERT(optional_result);
		return *optional_result;
	}

	ParseResult &GetResult() {
		if (!optional_result) {
			throw InternalException("OptionalParseResult is null");
		}
		return *optional_result;
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		if (HasResult()) {
			// The optional node has a value, so we "collapse" it by just printing its child.
			// We pass the same indentation and is_last status, so it takes the place of the Optional node.
			optional_result->ToStringInternal(ss, visited, indent, is_last);
		} else {
			// The optional node is empty, which is useful information, so we print it.
			ss << indent << (is_last ? "└─" : "├─") << " " << ParseResultToString(type) << " [empty]\n";
		}
	}

private:
	optional_ptr<ParseResult> optional_result;
};

class ChoiceParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::CHOICE;

	explicit ChoiceParseResult(ParseResult &parse_result_p, idx_t selected_idx_p, optional_idx offset)
	    : ParseResult(TYPE, offset), result(parse_result_p), selected_idx(selected_idx_p) {
		name = parse_result_p.name;
	}

	ParseResult &GetResult() {
		return result;
	}

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		// The choice was resolved. We print a marker and then print the child below it.
		ss << indent << (is_last ? "└─" : "├─") << " [" << ParseResultToString(type) << " (idx: " << selected_idx
		   << ")] ->\n";

		// The child is now on a new indentation level and is the only child of our marker.
		std::string child_indent = indent + (is_last ? "   " : "│  ");
		result.ToStringInternal(ss, visited, child_indent, true);
	}

private:
	ParseResult &result;
	idx_t selected_idx;
};

class NumberParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::NUMBER;

	explicit NumberParseResult(std::string_view number_p, optional_idx offset)
	    : ParseResult(TYPE, offset), number(number_p) {
	}
	std::string_view number;

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": " << number << "\n";
	}
};

class StringLiteralParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::STRING;

	explicit StringLiteralParseResult(std::string_view string_p, SpecialStringCharacter string_type_p,
	                                  optional_idx offset)
	    : ParseResult(TYPE, offset), result(string_p), string_type(string_type_p) {
	}

	std::string_view GetRawString() const {
		return result;
	}

	unique_ptr<ParsedExpression> ToExpression() {
		switch (string_type) {
		case SpecialStringCharacter::STANDARD:
			return make_uniq<ConstantExpression>(Value(result));
		case SpecialStringCharacter::NATIONAL_STRING:
			return make_uniq<CastExpression>(LogicalType::VARCHAR, make_uniq<ConstantExpression>(Value(result)));
		case SpecialStringCharacter::HEXADECIMAL_STRING: {
			// result contains raw hex digits (e.g. "FF" for X'FF')
			if (result.size() % 2 != 0) {
				throw ParserException("Hex string literal must have an even number of hex digits");
			}
			// Build \xHH-escaped string that Blob::ToBlob (via Value::BLOB) expects
			idx_t blob_len = result.size() / 2;
			string escaped;
			escaped.reserve(blob_len * 4);
			for (idx_t i = 0; i < result.size(); i += 2) {
				escaped += "\\x";
				escaped += result[i];
				escaped += result[i + 1];
			}
			return make_uniq<ConstantExpression>(Value::BLOB(escaped));
		}
		case SpecialStringCharacter::BIT_STRING:
			return make_uniq<ConstantExpression>(Value(absl::StrCat("b", result)));
		case SpecialStringCharacter::ESCAPE_STRING:
			string escaped_result;
			escaped_result.reserve(result.size());

			for (size_t i = 0; i < result.size(); ++i) {
				if (result[i] == '\\' && i + 1 < result.size()) {
					i++;
					switch (result[i]) {
					case 'b':
						escaped_result += '\b';
						break;
					case 'f':
						escaped_result += '\f';
						break;
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7': {
						size_t oct_start = i;
						size_t oct_end = oct_start + 1;
						while (oct_end < result.size() && oct_end - oct_start < 3 && result[oct_end] >= '0' &&
						       result[oct_end] <= '7') {
							oct_end++;
						}
						uint64_t oct_val = 0;
						fast_float::from_chars(result.data() + oct_start, result.data() + oct_end, oct_val, 8);
						escaped_result += static_cast<char>(oct_val);
						i = oct_end - 1;
						break;
					}
					case 'x': {
						size_t hex_start = i + 1;
						size_t hex_end = hex_start;
						while (hex_end < result.size() && hex_end - hex_start < 2 &&
						       StringUtil::CharacterIsHex(result[hex_end])) {
							hex_end++;
						}
						if (hex_end > hex_start) {
							uint64_t hex_val = 0;
							fast_float::from_chars(result.data() + hex_start, result.data() + hex_end, hex_val, 16);
							escaped_result += static_cast<char>(hex_val);
							i = hex_end - 1;
						} else {
							escaped_result += 'x';
						}
						break;
					}
					case 'n':
						escaped_result += '\n';
						break;
					case 't':
						escaped_result += '\t';
						break;
					case 'r':
						escaped_result += '\r';
						break;
					case '\\':
						escaped_result += '\\';
						break;
					case '\'':
						escaped_result += '\'';
						break;
					default:
						escaped_result += result[i];
						break;
					}
				} else {
					escaped_result += result[i];
				}
			}
			if (escaped_result.find('\0') != string::npos) {
				throw ParserException("Null character not permitted in escape string literal");
			}
			UnicodeInvalidReason reason;
			size_t pos;
			auto utf_validity = Utf8Proc::Analyze(escaped_result.c_str(), escaped_result.size(), &reason, &pos);
			if (utf_validity == UnicodeType::INVALID) {
				const char *reason_str =
				    reason == UnicodeInvalidReason::BYTE_MISMATCH ? "byte mismatch" : "invalid unicode codepoint";
				throw ParserException("Invalid UTF-8 in escape string literal at byte offset %d: %s", pos, reason_str);
			}
			return make_uniq<ConstantExpression>(Value(escaped_result));
		}
		return make_uniq<ConstantExpression>(Value(result));
	}

	std::string_view result;

	SpecialStringCharacter string_type;

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		string special_string;
		if (string_type == SpecialStringCharacter::ESCAPE_STRING) {
			special_string = "E";
		} else if (string_type == SpecialStringCharacter::NATIONAL_STRING) {
			special_string = "N";
		} else if (string_type == SpecialStringCharacter::HEXADECIMAL_STRING) {
			special_string = "X";
		}
		ss << ": " << special_string << "\"" << result << "\"n";
	}
};

class OperatorParseResult : public ParseResult {
public:
	static constexpr ParseResultType TYPE = ParseResultType::OPERATOR;

	explicit OperatorParseResult(std::string_view operator_p, optional_idx offset)
	    : ParseResult(TYPE, offset), operator_token(operator_p) {
	}
	std::string_view operator_token;

	void ToStringInternal(std::stringstream &ss, std::unordered_set<const ParseResult *> &visited,
	                      const std::string &indent, bool is_last) const override {
		ParseResult::ToStringInternal(ss, visited, indent, is_last);
		ss << ": " << operator_token << "\n";
	}
};

} // namespace duckdb
