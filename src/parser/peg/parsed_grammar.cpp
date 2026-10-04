#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/peg/matcher.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#ifdef PEG_PARSER_SOURCE_FILE
#include <fstream>
#else
#include "duckdb/parser/peg/inlined_grammar.hpp"
#endif

namespace duckdb {

ParsedGrammar::ParsedGrammar(ParsedGrammar &&other) noexcept
    : rules(std::move(other.rules)),
      terminal_rule_override_callbacks(std::move(other.terminal_rule_override_callbacks)) {
	string_heap.Move(other.string_heap);
}

ParsedGrammar &ParsedGrammar::operator=(ParsedGrammar &&other) noexcept {
	if (this != &other) {
		rules.clear();
		terminal_rule_override_callbacks.clear();
		string_heap.Destroy();
		string_heap.Move(other.string_heap);
		rules = std::move(other.rules);
		terminal_rule_override_callbacks = std::move(other.terminal_rule_override_callbacks);
	}
	return *this;
}

ParsedGrammar ParsedGrammar::Parse(const string &grammar) {
	PEGParser parser;
	parser.ParseRules(grammar.c_str());
	ParsedGrammar result;
	for (auto &entry : parser.rules) {
		result.AddParsedRule(ParsedGrammarRule(entry.first, std::move(entry.second)));
	}
	return result;
}

ParsedGrammar ParsedGrammar::CreateDefault() {
#ifdef PEG_PARSER_SOURCE_FILE
	std::ifstream t(PEG_PARSER_SOURCE_FILE);
	std::stringstream buffer;
	buffer << t.rdbuf();
	auto grammar_string = buffer.str();

	const char *grammar = grammar_string.c_str();
#else
	const char *grammar = const_char_ptr_cast(INLINED_PEG_GRAMMAR);
#endif
	auto result = Parse(grammar);
	PEGTransformerFactory::RegisterDefaultTransforms(result);
	return result;
}

optional_ptr<const ParsedGrammarRule> ParsedGrammar::GetRule(const string &rule_name) const {
	auto entry = rules.find(rule_name);
	if (entry == rules.end()) {
		return nullptr;
	}
	return *entry->second;
}
ParsedGrammarRule &ParsedGrammar::GetMutableRule(const string &rule_name) {
	auto entry = rules.find(rule_name);
	if (entry == rules.end()) {
		throw InvalidInputException("Grammar rule '%s' does not exist", rule_name);
	}
	return *entry->second;
}

ParsedGrammarRule ParsedGrammar::ParseSingleRule(const string &rule_definition) {
	PEGParser parser;
	parser.ParseRules(rule_definition.c_str());
	if (parser.rules.size() != 1) {
		throw InvalidInputException("Expected exactly one PEG rule definition");
	}
	auto &entry = *parser.rules.begin();
	return ParsedGrammarRule(entry.first, std::move(entry.second));
}

static void RegisterText(StringHeap &string_heap, PEGExpression &expression) {
	expression.text = string_heap.AddString(expression.text);
	for (auto &child : expression.children) {
		RegisterText(string_heap, child);
	}
}

void ParsedGrammar::RegisterStrings(PEGRule &rule) {
	string_map_t<idx_t> parameters;
	for (auto &entry : rule.parameters) {
		parameters.emplace(string_heap.AddString(entry.first), entry.second);
	}
	rule.parameters = std::move(parameters);
	RegisterText(string_heap, rule.expression);
}

void ParsedGrammar::AddParsedRule(ParsedGrammarRule rule) {
	if (GetRule(rule.name)) {
		throw InvalidInputException("Grammar rule '%s' already exists", rule.name);
	}
	RegisterStrings(rule.recipe);
	auto name = rule.name;
	rules.emplace(std::move(name), make_uniq<ParsedGrammarRule>(std::move(rule)));
}

void ParsedGrammar::AddRule(const string &rule_definition, grammar_transform_process_function_t transform_process) {
	auto rule = ParseSingleRule(rule_definition);
	rule.transform_process = std::move(transform_process);
	AddParsedRule(std::move(rule));
}

void ParsedGrammar::ReplaceRule(const string &rule_definition, grammar_transform_process_function_t transform_process) {
	auto rule = ParseSingleRule(rule_definition);
	auto entry = rules.find(rule.name);
	if (entry == rules.end()) {
		throw InvalidInputException("Grammar rule '%s' does not exist", rule.name);
	}
	RegisterStrings(rule.recipe);
	rule.transform_process = std::move(transform_process);
	entry->second = make_uniq<ParsedGrammarRule>(std::move(rule));
}

void ParsedGrammar::SetTransformProcess(const string &rule_name, grammar_transform_process_function_t transform_process,
                                        bool collapsible) {
	auto &rule = GetMutableRule(rule_name);
	rule.transform_process = std::move(transform_process);
	rule.collapsible = collapsible;
}

void ParsedGrammar::AddTerminalRuleOverride(const string &rule_name, terminal_rule_matcher_factory_t matcher_factory) {
	if (!matcher_factory) {
		throw InvalidInputException("Cannot add an empty terminal rule matcher factory for '%s'", rule_name);
	}
	terminal_rule_override_callbacks.emplace_back(
	    [rule_name, matcher_factory = std::move(matcher_factory)](const PEGKeywordHelper &keyword_helper,
	                                                              terminal_rule_overrides_t &overrides) {
		    AddTerminalRuleOverride(overrides, rule_name, matcher_factory(keyword_helper));
	    });
}

void ParsedGrammar::AddTerminalRuleOverride(terminal_rule_overrides_t &overrides, const string &rule_name,
                                            unique_ptr<Matcher> matcher) {
	if (!matcher) {
		throw InvalidInputException("Cannot add an empty terminal rule override for '%s'", rule_name);
	}
	if (overrides.count(rule_name)) {
		throw InvalidInputException("Terminal rule override for '%s' already exists", rule_name);
	}
	overrides.emplace(rule_name, std::move(matcher));
}

} // namespace duckdb
