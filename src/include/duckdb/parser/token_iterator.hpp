//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/token_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/peg/matcher_token.hpp"

namespace duckdb {
struct SimpleToken;

class TokenIterator {
public:
	DUCKDB_API explicit TokenIterator(vector<MatcherToken> &tokens);
	TokenIterator(const TokenIterator &other) = default;
	TokenIterator &operator=(const TokenIterator &) = delete;

	DUCKDB_API bool AtEnd() const;
	DUCKDB_API bool HasMoreStatements() const;
	idx_t Position() const {
		return position;
	}
	idx_t Size() const {
		return tokens.size();
	}
	DUCKDB_API idx_t EndOffset() const;

	optional_ptr<const MatcherToken> Current() const {
		if (position >= tokens.size()) {
			return nullptr;
		}
		return tokens[position];
	}
	LiteralInfo CurrentLiteralInfo(const GrammarLiteralTable &table) {
		if (position >= tokens.size()) {
			return LiteralInfo();
		}
		return tokens[position].GetLiteralInfo(table);
	}
	DUCKDB_API const MatcherToken &Previous() const;
	const MatcherToken &GetToken(idx_t index) const {
		if (index >= tokens.size()) {
			ThrowIndexOutOfRange(index);
		}
		return tokens[index];
	}

	void Advance(idx_t count = 1) {
		if (count > tokens.size() - position) {
			ThrowAdvanceOutOfRange(count);
		}
		position += count;
	}
	void SetPosition(idx_t position_p) {
		if (position_p > tokens.size()) {
			ThrowPositionOutOfRange(position_p);
		}
		position = position_p;
	}
	void SetPosition(const TokenIterator &other) {
		if (&tokens != &other.tokens) {
			ThrowForeignIterator();
		}
		position = other.position;
	}
	DUCKDB_API void SetPreviousTokenType(TokenType type);

	DUCKDB_API vector<SimpleToken> RemainingTokens() const;
	DUCKDB_API string ToString() const;

private:
	[[noreturn]] DUCKDB_API void ThrowIndexOutOfRange(idx_t index) const;
	[[noreturn]] DUCKDB_API void ThrowAdvanceOutOfRange(idx_t count) const;
	[[noreturn]] DUCKDB_API void ThrowPositionOutOfRange(idx_t position_p) const;
	[[noreturn]] DUCKDB_API void ThrowForeignIterator() const;

private:
	vector<MatcherToken> &tokens;
	idx_t position = 0;
};

} // namespace duckdb
