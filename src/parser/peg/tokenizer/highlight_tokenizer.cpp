#include "duckdb/parser/peg/tokenizer/highlight_tokenizer.hpp"

namespace duckdb {

HighlightTokenizerBehavior::HighlightTokenizerBehavior(std::string_view sql, vector<MatcherToken> &tokens)
    : TokenizerBehavior(sql, tokens) {
}

void HighlightTokenizerBehavior::PushToken(idx_t start, idx_t end, TokenType type, bool unterminated) {
	if (start >= end) {
		return;
	}
	tokens.emplace_back(sql.substr(start, end - start), start, type, unterminated);
}

void HighlightTokenizerBehavior::OnStatementEnd(idx_t pos) {
	tokens.emplace_back(";", pos, TokenType::TERMINATOR);
}
} // namespace duckdb
