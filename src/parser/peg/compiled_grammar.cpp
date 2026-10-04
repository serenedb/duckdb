#include "duckdb/parser/peg/compiled_grammar.hpp"

#include "duckdb/parser/peg/passthrough_dialect.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/peg/keyword_helper/parsed_grammar_keyword_helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

CompiledGrammar::CompiledGrammar(MatcherAllocator &&allocator_p, unique_ptr<PEGKeywordHelper> &&keyword_helper_p,
                                 unique_ptr<Tokenizer> &&tokenizer_p, compiled_rules_map_t &&rules_p,
                                 const Matcher &program_matcher, const Matcher &top_level_statement_matcher)
    : allocator(std::move(allocator_p)), keyword_helper(std::move(keyword_helper_p)), tokenizer(std::move(tokenizer_p)),
      rules(std::move(rules_p)), program_matcher(program_matcher),
      top_level_statement_matcher(top_level_statement_matcher) {
}

const CompiledGrammar &CompiledGrammar::Get(const ClientContext &context) {
	return context.IsConnected() ? Passthrough() : Base();
}

static void ValidateParsedGrammarRoots(const ParsedGrammar &grammar) {
	if (!grammar.GetRule("Program")) {
		throw InvalidInputException("Grammar is missing required root rule 'Program'");
	}
	if (!grammar.GetRule("TopLevelStatement")) {
		throw InvalidInputException("Grammar is missing required root rule 'TopLevelStatement'");
	}
}

static void CheckReference(const ParsedGrammar &grammar, const ParsedGrammarRule &parsed_rule,
                           const PEGExpression &expression) {
	do {
		if (expression.type != PEGExpression::Type::REFERENCE &&
		    expression.type != PEGExpression::Type::FUNCTION_CALL) {
			break;
		}
		if (expression.type == PEGExpression::Type::REFERENCE && parsed_rule.recipe.parameters.count(expression.text)) {
			break;
		}
		if (StringUtil::CIEquals(expression.text.GetString(), "EndOfInput")) {
			break;
		}
		if (!grammar.GetRule(expression.text.GetString())) {
			throw InvalidInputException("Grammar rule '%s' references missing rule '%s'", parsed_rule.name,
			                            expression.text.GetString());
		}
	} while (false);
	for (auto &child : expression.children) {
		CheckReference(grammar, parsed_rule, child);
	}
}

terminal_rule_overrides_t ParsedGrammar::BuildTerminalRuleOverrides(const PEGKeywordHelper &keyword_helper) const {
	terminal_rule_overrides_t overrides;
	//===--------------------------------------------------------------------===//
	// START GENERATED RULE OVERRIDES
	//===--------------------------------------------------------------------===//
	AddTerminalRuleOverride(overrides, "Identifier",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_VARIABLE, keyword_helper));
	AddTerminalRuleOverride(overrides, "ReservedIdentifier",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_VARIABLE, keyword_helper));
	AddTerminalRuleOverride(overrides, "CatalogName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_CATALOG_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "SchemaName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_SCHEMA_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "ReservedSchemaName",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_SCHEMA_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "TableName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_TABLE_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "ReservedTableName",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_TABLE_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "ColumnName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_COLUMN_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "ReservedColumnName",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_COLUMN_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "IndexName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_VARIABLE, keyword_helper));
	AddTerminalRuleOverride(overrides, "ReservedIndexName",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_VARIABLE, keyword_helper));
	AddTerminalRuleOverride(overrides, "SequenceName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_VARIABLE, keyword_helper));
	AddTerminalRuleOverride(
	    overrides, "FunctionName",
	    make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_SCALAR_FUNCTION_NAME, keyword_helper));
	AddTerminalRuleOverride(
	    overrides, "ReservedFunctionName",
	    make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_SCALAR_FUNCTION_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "ReservedKeyword",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_VARIABLE, keyword_helper));
	AddTerminalRuleOverride(overrides, "TableFunctionName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_TABLE_FUNCTION_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "TypeName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_TYPE_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "ReservedTypeName",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_TYPE_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "PragmaName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_PRAGMA_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "SettingName",
	                        make_uniq<IdentifierMatcher>(SuggestionState::SUGGEST_SETTING_NAME, keyword_helper));
	AddTerminalRuleOverride(overrides, "CopyOptionName",
	                        make_uniq<ReservedIdentifierMatcher>(SuggestionState::SUGGEST_VARIABLE, keyword_helper));
	AddTerminalRuleOverride(overrides, "NumberLiteral", make_uniq<NumberLiteralMatcher>());
	AddTerminalRuleOverride(overrides, "StringLiteral", make_uniq<StringLiteralMatcher>());
	AddTerminalRuleOverride(overrides, "OperatorLiteral", make_uniq<OperatorMatcher>());
	AddTerminalRuleOverride(overrides, "AnyOperatorLiteral",
	                        make_uniq<OperatorMatcher>(OperatorMatcherMode::ALL_OPERATORS));
	//===--------------------------------------------------------------------===//
	// END GENERATED RULE OVERRIDES
	//===--------------------------------------------------------------------===//

	AddTerminalRuleOverride(overrides, "EndOfInput", make_uniq<EndOfInputMatcher>());
	for (auto &callback : terminal_rule_override_callbacks) {
		callback(keyword_helper, overrides);
	}
	return overrides;
}

unique_ptr<CompiledGrammar> CompiledGrammar::Compile(ParsedGrammar &grammar) {
	ValidateParsedGrammarRoots(grammar);
	for (auto &entry : grammar.rules) {
		auto &parsed_rule = *entry.second;
		auto &expression = entry.second->recipe.expression;
		CheckReference(grammar, parsed_rule, expression);
	}

	auto keyword_helper = make_uniq<ParsedGrammarKeywordHelper>(grammar);
	auto tokenizer = make_uniq<Tokenizer>(*keyword_helper);
	compiled_rules_map_t rules;
	for (auto &entry : grammar.rules) {
		auto &rule = *entry.second;
		rules.emplace(rule.name, make_uniq<CompiledGrammarRule>(rule.name, rule.transform_process, rule.collapsible));
	}

	MatcherAllocator allocator;
	auto terminal_rule_overrides = grammar.BuildTerminalRuleOverrides(*keyword_helper);
	MatcherFactory factory(allocator, grammar, rules, *keyword_helper, std::move(terminal_rule_overrides));

	auto &program_matcher = factory.CreateRootMatcher("Program");
	auto &top_level_statement_matcher = factory.GetMatcher("TopLevelStatement");
	allocator.ComputeFirstSets(keyword_helper->GetLiteralTable());

	return make_uniq<CompiledGrammar>(std::move(allocator), std::move(keyword_helper), std::move(tokenizer),
	                                  std::move(rules), program_matcher, top_level_statement_matcher);
}

unique_ptr<CompiledGrammar> CompiledGrammar::Create() {
	auto grammar = ParsedGrammar::CreateDefault();
	return Compile(grammar);
}

const CompiledGrammar &CompiledGrammar::Base() {
	static const unique_ptr<CompiledGrammar> grammar = Create();
	return *grammar;
}

const CompiledGrammar &CompiledGrammar::Passthrough() {
	static const unique_ptr<CompiledGrammar> grammar = [] {
		auto parsed_grammar = ParsedGrammar::CreateDefault();
		ApplyPassthroughDialect(parsed_grammar);
		return Compile(parsed_grammar);
	}();
	return *grammar;
}

optional_ptr<const CompiledGrammarRule> CompiledGrammar::GetRule(const string &rule_name) const {
	auto entry = rules.find(rule_name);
	if (entry == rules.end()) {
		return nullptr;
	}
	return *entry->second;
}

} // namespace duckdb
