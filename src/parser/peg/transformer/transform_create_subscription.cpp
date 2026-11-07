#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"

namespace duckdb {

static ParseResult &SubscriptionUnwrap(ParseResult &node) {
	auto *current = &node;
	for (;;) {
		if (current->type == ParseResultType::CHOICE) {
			current = &current->Cast<ChoiceParseResult>().GetResult();
		} else if (current->type == ParseResultType::LIST &&
		           current->Cast<ListParseResult>().GetChildren().size() == 1) {
			current = &current->Cast<ListParseResult>().GetChild(0);
		} else {
			return *current;
		}
	}
}

static string SubscriptionKeyword(ParseResult &node) {
	auto &unwrapped = SubscriptionUnwrap(node);
	if (unwrapped.type == ParseResultType::LIST) {
		return SubscriptionKeyword(unwrapped.Cast<ListParseResult>().GetChild(0));
	}
	return StringUtil::Lower(string(unwrapped.Cast<KeywordParseResult>().keyword));
}

static unique_ptr<ParsedExpression> SubscriptionOptionValue(PEGTransformer &transformer, ParseResult &def_arg) {
	auto expr = transformer.Transform<unique_ptr<ParsedExpression>>(def_arg);
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &column_ref = expr->Cast<ColumnRefExpression>();
		if (!column_ref.IsQualified()) {
			return ConstantExpression::String(column_ref.GetColumnName().GetIdentifierName());
		}
	}
	if (expr->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("subscription option value must be a constant");
	}
	if (expr->Cast<ConstantExpression>().GetLiteral().ToValue().IsNull()) {
		return ConstantExpression::String("none");
	}
	return expr;
}

static void SubscriptionOptionInto(PEGTransformer &transformer, ParseResult &option, PragmaInfo &info) {
	auto &option_list = option.Cast<ListParseResult>();
	auto name = StringUtil::Lower(transformer.Transform<string>(option_list.GetChild(0)));
	auto &arg = option_list.Child<OptionalParseResult>(1);
	auto value = arg.HasResult()
	                 ? SubscriptionOptionValue(transformer, arg.GetResult().Cast<ListParseResult>().GetChild(1))
	                 : ConstantExpression::Boolean(true);
	auto inserted = info.named_parameters.emplace(name, std::move(value)).second;
	if (!inserted) {
		throw ParserException("conflicting or redundant options: \"%s\" specified more than once", name);
	}
}

static void SubscriptionOptionListInto(PEGTransformer &transformer, ParseResult &parens, PragmaInfo &info) {
	auto &list_inside = PEGTransformerFactory::ExtractResultFromParens(parens);
	for (auto &option : PEGTransformerFactory::ExtractParseResultsFromList(list_inside)) {
		SubscriptionOptionInto(transformer, option.get(), info);
	}
}

static void SubscriptionWithInto(PEGTransformer &transformer, OptionalParseResult &with, PragmaInfo &info) {
	if (!with.HasResult()) {
		return;
	}
	SubscriptionOptionListInto(transformer, with.GetResult().Cast<ListParseResult>().GetChild(1), info);
}

static void SubscriptionPublicationsInto(PEGTransformer &transformer, ParseResult &list, PragmaInfo &info) {
	for (auto &publication : PEGTransformerFactory::ExtractParseResultsFromList(list)) {
		info.parameters.push_back(ConstantExpression::String(transformer.Transform<string>(publication.get())));
	}
}

static unique_ptr<PragmaStatement> AlterSubscriptionPragma(const string &name, const string &action,
                                                           const string &arg) {
	auto result = make_uniq<PragmaStatement>();
	result->info->name = "alter_subscription";
	result->info->parameters.push_back(ConstantExpression::String(name));
	result->info->parameters.push_back(ConstantExpression::String(action));
	result->info->parameters.push_back(ConstantExpression::String(arg));
	return result;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateSubscriptionStatement(PEGTransformer &transformer,
                                                                                     ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto result = make_uniq<PragmaStatement>();
	result->info->name = "create_subscription";
	result->info->parameters.push_back(ConstantExpression::String(transformer.Transform<string>(list_pr.GetChild(2))));
	result->info->parameters.push_back(ConstantExpression::String(transformer.Transform<string>(list_pr.GetChild(4))));
	SubscriptionPublicationsInto(transformer, list_pr.GetChild(6), *result->info);
	SubscriptionWithInto(transformer, list_pr.Child<OptionalParseResult>(7), *result->info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropSubscriptionStatement(PEGTransformer &transformer,
                                                                                   ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	bool missing_ok = list_pr.Child<OptionalParseResult>(2).HasResult();
	bool cascade = false;
	transformer.TransformOptional<bool>(list_pr, 4, cascade);
	auto result = make_uniq<PragmaStatement>();
	result->info->name = "drop_subscription";
	result->info->parameters.push_back(ConstantExpression::String(transformer.Transform<string>(list_pr.GetChild(3))));
	result->info->parameters.push_back(ConstantExpression::Boolean(missing_ok));
	result->info->parameters.push_back(ConstantExpression::Boolean(cascade));
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterSubscriptionStatement(PEGTransformer &transformer,
                                                                                    ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto name = transformer.Transform<string>(list_pr.GetChild(2));
	auto &action = SubscriptionUnwrap(list_pr.GetChild(3));
	if (action.type == ParseResultType::KEYWORD) {
		return AlterSubscriptionPragma(name, SubscriptionKeyword(action), string());
	}
	auto &action_list = action.Cast<ListParseResult>();
	if (action_list.Name() == "SubscriptionConnectionAction") {
		return AlterSubscriptionPragma(name, "connection", transformer.Transform<string>(action_list.GetChild(1)));
	}
	if (action_list.Name() == "SubscriptionPublicationAction") {
		auto result =
		    AlterSubscriptionPragma(name, SubscriptionKeyword(action_list.GetChild(0)) + "_publication", string());
		SubscriptionPublicationsInto(transformer, action_list.GetChild(2), *result->info);
		SubscriptionWithInto(transformer, action_list.Child<OptionalParseResult>(3), *result->info);
		return std::move(result);
	}
	if (action_list.Name() == "SubscriptionRefreshAction") {
		auto result = AlterSubscriptionPragma(name, "refresh_publication", string());
		SubscriptionWithInto(transformer, action_list.Child<OptionalParseResult>(2), *result->info);
		return std::move(result);
	}
	if (action_list.Name() == "SubscriptionSkipAction") {
		auto result = AlterSubscriptionPragma(name, "skip", string());
		SubscriptionOptionInto(transformer, PEGTransformerFactory::ExtractResultFromParens(action_list.GetChild(1)),
		                       *result->info);
		return std::move(result);
	}
	if (action_list.Name() == "SubscriptionRenameAction") {
		return AlterSubscriptionPragma(name, "rename", transformer.Transform<string>(action_list.GetChild(2)));
	}
	if (action_list.Name() == "SubscriptionOwnerAction") {
		auto &role = SubscriptionUnwrap(action_list.GetChild(2));
		auto owner = role.type == ParseResultType::KEYWORD
		                 ? StringUtil::Upper(string(role.Cast<KeywordParseResult>().keyword))
		                 : transformer.Transform<string>(role);
		return AlterSubscriptionPragma(name, "owner", owner);
	}
	auto result = AlterSubscriptionPragma(name, "set", string());
	SubscriptionOptionListInto(transformer, action_list.GetChild(1), *result->info);
	return std::move(result);
}

} // namespace duckdb
