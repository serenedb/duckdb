#include "duckdb/parser/token_iterator.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

TokenIterator::TokenIterator(MatcherToken *tokens_p, idx_t token_count_p)
    : tokens(tokens_p), token_count(token_count_p) {
}

TokenIterator::TokenIterator(vector<MatcherToken> &tokens_p) : TokenIterator(tokens_p.data(), tokens_p.size()) {
	// A new root can receive tokens edited since an earlier match.
	for (auto &token : tokens_p) {
		token.ResetLiteralInfo();
	}
}

TokenIterator TokenIterator::FromTokenizer(vector<MatcherToken> &tokens_p) {
	return TokenIterator(tokens_p.data(), tokens_p.size());
}

bool TokenIterator::AtEnd() const {
	auto current = Current();
	return !current || current->type == TokenType::END_OF_INPUT;
}

bool TokenIterator::HasMoreStatements() const {
	for (idx_t index = position; index < token_count; index++) {
		auto type = tokens[index].type;
		if (type == TokenType::END_OF_INPUT) {
			return false;
		}
		if (type != TokenType::TERMINATOR) {
			return true;
		}
	}
	return false;
}

idx_t TokenIterator::EndOffset() const {
	if (token_count == 0) {
		return 0;
	}
	auto &last_token = tokens[token_count - 1];
	return last_token.offset + last_token.length;
}

const MatcherToken &TokenIterator::Previous() const {
	if (position == 0) {
		throw InternalException("TokenIterator has no previous token");
	}
	return GetToken(position - 1);
}

void TokenIterator::ThrowIndexOutOfRange(idx_t index) const {
	throw InternalException("Token index %llu is out of range (size %llu)", index, token_count);
}

void TokenIterator::ThrowAdvanceOutOfRange(idx_t count) const {
	throw InternalException("Cannot advance TokenIterator by %llu tokens from position %llu (size %llu)", count,
	                        position, token_count);
}

void TokenIterator::ThrowPositionOutOfRange(idx_t position_p) const {
	throw InternalException("Token position %llu is out of range (size %llu)", position_p, token_count);
}

void TokenIterator::ThrowForeignIterator() const {
	throw InternalException("Cannot set TokenIterator position from a different token collection");
}

void TokenIterator::SetPreviousTokenType(TokenType type) {
	if (position == 0) {
		throw InternalException("TokenIterator has no previous token to annotate");
	}
	tokens[position - 1].type = type;
}

string TokenIterator::ToString() const {
	string result;
	for (idx_t index = 0; index < token_count; index++) {
		result += tokens[index].text;
		result += ' ';
	}
	return result;
}

} // namespace duckdb
