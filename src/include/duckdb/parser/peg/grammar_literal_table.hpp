//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/grammar_literal_table.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/literal_info.hpp"

namespace duckdb {

class ParsedGrammar;

//! Immutable after construction, including literals only present in keyword-category rules.
class GrammarLiteralTable {
public:
	DUCKDB_API GrammarLiteralTable(const ParsedGrammar &grammar, const case_insensitive_map_t<LiteralInfo> &keywords);
	GrammarLiteralTable(const GrammarLiteralTable &) = delete;
	GrammarLiteralTable &operator=(const GrammarLiteralTable &) = delete;

	uint32_t CacheId() const {
		return cache_id;
	}

	LiteralInfo Lookup(std::string_view text) const {
		if (text.size() == 1 && static_cast<uint8_t>(text[0]) < SINGLE_BYTE_LITERALS) {
			return single_byte_literals[static_cast<uint8_t>(StringUtil::CharacterToLower(text[0]))];
		}
		auto entry = literals.find(text);
		return entry == literals.end() ? LiteralInfo() : entry->second;
	}

private:
	static constexpr idx_t SINGLE_BYTE_LITERALS = 128;

	void Register(const string &text, keyword_categories_t categories = keyword_categories_t());

private:
	const uint32_t cache_id;
	case_insensitive_map_t<LiteralInfo> literals;
	array<LiteralInfo, SINGLE_BYTE_LITERALS> single_byte_literals;
};

} // namespace duckdb
