#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/main/database.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/token_iterator.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/tokenizer/parser_tokenizer.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

ParseIterator::ParseIterator(ClientContext &context_p, const string &sql_p)
    : context(context_p), sql(Parser::NormalizeSQLString(sql_p)) {
}

ParseIterator::~ParseIterator() = default;

ParseIterator::ParseIterator(ParseIterator &&) noexcept = default;

ClientContext &ParseIterator::GetClientContext() {
	return context;
}

bool ParseIterator::Peek() {
	auto &client_context = context;
	// Already buffered from a prior Peek — just report it.
	if (current_statement) {
		return true;
	}
	if (exhausted) {
		return false;
	}
	if (!parser) {
		parser = make_uniq<Parser>(client_context.GetParserOptions());
	}
	EnsureTokenized();
	// Walk the token cursor through the cached `tokens`, calling Parser::ParseTopLevelStatement
	// repeatedly. A nullptr return with cursor advanced means a separator-only TopLevelStatement
	// (e.g. between statements or trailing ';'s); we loop past it. A nullptr return with cursor
	// at end means the input is exhausted.
	while (true) {
		if (token_iterator->AtEnd()) {
			exhausted = true;
			return false;
		}
		auto stmt = parser->ParseTopLevelStatement(*token_iterator);
		if (stmt) {
			// ParseTopLevelStatement doesn't populate stmt->query (it operates on tokens, not the
			// source string). Mirror Parser::ParseQuery's per-statement post-processing: extend from
			// the statement's start to the next statement's start (or end of input) so the trailing
			// `;` and inter-statement whitespace end up inside stmt->query — downstream consumers
			// (logging, error reporting, EXPLAIN) rely on that shape.
			idx_t stmt_loc = stmt->stmt_location.offset;
			idx_t end_loc = sql.size();
			if (auto current = token_iterator->Current()) {
				if (current->type != TokenType::END_OF_INPUT) {
					end_loc = current->offset;
				}
			}
			stmt->query = sql.substr(stmt_loc, end_loc - stmt_loc);
			stmt->stmt_location = QueryLocation(0, stmt->query.size());
			if (stmt->type == StatementType::CREATE_STATEMENT) {
				auto &create = stmt->Cast<CreateStatement>();
				create.info->sql = stmt->query;
			}
			current_statement = std::move(stmt);
			return true;
		}
		if (token_iterator->AtEnd()) {
			exhausted = true;
			return false;
		}
		// separator-only TLS in the middle of the input — loop and try the next.
	}
}

void ParseIterator::EnsureTokenized() {
	if (!token_iterator) {
		// Tokenize the full input once. Subsequent Peek/HasMore calls walk through the iterator;
		// we never re-tokenize. Tokenization is grammar-free.
		tokens = make_uniq<vector<MatcherToken>>();
		ParserTokenizerBehavior behavior(sql, *tokens);
		CompiledGrammar::Get(context).GetTokenizer().TokenizeInput(behavior);
		token_iterator = make_uniq<TokenIterator>(*tokens);
	}
}

bool ParseIterator::HasMore() {
	// A statement is already parsed and buffered by a prior Peek.
	if (current_statement) {
		return true;
	}
	if (exhausted) {
		return false;
	}
	EnsureTokenized();
	TokenIterator lookahead(*token_iterator);
	return lookahead.HasMoreStatements();
}

unique_ptr<SQLStatement> ParseIterator::GetStatement() {
	if (!current_statement) {
		return nullptr;
	}
	return std::move(current_statement);
}

} // namespace duckdb
