#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/default_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

// Extract the matched isolation level string from an IsolationLevel parse result.
// Grammar: IsolationLevel <- ('READ' 'COMMITTED') / ('READ' 'UNCOMMITTED') /
//                            ('REPEATABLE' 'READ') / 'SERIALIZABLE'
// Returns a value compatible with TransactionIsolationLevel's enum string form
// (e.g. "read committed", "repeatable read").
string ExtractIsolationLevelString(ParseResult &parse_result) {
	// IsolationLevel is a top-level rule whose body is a Choice -> the matcher
	// wraps a single ChoiceMatcher in the rule's outer ListMatcher.
	auto &outer = parse_result.Cast<ListParseResult>();
	auto &choice_pr = outer.Child<ChoiceParseResult>(0);
	auto &inner = choice_pr.GetResult();
	if (inner.type == ParseResultType::KEYWORD) {
		// 'SERIALIZABLE'
		auto &keyword = inner.Cast<KeywordParseResult>().keyword;
		return StringUtil::Lower(keyword);
	}
	// Sequence of two keywords: ('READ' 'COMMITTED'), ('READ' 'UNCOMMITTED'),
	// or ('REPEATABLE' 'READ').
	auto &inner_list = inner.Cast<ListParseResult>();
	auto &first = inner_list.Child<KeywordParseResult>(0).keyword;
	auto &second = inner_list.Child<KeywordParseResult>(1).keyword;
	return StringUtil::Lower(first) + " " + StringUtil::Lower(second);
}

} // namespace

// ResetStatement <- 'RESET' (SetVariable / SetSetting)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformResetStatement(PEGTransformer &transformer,
                                                                        const SettingInfo &set_variable_or_setting) {
	if (set_variable_or_setting.scope == SetScope::LOCAL) {
		throw NotImplementedException("RESET LOCAL is not implemented.");
	}
	return make_uniq<ResetVariableStatement>(set_variable_or_setting.name, set_variable_or_setting.scope);
}

// SetTransactionIsolation <- 'TRANSACTION' 'ISOLATION' 'LEVEL' IsolationLevel
// Maps to PG's SET TRANSACTION ISOLATION LEVEL ...; we forward the parsed level
// into serenedb's existing "transaction_isolation" client setting, whose
// SetLocal callback enforces "must be inside a transaction".
unique_ptr<SetStatement> PEGTransformerFactory::TransformSetTransactionIsolation(PEGTransformer &transformer,
                                                                                 ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// children: 'TRANSACTION', 'ISOLATION', 'LEVEL', IsolationLevel
	auto level = ExtractIsolationLevelString(list_pr.Child<ListParseResult>(3));
	return make_uniq<SetVariableStatement>("transaction_isolation", make_uniq<ConstantExpression>(Value(level)),
	                                       SetScope::AUTOMATIC);
}

// SetSessionCharacteristics <- 'SESSION' 'CHARACTERISTICS' 'AS' 'TRANSACTION' 'ISOLATION' 'LEVEL' IsolationLevel
// Maps to PG's SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL ...;
// forwarded into serenedb's "default_transaction_isolation" setting, which is
// what BEGIN reads as the default for new transactions.
unique_ptr<SetStatement> PEGTransformerFactory::TransformSetSessionCharacteristics(PEGTransformer &transformer,
                                                                                   ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// children: 'SESSION', 'CHARACTERISTICS', 'AS', 'TRANSACTION', 'ISOLATION', 'LEVEL', IsolationLevel
	auto level = ExtractIsolationLevelString(list_pr.Child<ListParseResult>(6));
	return make_uniq<SetVariableStatement>("default_transaction_isolation", make_uniq<ConstantExpression>(Value(level)),
	                                       SetScope::AUTOMATIC);
}

// SetAssignment <- VariableAssign VariableList
vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSetAssignment(PEGTransformer &transformer,
                                              vector<unique_ptr<ParsedExpression>> variable_list) {
	return variable_list;
}

// SetSetting <- SettingScope? SettingName
SettingInfo PEGTransformerFactory::TransformSetSetting(PEGTransformer &transformer,
                                                       const optional<SetScope> &setting_scope,
                                                       const Identifier &setting_name) {
	SettingInfo result;
	result.name = setting_name;
	if (setting_scope) {
		result.scope = *setting_scope;
	}
	return result;
}

// SetStatement <- 'SET' SetAssignmentOrTimeZone
unique_ptr<SQLStatement>
PEGTransformerFactory::TransformSetStatement(PEGTransformer &transformer,
                                             unique_ptr<SetStatement> set_assignment_or_time_zone) {
	return std::move(set_assignment_or_time_zone);
}

// ZoneLocal <- 'LOCAL'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneLocal(PEGTransformer &transformer) {
	return make_uniq<DefaultExpression>();
}

// ZoneDefault <- 'DEFAULT'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneDefault(PEGTransformer &transformer) {
	return make_uniq<DefaultExpression>();
}

// ZoneStringLiteral <- StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneStringLiteral(PEGTransformer &transformer,
                                                                               const string &string_literal) {
	return make_uniq<ConstantExpression>(Value(string_literal));
}

// ZoneIdentifier <- Identifier
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneIdentifier(PEGTransformer &transformer,
                                                                            const Identifier &identifier) {
	return make_uniq<ConstantExpression>(Value(identifier));
}

// SetTimeZone <- 'TIME' 'ZONE' ZoneValue
unique_ptr<SetStatement> PEGTransformerFactory::TransformSetTimeZone(PEGTransformer &transformer,
                                                                     unique_ptr<ParsedExpression> zone_value) {
	if (zone_value->GetExpressionClass() == ExpressionClass::DEFAULT) {
		return make_uniq<ResetVariableStatement>("timezone", SetScope::AUTOMATIC);
	}
	return make_uniq<SetVariableStatement>("timezone", std::move(zone_value), SetScope::AUTOMATIC);
}

// SetVariable <- VariableScope Identifier
SettingInfo PEGTransformerFactory::TransformSetVariable(PEGTransformer &transformer, const SetScope &variable_scope,
                                                        const Identifier &identifier) {
	SettingInfo result;
	result.name = identifier;
	result.scope = variable_scope;
	return result;
}

// StandardAssignment <- SetVariableOrSetting SetAssignment
unique_ptr<SetStatement>
PEGTransformerFactory::TransformStandardAssignment(PEGTransformer &transformer,
                                                   const SettingInfo &set_variable_or_setting,
                                                   vector<unique_ptr<ParsedExpression>> set_assignment) {
	if (set_variable_or_setting.scope == SetScope::LOCAL) {
		throw NotImplementedException("SET LOCAL is not implemented.");
	}
	if (set_assignment.size() > 1) {
		throw ParserException("SET can only contain a single value");
	}
	auto value = std::move(set_assignment[0]);
	if (value->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		// SET value cannot be a column reference
		auto &col_ref = value->Cast<ColumnRefExpression>();
		value = make_uniq<ConstantExpression>(col_ref.GetColumnName());
	} else if (value->GetExpressionClass() == ExpressionClass::DEFAULT) {
		return make_uniq<ResetVariableStatement>(set_variable_or_setting.name, set_variable_or_setting.scope);
	}
	return make_uniq<SetVariableStatement>(set_variable_or_setting.name, std::move(value),
	                                       set_variable_or_setting.scope);
}

// VariableList <- List(Expression)
vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformVariableList(PEGTransformer &transformer,
                                             vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

// VariableScope <- 'VARIABLE'
SetScope PEGTransformerFactory::TransformVariableScope(PEGTransformer &transformer) {
	return SetScope::VARIABLE;
}

// LocalScope <- 'LOCAL'
SetScope PEGTransformerFactory::TransformLocalScope(PEGTransformer &transformer) {
	return SetScope::LOCAL;
}

// SessionScope <- 'SESSION'
SetScope PEGTransformerFactory::TransformSessionScope(PEGTransformer &transformer) {
	return SetScope::SESSION;
}

// GlobalScope <- 'GLOBAL'
SetScope PEGTransformerFactory::TransformGlobalScope(PEGTransformer &transformer) {
	return SetScope::GLOBAL;
}

// ZoneIntervalWithInterval <- 'INTERVAL' StringLiteral Interval?
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformZoneIntervalWithInterval(PEGTransformer &transformer, const string &string_literal,
                                                         const optional<DatePartSpecifier> &interval) {
	auto expr = make_uniq<ConstantExpression>(Value(string_literal));
	return make_uniq<CastExpression>(LogicalType::INTERVAL, std::move(expr));
}

// ZoneIntervalWithPrecision <- 'INTERVAL' Parens(NumberLiteral) StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformZoneIntervalWithPrecision(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> number_literal, const string &string_literal) {
	auto expr = make_uniq<ConstantExpression>(Value(string_literal));
	return make_uniq<CastExpression>(LogicalType::INTERVAL, std::move(expr));
}

} // namespace duckdb
