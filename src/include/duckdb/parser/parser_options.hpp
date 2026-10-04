//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parser_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/identifier_case_mode.hpp"
#include "duckdb/common/enums/regex_match_operator_semantics.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
struct CompiledGrammar;

struct ParserOptions {
	static constexpr idx_t DEFAULT_MAX_EXPRESSION_DEPTH = 1000;

	IdentifierCaseMode identifier_case_mode = IdentifierCaseMode::PRESERVE_CASE;
	bool integer_division = false;
	RegexMatchOperatorSemantics regex_match_operator_semantics = RegexMatchOperatorSemantics::PARTIAL;
	idx_t max_expression_depth = DEFAULT_MAX_EXPRESSION_DEPTH;
	optional_ptr<const CompiledGrammar> grammar;
};

} // namespace duckdb
