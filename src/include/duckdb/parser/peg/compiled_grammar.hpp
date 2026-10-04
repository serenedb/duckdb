#pragma once

#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"

namespace duckdb {

class ClientContext;

using compiled_rules_map_t = case_insensitive_map_t<unique_ptr<CompiledGrammarRule>>;

struct CompiledGrammar {
public:
	CompiledGrammar(MatcherAllocator &&allocator, unique_ptr<PEGKeywordHelper> &&keyword_helper,
	                unique_ptr<Tokenizer> &&tokenizer, compiled_rules_map_t &&rules, const Matcher &program_matcher,
	                const Matcher &top_level_statement_matcher);

public:
	const Matcher &ProgramMatcher() const {
		return program_matcher;
	}
	const Matcher &TopLevelStatementMatcher() const {
		return top_level_statement_matcher;
	}
	const PEGKeywordHelper &GetKeywordHelper() const {
		return *keyword_helper;
	}
	const Tokenizer &GetTokenizer() const {
		return *tokenizer;
	}
	idx_t PackratSlotCount() const {
		return allocator.PackratSlotCount();
	}
	optional_ptr<const CompiledGrammarRule> GetRule(const string &rule_name) const;

public:
	static const CompiledGrammar &Get(const ClientContext &context);
	static const CompiledGrammar &Base();
	static const CompiledGrammar &Passthrough();
	static CompiledGrammar Create();
	static CompiledGrammar Compile(ParsedGrammar &grammar);

private:
	MatcherAllocator allocator;
	unique_ptr<PEGKeywordHelper> keyword_helper;
	unique_ptr<Tokenizer> tokenizer;
	case_insensitive_map_t<unique_ptr<CompiledGrammarRule>> rules;
	const Matcher &program_matcher;
	const Matcher &top_level_statement_matcher;
};

} // namespace duckdb
