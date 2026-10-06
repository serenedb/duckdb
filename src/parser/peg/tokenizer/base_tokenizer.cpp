#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/tokenizer/tokenizer.hpp"
#include "duckdb/parser/peg/keyword_helper.hpp"

namespace duckdb {

TokenizerBehavior::TokenizerBehavior(std::string_view sql, vector<MatcherToken> &tokens) : sql(sql), tokens(tokens) {
}

Tokenizer::Tokenizer(const PEGKeywordHelper &keyword_helper_p) : keyword_helper(keyword_helper_p) {
}

bool Tokenizer::BackslashEscapesStringLiterals() const {
	return false;
}

bool Tokenizer::IsQuotedIdentifierDelimiter(char character) const {
	return character == '"';
}

void Tokenizer::HandleLastToken(TokenizerBehavior &behavior, TokenizeState state, std::string_view sql,
                                idx_t last_pos) const {
	behavior.OnLastToken(*this, state, sql.substr(last_pos), last_pos);
}

bool Tokenizer::IsCompoundColonToken(std::string_view sql, idx_t pos, idx_t &token_length) const {
	if (pos + 1 >= sql.size() || sql[pos] != ':') {
		return false;
	}
	if (sql[pos + 1] != ':' && sql[pos + 1] != '=') {
		return false;
	}
	token_length = 2;
	return true;
}

bool Tokenizer::IsHashOperatorToken(std::string_view sql, idx_t pos, idx_t &token_length) {
	const auto rest = sql.substr(pos);
	switch (rest[0]) {
	case '<':
		if (rest.starts_with("<#>")) {
			token_length = 3;
			return true;
		}
		return false;
	case '#':
		if (rest.starts_with("#>>")) {
			token_length = 3;
			return true;
		}
		if (rest.starts_with("#>") || rest.starts_with("##")) {
			token_length = 2;
			return true;
		}
		return false;
	default:
		return false;
	}
}

bool Tokenizer::IsSingleByteOperator(char c) {
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

bool Tokenizer::CharacterIsInitialNumber(char c) {
	if (c >= '0' && c <= '9') {
		return true;
	}
	return c == '.';
}

bool Tokenizer::CharacterIsSpecialStringCharacter(char c) {
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

bool Tokenizer::CharacterIsNumber(char c) {
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

bool Tokenizer::CharacterIsScientific(char c) {
	switch (c) {
	case 'e':
	case 'E':
		return true;
	default:
		return false;
	}
}

bool Tokenizer::CharacterIsControlFlow(char c) {
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

bool Tokenizer::CharacterIsKeyword(char c) {
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

bool Tokenizer::CharacterIsOperator(char c) {
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

TokenType Tokenizer::TokenizeStateToType(TokenizeState state) {
	switch (state) {
	case TokenizeState::STANDARD:
		return TokenType::IDENTIFIER;
	case TokenizeState::SINGLE_LINE_COMMENT:
		return TokenType::COMMENT;
	case TokenizeState::MULTI_LINE_COMMENT:
		return TokenType::COMMENT;
	case TokenizeState::QUOTED_IDENTIFIER:
		return TokenType::IDENTIFIER;
	case TokenizeState::STRING_LITERAL:
		return TokenType::STRING_LITERAL;
	case TokenizeState::KEYWORD:
		return TokenType::KEYWORD;
	case TokenizeState::NUMERIC:
		return TokenType::NUMBER_LITERAL;
	case TokenizeState::OPERATOR:
		return TokenType::OPERATOR;
	case TokenizeState::DOLLAR_QUOTED_STRING:
		return TokenType::STRING_LITERAL;
	default:
		throw InternalException("Unknown token type");
	}
}

void TokenizerBehavior::PushToken(idx_t start, idx_t end, TokenType type, bool unterminated) {
	if (type == TokenType::COMMENT) {
		if (end >= start + 2 && sql[start] == '/' && sql[start + 1] == '*') {
			has_block_comment = true;
			last_block_comment_position = start;
		}
		return;
	}
	if (start >= end) {
		return;
	}
	tokens.emplace_back(sql.substr(start, end - start), start, type, unterminated);
	if (tokens.size() < 2) {
		return;
	}
	auto &previous_token = tokens[tokens.size() - 2];
	auto previous_token_end = previous_token.offset + previous_token.length;
	if (has_block_comment && last_block_comment_position >= previous_token_end && last_block_comment_position < start) {
		tokens.back().preceded_by_block_comment = true;
	}
	for (idx_t pos = previous_token_end; pos < start; pos++) {
		if (StringUtil::CharacterIsNewline(sql[pos])) {
			tokens.back().preceded_by_newline = true;
			break;
		}
	}
}

// Valid characters can be between A-Z, a-z, 0-9, underscore, or \200 - \377
// Note: 0-9 are only valid after the first character. Callers are expected to validate that before calling this
// function.
bool Tokenizer::IsValidDollarTagCharacter(char c) {
	if (c >= 'A' && c <= 'Z') {
		return true;
	}
	if (c >= 'a' && c <= 'z') {
		return true;
	}
	if (c >= '0' && c <= '9') {
		return true;
	}
	if (c == '_') {
		return true;
	}
	if ((unsigned char)c >= (unsigned char)'\200') {
		return true;
	}
	return false;
}

bool Tokenizer::IsUnterminatedState(TokenizeState state) {
	switch (state) {
	case TokenizeState::STRING_LITERAL:
	case TokenizeState::QUOTED_IDENTIFIER:
	case TokenizeState::DOLLAR_QUOTED_STRING:
		return true;
	default:
		return false;
	}
}

bool Tokenizer::TokenizeInput(TokenizerBehavior &behavior) const {
	auto &sql = behavior.sql;
	auto &tokens = behavior.tokens;
	if (TokenizeInputInternal(behavior)) {
		auto terminator = behavior.GetTerminator();
		tokens.emplace_back("", sql.size(), terminator);
		if (terminator == TokenType::END_OF_INPUT_AUTOCOMPLETE) {
			return true;
		}
	} else {
		tokens.emplace_back("", sql.size(), TokenType::END_OF_INPUT);
	}
	return false;
}

void Tokenizer::PushOperatorToken(TokenizerBehavior &behavior, idx_t start, idx_t end) const {
	auto &sql = behavior.sql;
	auto &tokens = behavior.tokens;
	// Apply PostgreSQL trimming rule: an operator cannot end in '+' or '-' unless
	// it contains at least one of: ~ ! @ # % ^ & | ` ?
	idx_t end_pos = end;
	bool has_special = false;
	for (idx_t pos = start; pos < end_pos; pos++) {
		char operator_char = sql[pos];
		if (operator_char == '~' || operator_char == '!' || operator_char == '@' || operator_char == '#' ||
		    operator_char == '%' || operator_char == '^' || operator_char == '&' || operator_char == '|' ||
		    operator_char == '`' || operator_char == '?') {
			has_special = true;
			break;
		}
	}
	if (!has_special) {
		while (end_pos > start + 1 && (sql[end_pos - 1] == '+' || sql[end_pos - 1] == '-')) {
			end_pos--;
		}
	}
	behavior.PushToken(start, end_pos, TokenType::OPERATOR);
	// Push any trimmed '+' or '-' characters as individual tokens
	for (idx_t pos = end_pos; pos < end; pos++) {
		tokens.emplace_back(sql.substr(pos, 1), pos, TokenType::OPERATOR);
	}
}

bool Tokenizer::TokenizeInputInternal(TokenizerBehavior &behavior) const {
	auto &sql = behavior.sql;
	auto &tokens = behavior.tokens;
	auto state = TokenizeState::STANDARD;
	idx_t last_pos = 0;
	bool escape_string = false;
	char quoted_identifier_delimiter = '"';
	std::string_view dollar_quote_marker;
	idx_t dollar_marker_start = 0;
	idx_t multi_line_comment_depth = 0;
	for (idx_t i = 0; i < sql.size(); i++) {
		auto c = sql[i];
		switch (state) {
		case TokenizeState::STANDARD:
			if (c == '\'') {
				state = TokenizeState::STRING_LITERAL;
				last_pos = i;
				escape_string = false;
				break;
			}
			if (IsQuotedIdentifierDelimiter(c)) {
				state = TokenizeState::QUOTED_IDENTIFIER;
				quoted_identifier_delimiter = c;
				last_pos = i;
				break;
			}
			if (c == ';') {
				// end of statement
				behavior.OnStatementEnd(i);
				last_pos = i + 1;
				break;
			}
			if (c == '$') {
				// Dollar-quoted string statement
				if (i + 1 >= sql.size()) {
					// We need more than a single dollar
					break;
				}
				if (sql[i + 1] >= '0' && sql[i + 1] <= '9') {
					// $[numeric] is a parameter, not a dollar-quoted string
					tokens.emplace_back(sql.substr(i, 1), i, TokenType::OPERATOR);
					break;
				}
				// Dollar-quoted string or collabel parameter ($collabel)
				last_pos = i;
				// Scan until next $
				idx_t next_dollar = 0;
				for (idx_t idx = i + 1; idx < sql.size(); idx++) {
					if (sql[idx] == '$') {
						next_dollar = idx;
						break;
					}
					if (!IsValidDollarTagCharacter(sql[idx])) {
						break;
					}
				}
				if (next_dollar == 0) {
					// Collabel parameter ($collabel)
					tokens.emplace_back(sql.substr(i, 1), i, TokenType::OPERATOR);
					break;
				}
				state = TokenizeState::DOLLAR_QUOTED_STRING;
				last_pos = i;
				i = next_dollar;
				if (i < sql.size()) {
					// Found a complete marker, store it.
					dollar_marker_start = last_pos + 1;
					dollar_quote_marker = sql.substr(dollar_marker_start, i - dollar_marker_start);
				}
				break;
			}
			if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
				i++;
				state = TokenizeState::SINGLE_LINE_COMMENT;
				break;
			}
			if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
				i++;
				state = TokenizeState::MULTI_LINE_COMMENT;
				multi_line_comment_depth = 1;
				break;
			}
			if (StringUtil::CharacterIsSpace(c)) {
				// space character - skip
				last_pos = i + 1;
				break;
			}
			idx_t token_length;
			if (IsCompoundColonToken(sql, i, token_length)) {
				if (i + token_length < sql.size() && CharacterIsOperator(sql[i + token_length])) {
					state = TokenizeState::OPERATOR;
					last_pos = i;
					break;
				}
				// Push the compound colon token
				tokens.emplace_back(sql.substr(i, token_length), last_pos, TokenType::OPERATOR);
				i += token_length - 1;
				last_pos = i + 1;
				break;
			}
			if (IsHashOperatorToken(sql, i, token_length)) {
				tokens.emplace_back(sql.substr(i, token_length), last_pos, TokenType::OPERATOR);
				i += token_length - 1;
				last_pos = i + 1;
				break;
			}
			if (IsSingleByteOperator(c)) {
				// single-byte operator - directly push the token
				tokens.emplace_back(sql.substr(i, 1), last_pos, TokenType::OPERATOR);
				last_pos = i + 1;
				break;
			}
			if (CharacterIsInitialNumber(c)) {
				// parse a numeric literal
				state = TokenizeState::NUMERIC;
				last_pos = i;
				break;
			}
			if (CharacterIsSpecialStringCharacter(c)) {
				// Look ahead to see if a quote follows
				if (i + 1 < sql.size() && sql[i + 1] == '\'') {
					state = TokenizeState::STRING_LITERAL;
					last_pos = i;
					if (c == 'E' || c == 'e') {
						escape_string = true;
					}
					i++;
					break;
				}
			}
			if (StringUtil::CharacterIsOperator(c)) {
				state = TokenizeState::OPERATOR;
				last_pos = i;
				break;
			}
			state = TokenizeState::KEYWORD;
			last_pos = i;
			break;
		case TokenizeState::NUMERIC:
			// Hex literal `0x...`/`0X...` and binary `0b...`/`0B...`: after a leading `0`, allow the
			// prefix character and treat subsequent hex/bin digits as part of the same number token.
			if (i == last_pos + 1 && sql[last_pos] == '0' && (c == 'x' || c == 'X' || c == 'b' || c == 'B')) {
				break; // consume the prefix; remaining hex/bin digits handled below
			}
			if (i > last_pos + 1 && sql[last_pos] == '0' && (sql[last_pos + 1] == 'x' || sql[last_pos + 1] == 'X') &&
			    StringUtil::CharacterIsHex(c)) {
				break;
			}
			// A second '.' inside the same token, or a '.' that would be followed by an identifier
			// character (e.g. `$1.x`, `tbl.col`, `1.method()`), is not part of the number.
			// Stop here so the '.' becomes a separate DotOperator token.
			if (c == '.') {
				bool already_has_dot = false;
				for (idx_t j = last_pos; j < i; j++) {
					if (sql[j] == '.') {
						already_has_dot = true;
						break;
					}
				}
				// What follows the '.' decides whether it is part of the number:
				//   digit          -> fraction,      `1.5`
				//   exponent       -> trailing dot,  `1.e5`, `4664.E+5`
				//   identifier     -> field access,  `tbl.col`, `1.method()`, `$1.x`
				//   anything else  -> trailing dot,  `42.`, `42.)`, `42.::INT`, `42.` at EOF
				bool dot_is_part_of_number;
				if (already_has_dot) {
					dot_is_part_of_number = false;
				} else if (i + 1 >= sql.size()) {
					dot_is_part_of_number = true;
				} else if (StringUtil::CharacterIsDigit(sql[i + 1])) {
					dot_is_part_of_number = true;
				} else if (CharacterIsScientific(sql[i + 1])) {
					// Only when a real exponent follows, so `1.e5` is a number while
					// `1.exp` stays a field access.
					idx_t j = i + 2;
					if (j < sql.size() && (sql[j] == '+' || sql[j] == '-')) {
						j++;
					}
					dot_is_part_of_number = j < sql.size() && StringUtil::CharacterIsDigit(sql[j]);
				} else {
					dot_is_part_of_number = !StringUtil::CharacterIsAlpha(sql[i + 1]) && sql[i + 1] != '_';
				}
				if (!dot_is_part_of_number) {
					behavior.PushToken(last_pos, i, TokenType::NUMBER_LITERAL);
					state = TokenizeState::STANDARD;
					last_pos = i;
					i--;
					break;
				}
				break; // Decimal point inside a number like `1.5`.
			}
			// Check for "always allowed" numeric characters
			if (CharacterIsInitialNumber(c)) {
				break; // Continue tokenizing
			}
			// Allow underscore only when immediately followed by a digit (no consecutive underscores)
			if (c == '_' && i + 1 < sql.size() && CharacterIsInitialNumber(sql[i + 1])) {
				break;
			}

			// Check for scientific notation marker
			if (CharacterIsScientific(c)) {
				// (e.g., "1ee5" is invalid)
				if (!CharacterIsScientific(sql[i - 1])) {
					// Require at least one digit before 'e'/'E' (e.g., ".e100" is not a number)
					if (StringUtil::CharacterIsDigit(sql[last_pos])) {
						break; // Number starts with a digit (e.g., "1e5", "1.e5")
					}
					if (StringUtil::CharacterIsDigit(sql[i - 1])) {
						break; // Digit immediately before 'e' (e.g., ".1e5")
					}
				}
			}

			// Check for '+' or '-'
			if (c == '+' || c == '-') {
				if (CharacterIsScientific(sql[i - 1])) {
					break; // Valid, e.g., "1e-5". Continue.
				}
				// Invalid, e.g., "1+5" or "1e5+5". Fall through to stop.
			}

			// --- End of number ---
			// The character 'c' is not a valid part of the number.
			// Stop tokenizing and backtrack as per your original logic.
			// Hex/binary tokens keep all consumed chars; the decimal-number backtrack
			// would otherwise trim through the hex/bin digits and discard them.
			{
				bool is_hex_or_bin = i > last_pos + 1 && sql[last_pos] == '0' &&
				                     (sql[last_pos + 1] == 'x' || sql[last_pos + 1] == 'X' ||
				                      sql[last_pos + 1] == 'b' || sql[last_pos + 1] == 'B');
				if (!is_hex_or_bin) {
					while (!CharacterIsInitialNumber(sql[i - 1])) {
						i--;
					}
				}
			}
			behavior.PushToken(last_pos, i, TokenType::NUMBER_LITERAL);
			state = TokenizeState::STANDARD;
			last_pos = i;
			i--;
			break;
		case TokenizeState::OPERATOR:
			if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
				PushOperatorToken(behavior, last_pos, i);
				state = TokenizeState::STANDARD;
				last_pos = i;
				i--;
				break;
			}
			if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
				PushOperatorToken(behavior, last_pos, i);
				// Go back to STANDARD state so it tokenizes as a block comment
				state = TokenizeState::STANDARD;
				last_pos = i;
				i--;
				break;
			}
			// operator literal - check if this is still an operator
			if (!CharacterIsOperator(c)) {
				PushOperatorToken(behavior, last_pos, i);
				state = TokenizeState::STANDARD;
				last_pos = i;
				i--;
			}
			break;
		case TokenizeState::KEYWORD:
			// keyword - check if this is still a keyword
			// '$' is valid as a non-initial identifier character in PostgreSQL
			if (c != '$' && !CharacterIsKeyword(c)) {
				// not a keyword - return to standard state
				auto word = sql.substr(last_pos, i - last_pos);
				auto token_type = keyword_helper.IsKeyword(word) ? TokenType::KEYWORD : TokenType::IDENTIFIER;
				behavior.PushToken(last_pos, i, token_type);
				state = TokenizeState::STANDARD;
				last_pos = i;
				i--;
			}
			break;
		case TokenizeState::STRING_LITERAL:
			if ((escape_string || BackslashEscapesStringLiterals()) && c == '\\' && i + 1 < sql.size()) {
				i++;
				break;
			}
			if (c == '\'') {
				if (i + 1 < sql.size() && sql[i + 1] == '\'') {
					// escaped - skip escape
					i++;
				} else {
					behavior.PushToken(last_pos, i + 1, TokenType::STRING_LITERAL);
					last_pos = i + 1;
					escape_string = false;
					state = TokenizeState::STANDARD;
				}
			}
			break;
		case TokenizeState::QUOTED_IDENTIFIER:
			if (c == quoted_identifier_delimiter) {
				if (i + 1 < sql.size() && sql[i + 1] == quoted_identifier_delimiter) {
					// escaped - skip escape
					i++;
				} else {
					behavior.PushToken(last_pos, i + 1, TokenType::IDENTIFIER);
					last_pos = i + 1;
					state = TokenizeState::STANDARD;
				}
			}
			break;
		case TokenizeState::SINGLE_LINE_COMMENT:
			if (c == '\n' || c == '\r') {
				behavior.PushToken(last_pos, i + 1, TokenType::COMMENT);
				last_pos = i + 1;
				state = TokenizeState::STANDARD;
			}
			break;
		case TokenizeState::MULTI_LINE_COMMENT:
			if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
				i++;
				multi_line_comment_depth++;
			} else if (c == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
				i++;
				multi_line_comment_depth--;
				if (multi_line_comment_depth == 0) {
					behavior.PushToken(last_pos, i + 1, TokenType::COMMENT);
					last_pos = i + 1;
					state = TokenizeState::STANDARD;
				}
			}
			break;
		case TokenizeState::DOLLAR_QUOTED_STRING: {
			// Dollar-quoted string -- all that will get us out is a $[marker]$
			if (c != '$') {
				break;
			}
			if (i + 1 >= sql.size()) {
				// No room for the final dollar
				break;
			}
			// Skip to the next dollar symbol
			idx_t start = i + 1;
			idx_t end = start;
			while (end < sql.size() && sql[end] != '$') {
				end++;
			}
			if (end >= sql.size()) {
				// No final dollar, continue as normal
				break;
			}
			if (end - start != dollar_quote_marker.size()) {
				// Length mismatch, cannot match
				break;
			}
			if (sql.compare(start, dollar_quote_marker.size(), dollar_quote_marker) != 0) {
				// marker mismatch
				break;
			}
			// Marker found! Revert to standard state
			behavior.PushToken(last_pos, end + 1, TokenType::STRING_LITERAL);
			dollar_quote_marker = {};
			state = TokenizeState::STANDARD;
			i = end;
			last_pos = i + 1;
			break;
		}
		default:
			throw InternalException("unrecognized tokenize state");
		}
	}

	switch (state) {
	case TokenizeState::SINGLE_LINE_COMMENT:
		behavior.PushToken(last_pos, sql.size(), TokenType::COMMENT);
		return false;
	case TokenizeState::MULTI_LINE_COMMENT:
		behavior.PushToken(last_pos, sql.size(), TokenType::COMMENT, true);
		return false;
	case TokenizeState::OPERATOR:
		PushOperatorToken(behavior, last_pos, sql.size());
		return true;
	case TokenizeState::DOLLAR_QUOTED_STRING:
		behavior.PushToken(last_pos, sql.size(), TokenType::STRING_LITERAL, true);
		return false;
	default:
		break;
	}
	HandleLastToken(behavior, state, sql, last_pos);
	return true;
}

void TokenizerBehavior::OnStatementEnd(idx_t pos) {
	// Default: Do nothing
}

void TokenizerBehavior::OnLastToken(const Tokenizer &tokenizer, TokenizeState state, std::string_view last_word,
                                    idx_t last_pos) {
	if (last_word.empty()) {
		return;
	}
	if (state == TokenizeState::KEYWORD && !tokenizer.keyword_helper.IsKeyword(last_word)) {
		state = TokenizeState::STANDARD;
	}

	bool is_unterminated = Tokenizer::IsUnterminatedState(state);
	tokens.emplace_back(last_word, last_pos, Tokenizer::TokenizeStateToType(state), is_unterminated);
}

} // namespace duckdb
