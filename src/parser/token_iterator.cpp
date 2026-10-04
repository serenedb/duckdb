#include "duckdb/parser/token_iterator.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

TokenIterator::TokenIterator(vector<MatcherToken> &tokens_p) : tokens(tokens_p) {
	// A new root can receive tokens edited since an earlier match.
	for (auto &token : tokens) {
		token.ResetLiteralInfo();
	}
}

bool TokenIterator::AtEnd() const {
	auto current = Current();
	return !current || current->type == TokenType::END_OF_INPUT;
}

bool TokenIterator::HasMoreStatements() const {
	for (idx_t index = position; index < tokens.size(); index++) {
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
	if (tokens.empty()) {
		return 0;
	}
	auto &last_token = tokens.back();
	return last_token.offset + last_token.length;
}

const MatcherToken &TokenIterator::Previous() const {
	if (position == 0) {
		throw InternalException("TokenIterator has no previous token");
	}
	return GetToken(position - 1);
}

void TokenIterator::ThrowIndexOutOfRange(idx_t index) const {
	throw InternalException("Token index %llu is out of range (size %llu)", index, tokens.size());
}

void TokenIterator::ThrowAdvanceOutOfRange(idx_t count) const {
	throw InternalException("Cannot advance TokenIterator by %llu tokens from position %llu (size %llu)", count,
	                        position, tokens.size());
}

void TokenIterator::ThrowPositionOutOfRange(idx_t position_p) const {
	throw InternalException("Token position %llu is out of range (size %llu)", position_p, tokens.size());
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

vector<SimpleToken> TokenIterator::RemainingTokens() const {
	vector<SimpleToken> result;
	result.reserve(tokens.size() - position);
	for (idx_t index = position; index < tokens.size(); index++) {
		result.emplace_back(tokens[index].text, tokens[index].type);
	}
	return result;
}

string TokenIterator::ToString() const {
	string result;
	for (auto &token : tokens) {
		result += token.text + " ";
	}
	return result;
}

} // namespace duckdb
