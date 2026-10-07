//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/tokenizer/tokenizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/peg/keyword_helper.hpp"
#include "duckdb/parser/peg/token_type.hpp"
#include "duckdb/parser/peg/matcher_token.hpp"

namespace duckdb {

enum class TokenizeState {
	STANDARD = 0,
	SINGLE_LINE_COMMENT,
	MULTI_LINE_COMMENT,
	QUOTED_IDENTIFIER,
	STRING_LITERAL,
	KEYWORD,
	NUMERIC,
	OPERATOR,
	DOLLAR_QUOTED_STRING
};

class Tokenizer;

class TokenizerBehavior {
public:
	TokenizerBehavior(std::string_view sql, vector<MatcherToken> &tokens);
	virtual ~TokenizerBehavior() = default;

public:
	virtual void PushToken(idx_t start, idx_t end, TokenType type, bool unterminated = false);
	virtual void OnStatementEnd(idx_t pos);
	virtual void OnLastToken(const Tokenizer &tokenizer, TokenizeState state, std::string_view last_word,
	                         idx_t last_pos);

	//! Sentinel appended at the end of the token vector on a clean exit. Override to return
	//! `END_OF_INPUT_AUTOCOMPLETE` for autocomplete behavior. Dirty exits (unterminated comment /
	//! dollar-quote) always append `END_OF_INPUT` regardless of this hook.
	virtual TokenType GetTerminator() const {
		return TokenType::END_OF_INPUT;
	}

public:
	std::string_view sql;
	vector<MatcherToken> &tokens;
	bool has_block_comment = false;
	idx_t last_block_comment_position = 0;
};

class Tokenizer {
public:
	explicit Tokenizer(const PEGKeywordHelper &keyword_helper);

public:
	//! Tokenize the behavior's input and return whether autocomplete can be offered.
	bool TokenizeInput(TokenizerBehavior &behavior) const;

protected:
	bool BackslashEscapesStringLiterals() const {
		return false;
	}
	bool IsQuotedIdentifierDelimiter(char character) const {
		return character == '"';
	}
	void PushOperatorToken(TokenizerBehavior &behavior, idx_t start, idx_t end) const;
	void HandleLastToken(TokenizerBehavior &behavior, TokenizeState state, std::string_view sql, idx_t last_pos) const;

private:
	//! Core tokenization loop. Returns true on a clean exit, false if the input ended inside an
	//! unterminated comment / dollar-quoted string. Does NOT append the trailing sentinel —
	//! `TokenizeInput()` is the one that appends `GetTerminator()` (clean) or `END_OF_INPUT`
	//! (dirty) based on the return value.
	bool TokenizeInputInternal(TokenizerBehavior &behavior) const;
	bool IsCompoundColonToken(std::string_view sql, idx_t pos, idx_t &token_length) const;
	static bool IsHashOperatorToken(std::string_view sql, idx_t pos, idx_t &token_length);

public:
	static bool IsSingleByteOperator(char c) {
		switch (c) {
		case '(':
		case ')':
		case '{':
		case '}':
		case '[':
		case ']':
		case ',':
		case ':':
		case '?':
		case '$':
		case '#':
			return true;
		default:
			return false;
		}
	}
	static bool CharacterIsInitialNumber(char c) {
		if (c >= '0' && c <= '9') {
			return true;
		}
		return c == '.';
	}
	static bool CharacterIsNumber(char c) {
		if (CharacterIsInitialNumber(c)) {
			return true;
		}
		switch (c) {
		case 'e': // exponents
		case 'E':
		case '_':
			return true;
		default:
			return false;
		}
	}
	static bool CharacterIsScientific(char c) {
		switch (c) {
		case 'e':
		case 'E':
			return true;
		default:
			return false;
		}
	}
	static bool CharacterIsControlFlow(char c) {
		switch (c) {
		case '\'':
		case ';':
		case '"':
		case '.':
			return true;
		default:
			return false;
		}
	}
	static bool CharacterIsKeyword(char c) {
		if (IsSingleByteOperator(c)) {
			return false;
		}
		if (StringUtil::CharacterIsOperator(c)) {
			return false;
		}
		if (StringUtil::CharacterIsSpace(c)) {
			return false;
		}
		if (CharacterIsControlFlow(c)) {
			return false;
		}
		return true;
	}
	static bool CharacterIsOperator(char c) {
		switch (c) {
		case '+':
		case '-':
		case '*':
		case '/':
		case '%':
		case '^':
		case '<':
		case '>':
		case '=':
		case '~':
		case '!':
		case '@':
		case '&':
		case '|':
			return true;
		default:
			return false;
		}
	}
	static bool CharacterIsSpecialStringCharacter(char c) {
		if (c == 'N' || c == 'n') {
			return true;
		}
		if (c == 'X' || c == 'x') {
			return true;
		}
		if (c == 'E' || c == 'e') {
			return true;
		}
		if (c == 'B' || c == 'b') {
			return true;
		}
		return false;
	}
	static bool IsValidDollarTagCharacter(char c);
	static TokenType TokenizeStateToType(TokenizeState state);
	static bool IsUnterminatedState(TokenizeState state);

public:
	const PEGKeywordHelper &keyword_helper;
};

} // namespace duckdb
