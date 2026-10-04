#include "catch.hpp"
#include <type_traits>
#include "test_helpers.hpp"

#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/peg/keyword_helper/default_keyword_maps.hpp"
#include "duckdb/parser/peg/matcher/identifier_matcher.hpp"
#include "duckdb/parser/peg/matcher/keyword_matcher.hpp"
#include "duckdb/parser/peg/matcher/list_matcher.hpp"
#include "duckdb/parser/peg/matcher_stack.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

using namespace duckdb;

struct LiteralChoiceTestResult {
	bool success;
	idx_t position;
	idx_t max_position;
	string tree;
};

static compiled_rules_map_t CompileTestProgramRule(const ParsedGrammar &grammar) {
	compiled_rules_map_t rules;
	auto rule = grammar.GetRule("Program");
	if (!rule) {
		throw InternalException("Test grammar is missing the Program rule");
	}
	rules.emplace(rule->name, make_uniq<CompiledGrammarRule>(rule->name, rule->transform_process, rule->collapsible));
	return rules;
}

static vector<MatcherSuggestion> GetLiteralChoiceSuggestions(const Matcher &matcher) {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_position = 0;
	MatchContext context(suggestions, allocator, max_position);
	MatchState state(iterator, context);
	matcher.AddSuggestion(state);
	return suggestions;
}

TEST_CASE("Literal choice dispatch retains autocomplete metadata", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	auto grammar = ParsedGrammar::Parse("Program <- 'TABLE' / '(' / '.' / 'table'");
	auto rules = CompileTestProgramRule(grammar);
	MatcherAllocator allocator;
	MatcherFactory factory(allocator, grammar, rules, compiled.GetKeywordHelper(), {});
	auto &root = factory.CreateRootMatcher("Program").Cast<ListMatcher>();
	auto &choice = root.matchers[0].get().Cast<ChoiceMatcher>();
	vector<reference<Matcher>> children = choice.matchers;
	ChoiceMatcher sequential(std::move(children));
	auto actual = GetLiteralChoiceSuggestions(choice);
	auto expected = GetLiteralChoiceSuggestions(sequential);
	REQUIRE(actual.size() == 3);
	REQUIRE(actual.size() == expected.size());
	for (idx_t i = 0; i < actual.size(); i++) {
		REQUIRE(actual[i].type == expected[i].type);
		REQUIRE(actual[i].keyword.candidate == expected[i].keyword.candidate);
		REQUIRE(actual[i].keyword.score_bonus == expected[i].keyword.score_bonus);
		REQUIRE(actual[i].keyword.extra_char == expected[i].keyword.extra_char);
		REQUIRE(actual[i].keyword.candidate_type == expected[i].keyword.candidate_type);
	}
}

static LiteralChoiceTestResult MatchLiteralChoiceTest(const Matcher &matcher, const string &text, MatchMode mode) {
	vector<MatcherToken> tokens;
	if (!text.empty()) {
		tokens.emplace_back(text, 0, TokenType::KEYWORD);
	}
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	ParserPackratCache packrat(0, 0);
	idx_t max_position = 0;
	MatchContext context(suggestions, allocator, max_position, mode, IdentifierCaseMode::PRESERVE_CASE, &packrat);
	MatchState state(iterator, context);
	auto result = matcher.MatchParseResult(state);
	return {result.IsSuccess(), state.token_iterator.Position(), max_position,
	        result.HasParseResult() ? result.GetParseResult()->ToString() : string()};
}

TEST_CASE("Literal choice dispatch preserves ordered choice results", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	auto grammar = ParsedGrammar::Parse("Program <- 'SELECT' / 'FROM' / 'select' / 'WHERE' / '('");
	auto rules = CompileTestProgramRule(grammar);
	MatcherAllocator allocator;
	MatcherFactory factory(allocator, grammar, rules, compiled.GetKeywordHelper(), {});
	auto &root = factory.CreateRootMatcher("Program").Cast<ListMatcher>();
	auto &choice = root.matchers[0].get().Cast<ChoiceMatcher>();
	vector<reference<Matcher>> children = choice.matchers;
	ChoiceMatcher sequential(std::move(children));
	auto &table = compiled.GetKeywordHelper().GetLiteralTable();
	REQUIRE(choice.matchers[0].get().Cast<KeywordMatcher>().GetDispatchLiteral(table).IsValid());
	for (auto &text : vector<string> {"WHERE", "unknown_literal"}) {
		vector<MatcherToken> tokens {MatcherToken(text, 0, TokenType::KEYWORD)};
		TokenIterator iterator(tokens);
		vector<MatcherSuggestion> suggestions;
		ParseResultAllocator parse_results;
		idx_t max_position = 0;
		MatchContext context(suggestions, parse_results, max_position);
		MatchState state(iterator, context);
		auto process = choice.StartMatch(state);
		auto step = process->Resume(nullopt);
		if (text == "WHERE") {
			REQUIRE(step.HasChild());
			REQUIRE(&step.GetChild().matcher == &choice.matchers[3].get());
		} else {
			REQUIRE_FALSE(step.HasChild());
			REQUIRE_FALSE(step.GetResult().IsSuccess());
		}
	}
	for (auto mode : {MatchMode::BUILD_PARSE_RESULT, MatchMode::RECOGNIZE_ONLY}) {
		for (auto &text : vector<string> {"select", "FROM", "where", "(", "unknown_literal", ""}) {
			auto actual = MatchLiteralChoiceTest(choice, text, mode);
			auto expected = MatchLiteralChoiceTest(sequential, text, mode);
			REQUIRE(actual.success == expected.success);
			REQUIRE(actual.position == expected.position);
			REQUIRE(actual.max_position == expected.max_position);
			REQUIRE(actual.tree == expected.tree);
		}
	}
}

class DispatchOverrideKeywordMatcher final : public KeywordMatcher {
public:
	DispatchOverrideKeywordMatcher(const string &keyword, const KeywordInfo &info, const PEGKeywordHelper &helper,
	                               idx_t &calls_p)
	    : KeywordMatcher(keyword, info, helper), calls(calls_p), accepts_from(keyword == "SELECT") {
	}

	MatcherResult MatchAtomic(MatchState &state) const override {
		calls++;
		auto token = state.token_iterator.Current();
		if (accepts_from && token && StringUtil::CIEquals(token->text, "FROM")) {
			state.token_iterator.Advance();
			return MatcherResult::Success();
		}
		return KeywordMatcher::MatchAtomic(state);
	}

private:
	idx_t &calls;
	bool accepts_from;
};

class DispatchOverrideMatcherFactory final : public MatcherFactory {
public:
	DispatchOverrideMatcherFactory(MatcherAllocator &allocator, const ParsedGrammar &grammar,
	                               const compiled_rules_map_t &rules, const PEGKeywordHelper &helper_p, idx_t &calls_p)
	    : MatcherFactory(allocator, grammar, rules, helper_p, {}), helper(helper_p), calls(calls_p) {
	}

private:
	unique_ptr<KeywordMatcher> CreateKeyword(const string &keyword, const KeywordInfo &info) const override {
		return make_uniq<DispatchOverrideKeywordMatcher>(keyword, info, helper, calls);
	}

	const PEGKeywordHelper &helper;
	idx_t &calls;
};

TEST_CASE("Literal dispatch does not assume custom keyword matcher semantics", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	auto grammar = ParsedGrammar::Parse("Program <- 'SELECT' / 'FROM'");
	auto rules = CompileTestProgramRule(grammar);
	MatcherAllocator allocator;
	idx_t calls = 0;
	DispatchOverrideMatcherFactory factory(allocator, grammar, rules, compiled.GetKeywordHelper(), calls);
	auto &root = factory.CreateRootMatcher("Program");
	auto result = MatchLiteralChoiceTest(root, "FROM", MatchMode::RECOGNIZE_ONLY);
	REQUIRE(result.success);
	REQUIRE(result.position == 1);
	REQUIRE(calls == 1);
}

TEST_CASE("Literal dispatch leaves mixed and unregistered alternatives unchanged", "[api][grammar_extension]") {
	auto compiled = CompiledGrammar::Create();
	for (auto &definition : vector<string> {"Program <- 'SELECT' / ('FROM' 'WHERE')",
	                                        "Program <- 'SELECT' / 'unregistered_dispatch_word'"}) {
		auto grammar = ParsedGrammar::Parse(definition);
		auto rules = CompileTestProgramRule(grammar);
		MatcherAllocator allocator;
		MatcherFactory factory(allocator, grammar, rules, compiled.GetKeywordHelper(), {});
		auto &root = factory.CreateRootMatcher("Program").Cast<ListMatcher>();
		auto &choice = root.matchers[0].get().Cast<ChoiceMatcher>();
		vector<reference<Matcher>> children = choice.matchers;
		ChoiceMatcher sequential(std::move(children));
		for (auto &text : vector<string> {"SELECT", "FROM", "unregistered_dispatch_word", "missing"}) {
			auto actual = MatchLiteralChoiceTest(choice, text, MatchMode::BUILD_PARSE_RESULT);
			auto expected = MatchLiteralChoiceTest(sequential, text, MatchMode::BUILD_PARSE_RESULT);
			REQUIRE(actual.success == expected.success);
			REQUIRE(actual.position == expected.position);
			REQUIRE(actual.max_position == expected.max_position);
			REQUIRE(actual.tree == expected.tree);
		}
	}
}

TEST_CASE("Keyword category masks require explicit conversions and valid IDs", "[api][grammar_extension]") {
	static_assert(!std::is_convertible<uint8_t, keyword_categories_t>::value, "Category masks must be explicit");
	static_assert(!std::is_convertible<keyword_categories_t, uint8_t>::value, "Category masks must stay opaque");
	static_assert(!std::is_assignable<keyword_categories_t &, uint8_t>::value, "Raw masks must be explicit");
	constexpr auto first = keyword_categories_t::CreateCategory(0);
	constexpr auto last = keyword_categories_t::CreateCategory(keyword_categories_t::MAX_CATEGORY_ID - 1);
	static_assert(sizeof(keyword_categories_t) == sizeof(uint8_t), "Category masks must remain compact");
	REQUIRE(static_cast<uint8_t>(first) == 1);
	REQUIRE(static_cast<uint8_t>(last) == 128);
	REQUIRE(keyword_categories_t(static_cast<uint8_t>(first)) == first);
	REQUIRE((first & last) == keyword_categories_t());
	REQUIRE(((first | last) & first) == first);
	REQUIRE(first != keyword_categories_t());
	REQUIRE_THROWS_AS(keyword_categories_t::CreateCategory(keyword_categories_t::MAX_CATEGORY_ID),
	                  InvalidInputException);
	REQUIRE_THROWS_AS(keyword_categories_t::CreateCategory(256), InvalidInputException);
}

TEST_CASE("Literal IDs and opaque flags are independent", "[api][grammar_extension]") {
	REQUIRE(sizeof(LiteralInfo) == sizeof(uint32_t));
	REQUIRE(sizeof(LiteralInfo().LiteralId()) == sizeof(uint16_t));
	LiteralInfo missing;
	REQUIRE(missing.LiteralId() == 0);
	REQUIRE_FALSE(missing.IsKeyword());
	REQUIRE_FALSE(missing.HasAnyFlags(~keyword_categories_t()));
	LiteralInfo original(LiteralInfo::MAX_LITERAL_ID);
	REQUIRE(original.LiteralId() == 65535);
	REQUIRE_FALSE(original.IsKeyword());
	LiteralInfo accumulated(LiteralInfo::MAX_LITERAL_ID);
	keyword_categories_t expected_flags;
	for (idx_t id = 0; id < keyword_categories_t::MAX_CATEGORY_ID; id++) {
		auto flag = keyword_categories_t::CreateCategory(id);
		LiteralInfo literal(LiteralInfo::MAX_LITERAL_ID, flag);
		REQUIRE(literal.HasAnyFlags(flag));
		REQUIRE_FALSE(literal.HasAnyFlags(~flag));
		REQUIRE_FALSE(literal.HasAnyFlags(keyword_categories_t()));
		REQUIRE(literal.IsKeyword());
		REQUIRE(literal.LiteralId() == original.LiteralId());
		REQUIRE_FALSE(literal == original);
		LiteralInfo copy(literal);
		REQUIRE(copy == literal);
		LiteralInfo unassigned;
		unassigned.AddCategories(flag);
		REQUIRE(unassigned.LiteralId() == 0);
		REQUIRE(unassigned.IsKeyword());
		REQUIRE(unassigned.CategoryFlags() == flag);
		accumulated.AddCategories(flag);
		expected_flags |= flag;
		REQUIRE(accumulated.LiteralId() == LiteralInfo::MAX_LITERAL_ID);
		REQUIRE(accumulated.CategoryFlags() == expected_flags);
		LiteralInfo before(accumulated);
		accumulated.AddCategories(flag);
		accumulated.AddCategories(keyword_categories_t());
		REQUIRE(accumulated == before);
	}
}

TEST_CASE("Default keyword categories decode independently of literal IDs", "[api][grammar_extension]") {
	DefaultKeywordMaps maps;
	vector<KeywordCategory> expected;
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(LiteralInfo()).empty());
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(LiteralInfo(LiteralInfo::MAX_LITERAL_ID)).empty());
	vector<pair<KeywordCategory, reference<case_insensitive_set_t>>> categories {
	    {KeywordCategory::KEYWORD_RESERVED, maps.reserved_keyword_map},
	    {KeywordCategory::KEYWORD_UNRESERVED, maps.unreserved_keyword_map},
	    {KeywordCategory::KEYWORD_TYPE_FUNC, maps.typefunc_keyword_map},
	    {KeywordCategory::KEYWORD_COL_NAME, maps.colname_keyword_map},
	    {KeywordCategory::KEYWORD_TYPE_NAME, maps.typename_keyword_map}};
	for (auto &category : categories) {
		category.second.get().insert("overlapping");
		expected.push_back(category.first);
		auto single_word = "category_" + to_string(expected.size());
		category.second.get().insert(single_word);
		REQUIRE(DefaultKeywordMaps::GetKeywordCategories(maps.LookupKeyword(single_word)) ==
		        vector<KeywordCategory> {category.first});
		for (auto id : {uint16_t(0), uint16_t(1), LiteralInfo::MAX_LITERAL_ID}) {
			auto info = maps.LookupKeyword("OVERLAPPING", id);
			REQUIRE(info.LiteralId() == id);
			REQUIRE(DefaultKeywordMaps::GetKeywordCategories(info) == expected);
		}
	}
}

TEST_CASE("Grammar literal IDs reject overflow", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- '('");
	case_insensitive_map_t<LiteralInfo> keywords;
	const auto flags = keyword_categories_t::CreateCategory(6) | keyword_categories_t::CreateCategory(7);
	for (idx_t i = 1; i < LiteralInfo::MAX_LITERAL_ID; i++) {
		keywords.emplace("literal_limit_" + to_string(i), LiteralInfo(0, flags));
	}
	GrammarLiteralTable table(grammar, keywords);
	idx_t max_id = table.Lookup("(").LiteralId();
	for (auto &entry : keywords) {
		auto info = table.Lookup(entry.first);
		REQUIRE(info.LiteralId() != 0);
		REQUIRE(info.HasAnyFlags(keyword_categories_t::CreateCategory(6)));
		REQUIRE(info.HasAnyFlags(keyword_categories_t::CreateCategory(7)));
		max_id = MaxValue<idx_t>(max_id, info.LiteralId());
	}
	REQUIRE(max_id == LiteralInfo::MAX_LITERAL_ID);
	keywords.emplace("one_literal_too_many", LiteralInfo());
	REQUIRE_THROWS_WITH(GrammarLiteralTable(grammar, keywords),
	                    Catch::Matchers::Contains("Grammar has too many distinct literals"));
}

TEST_CASE("Grammar literal IDs include category-only words and overlapping categories", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- 'SELECT' / 'select' / '('");
	DefaultKeywordMaps categories;
	categories.reserved_keyword_map.insert("SELECT");
	categories.typefunc_keyword_map.insert("category_only");
	categories.typename_keyword_map.insert("CATEGORY_ONLY");
	GrammarLiteralTable table(grammar, categories.ToLiteralMap());
	REQUIRE(sizeof(LiteralInfo) == sizeof(uint32_t));
	REQUIRE(table.Lookup("select") == table.Lookup("SELECT"));
	REQUIRE(table.Lookup("SELECT").LiteralId() != 0);
	REQUIRE(table.Lookup("SELECT").IsKeyword());
	REQUIRE(table.Lookup("(").LiteralId() != 0);
	REQUIRE_FALSE(table.Lookup("(").IsKeyword());
	REQUIRE(table.Lookup("category_only").LiteralId() != 0);
	vector<KeywordCategory> expected {KeywordCategory::KEYWORD_TYPE_FUNC, KeywordCategory::KEYWORD_TYPE_NAME};
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("category_only")) == expected);
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("SELECT")) ==
	        vector<KeywordCategory> {KeywordCategory::KEYWORD_RESERVED});
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("(")).empty());
	REQUIRE(DefaultKeywordMaps::GetKeywordCategories(table.Lookup("missing")).empty());
	REQUIRE(table.Lookup("missing").LiteralId() == 0);
	REQUIRE_FALSE(table.Lookup("missing").IsKeyword());
}

TEST_CASE("Grammar literal tables preserve dialect-defined flags", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- 'SHARED' / 'shared' / 'plain'");
	const keyword_categories_t first_flag = keyword_categories_t::CreateCategory(6);
	const keyword_categories_t second_flag = keyword_categories_t::CreateCategory(7);
	case_insensitive_map_t<LiteralInfo> keywords;
	keywords.emplace("shared", LiteralInfo(0, first_flag | second_flag));
	keywords.emplace("category_only", LiteralInfo(0, second_flag));
	GrammarLiteralTable table(grammar, keywords);

	auto shared = table.Lookup("SHARED");
	auto category_only = table.Lookup("CATEGORY_ONLY");
	auto plain = table.Lookup("plain");
	REQUIRE(shared == table.Lookup("shared"));
	REQUIRE(shared.LiteralId() != 0);
	REQUIRE(category_only.LiteralId() != 0);
	REQUIRE(plain.LiteralId() != 0);
	REQUIRE(shared.LiteralId() != category_only.LiteralId());
	REQUIRE(shared.LiteralId() != plain.LiteralId());
	REQUIRE(category_only.LiteralId() != plain.LiteralId());
	REQUIRE(shared.HasAnyFlags(first_flag));
	REQUIRE(shared.HasAnyFlags(second_flag));
	REQUIRE(category_only.HasAnyFlags(second_flag));
	REQUIRE_FALSE(category_only.HasAnyFlags(first_flag));
	REQUIRE_FALSE(plain.IsKeyword());
	REQUIRE_FALSE(plain.HasAnyFlags(first_flag | second_flag));
	REQUIRE(table.Lookup("missing") == LiteralInfo());
}

TEST_CASE("Token literal caches follow grammar identity and token edits", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::Parse("LiteralTest <- 'SELECT'");
	DefaultKeywordMaps first_categories;
	first_categories.reserved_keyword_map.insert("SELECT");
	DefaultKeywordMaps second_categories;
	second_categories.unreserved_keyword_map.insert("SELECT");
	second_categories.typename_keyword_map.insert("extension_word");
	GrammarLiteralTable first(grammar, first_categories.ToLiteralMap());
	optional<GrammarLiteralTable> second;
	second.emplace(grammar, second_categories.ToLiteralMap());
	vector<MatcherToken> tokens {MatcherToken("select", 0, TokenType::KEYWORD),
	                             MatcherToken("extension_word", 7, TokenType::IDENTIFIER)};
	TokenIterator iterator(tokens);
	REQUIRE(iterator.CurrentLiteralInfo(first) == first.Lookup("SELECT"));
	TokenIterator branch(iterator);
	branch.Advance();
	branch.SetPreviousTokenType(TokenType::COLUMN_NAME);
	REQUIRE(iterator.CurrentLiteralInfo(first) == first.Lookup("SELECT"));
	REQUIRE(iterator.CurrentLiteralInfo(*second) == second->Lookup("SELECT"));
	REQUIRE_FALSE(iterator.CurrentLiteralInfo(*second) == first.Lookup("SELECT"));
	REQUIRE(branch.CurrentLiteralInfo(first).LiteralId() == 0);
	REQUIRE(branch.CurrentLiteralInfo(*second) == second->Lookup("extension_word"));
	REQUIRE(branch.CurrentLiteralInfo(first).LiteralId() == 0);
	iterator.CurrentLiteralInfo(*second);
	auto old_cache_id = second->CacheId();
	second.reset();
	second.emplace(grammar, first_categories.ToLiteralMap());
	REQUIRE(second->CacheId() != old_cache_id);
	REQUIRE(iterator.CurrentLiteralInfo(*second) == first.Lookup("SELECT"));
	tokens[0].text = "extension_word";
	TokenIterator edited(tokens);
	REQUIRE(edited.CurrentLiteralInfo(*second).LiteralId() == 0);
	branch.Advance();
	REQUIRE(branch.CurrentLiteralInfo(first).LiteralId() == 0);
}

class LiteralTestKeywordHelper final : public PEGKeywordHelper {
public:
	explicit LiteralTestKeywordHelper(bool allow_identifier = false)
	    : literal_table(ParsedGrammar::Parse("LiteralTest <- 'custom_word'"), BuildKeywords(allow_identifier)) {
	}
	const GrammarLiteralTable &GetLiteralTable() const override {
		return literal_table;
	}
	keyword_categories_t GetIdentifierMask(SuggestionState type) const override {
		return DefaultKeywordMaps::GetIdentifierMask(type);
	}
	vector<ParserKeyword> KeywordList() const override {
		return {};
	}

private:
	static case_insensitive_map_t<LiteralInfo> BuildKeywords(bool allow_identifier) {
		DefaultKeywordMaps maps;
		if (allow_identifier) {
			maps.unreserved_keyword_map.insert("custom_word");
		} else {
			maps.reserved_keyword_map.insert("custom_word");
		}
		return maps.ToLiteralMap();
	}

	GrammarLiteralTable literal_table;
};

static bool MatchLiteralTestToken(const Matcher &matcher, const string &text) {
	vector<MatcherToken> tokens {MatcherToken(text, 0, TokenType::IDENTIFIER)};
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	MatchContext context(suggestions, allocator, max_token_index);
	MatchState state(iterator, context);
	return matcher.MatchParseResult(state).IsSuccess();
}

TEST_CASE("Custom keyword helpers and standalone literal matchers keep their semantics", "[api][grammar_extension]") {
	LiteralTestKeywordHelper helper;
	REQUIRE(helper.GetLiteralTable().Lookup("custom_word").LiteralId() != 0);
	IdentifierMatcher identifier(SuggestionState::SUGGEST_COLUMN_NAME, helper);
	REQUIRE_FALSE(MatchLiteralTestToken(identifier, "CUSTOM_WORD"));
	LiteralTestKeywordHelper permissive_helper(true);
	IdentifierMatcher permissive_identifier(SuggestionState::SUGGEST_COLUMN_NAME, permissive_helper);
	REQUIRE(MatchLiteralTestToken(permissive_identifier, "CUSTOM_WORD"));
	KeywordMatcher standalone("custom_word", KeywordInfo());
	KeywordMatcher custom_helper("custom_word", KeywordInfo(), helper);
	REQUIRE(MatchLiteralTestToken(standalone, "CUSTOM_WORD"));
	REQUIRE(MatchLiteralTestToken(custom_helper, "CUSTOM_WORD"));
	REQUIRE_FALSE(MatchLiteralTestToken(custom_helper, "another_word"));
	KeywordMatcher unregistered("unregistered_literal_test_word", KeywordInfo(),
	                            CompiledGrammar::Base().GetKeywordHelper());
	REQUIRE(MatchLiteralTestToken(unregistered, "UNREGISTERED_LITERAL_TEST_WORD"));
	REQUIRE_FALSE(MatchLiteralTestToken(unregistered, "another_word"));
}

struct RegisteredTransformResult {
	idx_t value;
};

namespace duckdb {
DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE("duckdb.test.RegisteredTransformResult", ::RegisteredTransformResult);
} // namespace duckdb

TEST_CASE("Transform result types use stable registered names", "[api][grammar_extension]") {
	TypedTransformResult<RegisteredTransformResult> result({42});
	auto copied_type_name = string(TransformResultTypeName<RegisteredTransformResult>());

	REQUIRE(result.GetValuePointer(copied_type_name.c_str()) == &result.value);
	REQUIRE(TryGetTransformResult<RegisteredTransformResult>(result) == &result.value);
	REQUIRE(TryGetTransformResult<bool>(result) == nullptr);
}

TEST_CASE("Literal caches follow the grammar they are matched against", "[api][grammar_extension]") {
	auto &base_table = CompiledGrammar::Base().GetKeywordHelper().GetLiteralTable();
	auto grammar = ParsedGrammar::CreateDefault();
	grammar.AddRule("LiteralCacheTestValue <- 'ANSWER'");
	auto extended = CompiledGrammar::Compile(grammar);
	auto &extended_table = extended.GetKeywordHelper().GetLiteralTable();
	REQUIRE(extended_table.CacheId() != base_table.CacheId());
	vector<MatcherToken> tokens {MatcherToken("answer", 0, TokenType::IDENTIFIER)};
	TokenIterator iterator(tokens);
	REQUIRE(iterator.CurrentLiteralInfo(base_table).LiteralId() == 0);
	REQUIRE(iterator.CurrentLiteralInfo(extended_table).LiteralId() != 0);
	REQUIRE(iterator.CurrentLiteralInfo(base_table).LiteralId() == 0);
	KeywordMatcher keyword("ANSWER", KeywordInfo(), extended.GetKeywordHelper());
	REQUIRE(MatchLiteralTestToken(keyword, "answer"));
	REQUIRE_FALSE(MatchLiteralTestToken(keyword, "missing"));
}

class ConstantSevenTransformProcess final : public TransformProcess {
public:
	explicit ConstantSevenTransformProcess(PEGTransformer &transformer_p) : transformer(transformer_p) {
	}

	TransformStep Resume(arena_ptr<TransformResultValue> child_result) override {
		D_ASSERT(!child_result);
		return TransformStep::Complete(
		    transformer.MakeResult<unique_ptr<ParsedExpression>>(ConstantExpression::Integer(7)));
	}

private:
	PEGTransformer &transformer;
};

static arena_ptr<TransformProcess> StartConstantSevenTransform(PEGTransformer &transformer, ParseResult &) {
	return transformer.MakeProcess<ConstantSevenTransformProcess>(transformer);
}

static string TransformFirstSelectExpression(ParsedGrammar &grammar, const string &query) {
	auto compiled = CompiledGrammar::Compile(grammar);
	auto options = ParserOptions::Builtin();
	options.grammar = &compiled;
	Parser parser(options);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	auto &node = parser.statements[0]->Cast<SelectStatement>().node->Cast<SelectNode>();
	return node.select_list[0]->ToString();
}

TEST_CASE("Overriding a transform opts its rule out of collapsing", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::CreateDefault();
	grammar.SetTransformProcess("AdditiveExpression", StartConstantSevenTransform);
	REQUIRE(TransformFirstSelectExpression(grammar, "SELECT 1") == "7");

	grammar.SetTransformProcess("AdditiveExpression", StartConstantSevenTransform, true);
	REQUIRE(TransformFirstSelectExpression(grammar, "SELECT 1") == "1");
	REQUIRE(TransformFirstSelectExpression(grammar, "SELECT 1 + 1") == "7");
}

struct MatchProcessLifetimeState {
	idx_t active = 0;
	idx_t started = 0;
	idx_t depth = 0;
	bool throw_at_leaf = false;
	bool throw_in_constructor = false;
	bool storage_valid = true;
	bool create_result = false;
	bool fail_at_leaf = false;
	bool state_valid = true;
	idx_t root_children = 1;
	vector<idx_t> destroyed;
};

class NestedTestMatchProcess : public MatchProcess {
public:
	NestedTestMatchProcess(const Matcher &matcher_p, MatchState &state, MatchProcessLifetimeState &lifetime_p)
	    : matcher(matcher_p), input_state(state), child_state(state), lifetime(lifetime_p), depth(++lifetime.active) {
		lifetime.started++;
	}

	~NestedTestMatchProcess() override {
		lifetime.state_valid &= &input_state.context == &child_state.context;
		lifetime.destroyed.push_back(depth);
		lifetime.active--;
	}

	MatchStep Resume(const optional<MatcherResult> &child_result) override {
		if (child_result) {
			if (depth == 1 && ++completed_children < lifetime.root_children) {
				return MatchStep::Child({matcher, child_state});
			}
			return MatchStep::Complete(*child_result);
		}
		if (depth < lifetime.depth) {
			return MatchStep::Child({matcher, child_state});
		}
		if (lifetime.throw_at_leaf) {
			throw InvalidInputException("Nested matcher test failure");
		}
		if (lifetime.fail_at_leaf) {
			return MatchStep::Complete(MatcherResult::Failure());
		}
		if (lifetime.create_result) {
			return MatchStep::Complete(child_state.AllocateParseResult<ListParseResult>(
			    std::span<reference<ParseResult>>(), "nested result", optional_idx()));
		}
		return MatchStep::Complete(MatcherResult::Success());
	}

private:
	const Matcher &matcher;
	MatchState &input_state;
	MatchState child_state;
	MatchProcessLifetimeState &lifetime;
	idx_t depth;
	idx_t completed_children = 0;
};

class NestedTestMatcher final : public Matcher {
public:
	explicit NestedTestMatcher(MatchProcessLifetimeState &lifetime_p)
	    : Matcher(MatcherType::LIST), lifetime(lifetime_p) {
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		return state.Make<NestedTestMatchProcess>(*this, state, lifetime);
	}

	SuggestionType AddSuggestionInternal(MatchState &) const override {
		return SuggestionType::MANDATORY;
	}

	string ToString() const override {
		return "NestedTestMatcher";
	}

private:
	MatchProcessLifetimeState &lifetime;
};

TEST_CASE("Matcher stack vector growth preserves custom process lifetimes", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	MatchContext context(suggestions, allocator, max_token_index);
	MatchState state(iterator, context);
	MatchProcessLifetimeState lifetime;
	lifetime.destroyed.reserve(2049);
	NestedTestMatcher matcher(lifetime);

	SECTION("Completed frames are reused across vector growth boundaries") {
		MatchStack stack;
		lifetime.create_result = true;
		for (idx_t depth : {idx_t(1), idx_t(64), idx_t(65), idx_t(128), idx_t(129), idx_t(256), idx_t(257), idx_t(1025),
		                    idx_t(33), idx_t(130)}) {
			context.processes.FreeAll();
			lifetime.depth = depth;
			lifetime.started = 0;
			lifetime.destroyed.clear();
			auto result = stack.Execute({matcher, state});
			REQUIRE(result.IsSuccess());
			REQUIRE(result.HasParseResult());
			REQUIRE(result.GetParseResult()->name == "nested result");
			REQUIRE(lifetime.active == 0);
			REQUIRE(lifetime.state_valid);
			REQUIRE(lifetime.started == depth);
			REQUIRE(lifetime.destroyed.size() == depth);
			for (idx_t i = 0; i < depth; i++) {
				REQUIRE(lifetime.destroyed[i] == depth - i);
			}
		}
	}

	SECTION("Exceptions destroy child processes before their parents") {
		lifetime.depth = 1025;
		lifetime.throw_at_leaf = true;
		{
			MatchStack stack;
			REQUIRE_THROWS_AS(stack.Execute({matcher, state}), InvalidInputException);
		}
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == lifetime.depth);
		REQUIRE(lifetime.destroyed.size() == lifetime.depth);
		for (idx_t i = 0; i < lifetime.depth; i++) {
			REQUIRE(lifetime.destroyed[i] == lifetime.depth - i);
		}
	}

	SECTION("A waiting parent resumes after multiple deep children") {
		lifetime.depth = 1025;
		lifetime.root_children = 2;
		MatchStack stack;
		REQUIRE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == 1 + 2 * (lifetime.depth - 1));
		REQUIRE(lifetime.destroyed.size() == lifetime.started);
		for (idx_t sibling = 0; sibling < 2; sibling++) {
			for (idx_t i = 0; i < lifetime.depth - 1; i++) {
				REQUIRE(lifetime.destroyed[sibling * (lifetime.depth - 1) + i] == lifetime.depth - i);
			}
		}
		REQUIRE(lifetime.destroyed.back() == 1);
	}

	SECTION("Failed matches leave the vector ready for another execution") {
		lifetime.depth = 1025;
		lifetime.fail_at_leaf = true;
		MatchStack stack;
		REQUIRE_FALSE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == lifetime.depth);
		REQUIRE(lifetime.destroyed.size() == lifetime.depth);
		lifetime.fail_at_leaf = false;
		lifetime.started = 0;
		lifetime.destroyed.clear();
		REQUIRE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == lifetime.depth);
	}
}

class DerivedListTestMatcher final : public ListMatcher {
public:
	explicit DerivedListTestMatcher(MatchProcessLifetimeState &lifetime_p) : lifetime(lifetime_p) {
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		return state.Make<NestedTestMatchProcess>(*this, state, lifetime);
	}

private:
	MatchProcessLifetimeState &lifetime;
};

template <idx_t SIZE>
class ArenaNestedTestMatchProcess final : public NestedTestMatchProcess {
public:
	ArenaNestedTestMatchProcess(const Matcher &matcher, MatchState &state, MatchProcessLifetimeState &lifetime_p)
	    : NestedTestMatchProcess(matcher, state, lifetime_p), lifetime(lifetime_p) {
		lifetime.storage_valid &= reinterpret_cast<uintptr_t>(this) % alignof(ArenaNestedTestMatchProcess) == 0;
		if (lifetime.throw_in_constructor && lifetime.active == lifetime.depth) {
			throw InvalidInputException("Nested process constructor failure");
		}
		payload.fill(0xa5);
	}

	~ArenaNestedTestMatchProcess() override {
		for (auto byte : payload) {
			lifetime.storage_valid &= byte == 0xa5;
		}
	}

private:
	MatchProcessLifetimeState &lifetime;
	array<uint8_t, SIZE> payload;
};

class ArenaNestedTestMatcher final : public ListMatcher {
public:
	explicit ArenaNestedTestMatcher(MatchProcessLifetimeState &lifetime_p) : lifetime(lifetime_p) {
	}

	arena_ptr<MatchProcess> StartMatch(MatchState &state) const override {
		if (lifetime.active % 2) {
			return state.Make<ArenaNestedTestMatchProcess<9000>>(*this, state, lifetime);
		}
		return state.Make<ArenaNestedTestMatchProcess<32>>(*this, state, lifetime);
	}

private:
	MatchProcessLifetimeState &lifetime;
};

TEST_CASE("MatchState allocates processes through its shared context", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_results;
	idx_t max_token_index = 0;
	MatchContext context(suggestions, parse_results, max_token_index);
	MatchState state(iterator, context);
	MatchState child_state(state);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 3;
	ArenaNestedTestMatcher matcher(lifetime);
	auto parent = state.Make<ArenaNestedTestMatchProcess<9000>>(matcher, state, lifetime);
	auto parent_mark = context.processes.Mark();
	REQUIRE(parent_mark >= sizeof(ArenaNestedTestMatchProcess<9000>));
	REQUIRE(&child_state.context.processes == &context.processes);
	REQUIRE(matcher.MatchParseResult(child_state).IsSuccess());
	REQUIRE(context.processes.Mark() == parent_mark);
	REQUIRE(lifetime.active == 1);
	parent.reset();
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.storage_valid);
	REQUIRE(lifetime.state_valid);
	REQUIRE(lifetime.destroyed == vector<idx_t> {3, 2, 1});
}

TEST_CASE("Matcher driver preserves overrides on derived built-in matchers", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	MatchContext context(suggestions, allocator, max_token_index);
	MatchState state(iterator, context);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 130;
	DerivedListTestMatcher matcher(lifetime);
	REQUIRE(matcher.MatchParseResult(state).IsSuccess());
	REQUIRE(lifetime.started == lifetime.depth);
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.destroyed.size() == lifetime.depth);
}

TEST_CASE("Matcher stack supports variable-sized aligned arena processes", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator allocator;
	idx_t max_token_index = 0;
	MatchContext context(suggestions, allocator, max_token_index);
	MatchState state(iterator, context);
	MatchProcessLifetimeState lifetime;
	ArenaNestedTestMatcher matcher(lifetime);

	SECTION("Repeated executions preserve process storage and destruction order") {
		MatchStack stack;
		for (idx_t depth :
		     {idx_t(1), idx_t(64), idx_t(65), idx_t(128), idx_t(129), idx_t(130), idx_t(33), idx_t(130)}) {
			context.processes.FreeAll();
			lifetime.depth = depth;
			lifetime.started = 0;
			lifetime.destroyed.clear();
			REQUIRE(stack.Execute({matcher, state}).IsSuccess());
			REQUIRE(lifetime.started == depth);
			REQUIRE(lifetime.active == 0);
			REQUIRE(lifetime.storage_valid);
			REQUIRE(lifetime.state_valid);
			REQUIRE(lifetime.destroyed.size() == depth);
			for (idx_t i = 0; i < depth; i++) {
				REQUIRE(lifetime.destroyed[i] == depth - i);
			}
		}
	}

	SECTION("Resume exceptions destroy arena children before their parents") {
		lifetime.throw_at_leaf = true;
	}
	SECTION("Constructor exceptions destroy constructed members and parent processes") {
		lifetime.throw_in_constructor = true;
	}
	SECTION("Siblings preserve process storage while their parent stays alive") {
		lifetime.depth = 65;
		lifetime.root_children = 2;
		MatchStack stack;
		REQUIRE(stack.Execute({matcher, state}).IsSuccess());
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.storage_valid);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.started == 1 + 2 * (lifetime.depth - 1));
	}
	if (lifetime.throw_at_leaf || lifetime.throw_in_constructor) {
		lifetime.depth = 130;
		{
			MatchStack stack;
			REQUIRE_THROWS_AS(stack.Execute({matcher, state}), InvalidInputException);
		}
		REQUIRE(lifetime.active == 0);
		REQUIRE(lifetime.started == lifetime.depth);
		REQUIRE(lifetime.storage_valid);
		REQUIRE(lifetime.state_valid);
		REQUIRE(lifetime.destroyed.size() == lifetime.depth);
		for (idx_t i = 0; i < lifetime.depth; i++) {
			REQUIRE(lifetime.destroyed[i] == lifetime.depth - i);
		}
	}
}

TEST_CASE("Packrat results outlive reset process arenas", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_results;
	idx_t max_token_index = 0;
	MatchContext context(suggestions, parse_results, max_token_index);
	MatchProcessLifetimeState lifetime;
	lifetime.depth = 3;
	lifetime.create_result = true;
	MatcherAllocator matchers;
	auto &matcher = matchers.Allocate(make_uniq<ArenaNestedTestMatcher>(lifetime));
	matchers.SetPackratMemoized(matcher);
	ParserPackratCache cache(0, matchers.PackratSlotCount());
	context.packrat_cache = &cache;
	MatchState state(iterator, context);
	MatchStack stack;

	SECTION("Cached successes retain separately allocated parse results") {
	}
	SECTION("Cached failures need no new process") {
		lifetime.fail_at_leaf = true;
	}
	auto first = stack.Execute({matcher, state});
	REQUIRE(first.IsSuccess() == !lifetime.fail_at_leaf);
	REQUIRE(lifetime.started == lifetime.depth);
	REQUIRE(lifetime.active == 0);

	ArenaNestedTestMatcher overwriter(lifetime);
	lifetime.depth = 130;
	lifetime.create_result = false;
	context.processes.FreeAll();
	stack.Execute({overwriter, state});
	context.processes.FreeAll();
	lifetime.started = 0;
	auto cached = stack.Execute({matcher, state});
	REQUIRE(cached.IsSuccess() == first.IsSuccess());
	REQUIRE(cached.GetParseResult().get() == first.GetParseResult().get());
	REQUIRE(lifetime.started == 0);
	REQUIRE(lifetime.active == 0);
	REQUIRE(lifetime.storage_valid);
	REQUIRE(lifetime.state_valid);
	if (cached.IsSuccess()) {
		REQUIRE(cached.HasParseResult());
		REQUIRE(cached.GetParseResult()->name == "nested result");
	}
}

TEST_CASE("Compiled grammar processes use arena ownership", "[api][grammar_extension]") {
	vector<MatcherToken> tokens;
	TokenIterator iterator(tokens);
	vector<MatcherSuggestion> suggestions;
	ParseResultAllocator parse_results;
	idx_t max_token_index = 0;
	MatchContext context(suggestions, parse_results, max_token_index);
	MatchState state(iterator, context);
	auto grammar = CompiledGrammar::Create();
	auto process = grammar.TopLevelStatementMatcher().StartMatch(state);
	REQUIRE(process);
	REQUIRE(context.processes.Mark() > 0);
	process.reset();
	context.processes.FreeAll();
	REQUIRE(context.processes.Mark() == 0);
}

TEST_CASE("Default terminal rule overrides are registered before additions", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::CreateDefault();
	grammar.AddTerminalRuleOverride("identifier", [](const PEGKeywordHelper &) {
		return make_uniq<KeywordMatcher>("replacement", KeywordInfo(0, ' '));
	});
	REQUIRE_THROWS(CompiledGrammar::Compile(grammar));
}

TEST_CASE("Sessions parse with the base grammar", "[api][grammar_extension]") {
	DuckDB db(nullptr);
	Connection con(db);
	Connection other(db);
	REQUIRE(&CompiledGrammar::Get(*con.context) == &CompiledGrammar::Base());
	REQUIRE(&CompiledGrammar::Get(*other.context) == &CompiledGrammar::Base());
	REQUIRE(&CompiledGrammar::Passthrough() != &CompiledGrammar::Base());

	auto parser = Parser::GetBuiltinParser();
	REQUIRE_NOTHROW(parser.ParseQuery("SELECT 42"));
	REQUIRE(parser.statements.size() == 1);
}

TEST_CASE("Grammars with missing rule references fail compilation", "[api][grammar_extension]") {
	auto grammar = ParsedGrammar::CreateDefault();
	grammar.AddRule("GrammarExtensionInvalid <- GrammarExtensionMissingRule");
	REQUIRE_THROWS(CompiledGrammar::Compile(grammar));
}
