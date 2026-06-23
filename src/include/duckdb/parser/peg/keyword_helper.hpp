#pragma once

#include "duckdb/parser/simplified_token.hpp"

#include <string_view>

namespace duckdb {
enum class PEGKeywordCategory : uint8_t {
	KEYWORD_NONE,
	KEYWORD_UNRESERVED,
	KEYWORD_RESERVED,
	KEYWORD_TYPE_FUNC,
	KEYWORD_COL_NAME,
	KEYWORD_TYPE_NAME
};

// Keyword classification. The five keyword categories are case-insensitive
// membership sets (case_insensitive_set_view_t over the keyword literals); the
// lookups are defined in the generated keyword_map.cpp. Stateless free functions
// -- no instance to hold.
namespace peg {
bool KeywordCategoryType(std::string_view text, PEGKeywordCategory type);
bool IsKeyword(std::string_view text);
vector<ParserKeyword> KeywordList();
} // namespace peg
} // namespace duckdb
