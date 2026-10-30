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
#include "duckdb/common/stack_depth.hpp"

namespace duckdb {
class ClientContext;
struct CompiledGrammar;

struct ParserOptions {
private:
	ParserOptions() {
	}
	friend class ClientContext;

public:
	//! Explicit configuration for parsing without a client context.
	DUCKDB_API static ParserOptions Builtin();

public:
	IdentifierCaseMode identifier_case_mode = IdentifierCaseMode::PRESERVE_CASE;
	bool integer_division = false;
	RegexMatchOperatorSemantics regex_match_operator_semantics = RegexMatchOperatorSemantics::PARTIAL;
	idx_t max_expression_depth = StackDepth::NestingLimit();
	optional_ptr<const CompiledGrammar> grammar;
};

} // namespace duckdb
