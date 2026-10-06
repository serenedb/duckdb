//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/parse_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/peg/matcher_token.hpp"

namespace duckdb {
class ClientContext;
class Parser;
class SQLStatement;
class TokenIterator;

//! Iterator over the parse-facing statements of a multi-statement query.
//!
//! Usage:
//!   ParseIterator it(context, sql);
//!   while (it.Peek()) {
//!       auto stmt = it.GetStatement();
//!       // raw, just-parsed statement
//!   }
//!
//! Separator-only stretches (e.g. ";;;") are transparently skipped inside Peek — the caller
//! always sees either a real statement or a clean exhaustion.
class ParseIterator {
public:
	DUCKDB_API ParseIterator(ClientContext &context, std::string_view sql);
	DUCKDB_API ~ParseIterator();

	ParseIterator(const ParseIterator &) = delete;
	ParseIterator &operator=(const ParseIterator &) = delete;
	DUCKDB_API ParseIterator(ParseIterator &&) noexcept;
	// Not move-assignable: holds a ClientContext reference, which cannot be rebound.
	ParseIterator &operator=(ParseIterator &&) = delete;

	//! Returns true if a statement is currently available (after parsing as needed). Returns
	//! false when the iterator is exhausted. Non-const: parses on demand and buffers the result.
	DUCKDB_API bool Peek();

	//! Returns the next buffered statement and clears the buffer. Returns nullptr if no
	//! statement is buffered — the caller should call Peek() first.
	DUCKDB_API unique_ptr<SQLStatement> GetStatement();

	//! Grammar-free predicate: does another statement remain in the input? Unlike Peek() this never
	//! invokes the parser (only tokenizes / walks the token cursor, skipping separators), so it never
	//! throws a parser error and is safe as a look-ahead before the current statement has executed.
	//! Assumes the input has already been resolved by a prior Peek() (true for all current callers).
	DUCKDB_API bool HasMore();

	//! The context this iterator is bound to (used by StatementIterator to inherit it).
	DUCKDB_API ClientContext &GetClientContext();

private:
	//! Tokenize the full input once (grammar-free); no-op if already tokenized.
	void EnsureTokenized();

private:
	ClientContext &context;
	std::string_view sql;
	vector<char> stripped;
	//! Parser instance kept alive across Peek calls so its PEG matcher / transformer caches
	//! stay warm. Constructed lazily on the first Peek.
	unique_ptr<Parser> parser;
	vector<MatcherToken> tokens;
	//! Tokenized view of `sql` and its current position. Populated once on the first Peek.
	unique_ptr<TokenIterator> token_iterator;
	//! Single-statement buffer holding the result of the most recent Peek. Cleared by
	//! GetStatement.
	unique_ptr<SQLStatement> current_statement;
	//! Once Peek determines there are no more statements (cursor past end of tokens), we stay
	//! exhausted; subsequent Peek calls return false without re-invoking the parser.
	bool exhausted = false;
};

} // namespace duckdb
