#include "duckdb/parser/peg/tokenizer/highlight_tokenizer.hpp"

namespace duckdb {

HighlightTokenizer::HighlightTokenizer(std::string_view sql) : BaseTokenizer(sql, tokens) {
}

void HighlightTokenizer::PushToken(idx_t start, idx_t end, TokenType type, bool unterminated) {
	if (start >= end) {
		return;
	}
	auto last_token = sql.substr(start, end - start);
	tokens.emplace_back(string(last_token), start, type, unterminated);
}

void HighlightTokenizer::OnStatementEnd(idx_t pos) {
	tokens.emplace_back(";", pos, TokenType::TERMINATOR);
}
} // namespace duckdb
