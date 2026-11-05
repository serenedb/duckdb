#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_subscription_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_subscription_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"

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

static Value SubscriptionOptionValue(PEGTransformer &transformer, ParseResult &def_arg) {
	auto expr = transformer.Transform<unique_ptr<ParsedExpression>>(def_arg);
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &column_ref = expr->Cast<ColumnRefExpression>();
		if (!column_ref.IsQualified()) {
			return Value(column_ref.GetColumnName().GetIdentifierName());
		}
	}
	if (expr->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException("subscription option value must be a constant");
	}
	auto value = expr->Cast<ConstantExpression>().GetLiteral().ToValue();
	if (value.IsNull()) {
		return Value("none");
	}
	return value;
}

static void SubscriptionOptionInto(PEGTransformer &transformer, ParseResult &option,
                                   case_insensitive_map_t<Value> &options) {
	auto &option_list = option.Cast<ListParseResult>();
	auto name = StringUtil::Lower(transformer.Transform<string>(option_list.GetChild(0)));
	auto &arg = option_list.Child<OptionalParseResult>(1);
	auto value = arg.HasResult()
	                 ? SubscriptionOptionValue(transformer, arg.GetResult().Cast<ListParseResult>().GetChild(1))
	                 : Value::BOOLEAN(true);
	if (!options.emplace(name, std::move(value)).second) {
		throw ParserException("conflicting or redundant options: \"%s\" specified more than once", name);
	}
}

static void SubscriptionOptionListInto(PEGTransformer &transformer, ParseResult &parens,
                                       case_insensitive_map_t<Value> &options) {
	auto &list_inside = PEGTransformerFactory::ExtractResultFromParens(parens);
	for (auto &option : PEGTransformerFactory::ExtractParseResultsFromList(list_inside)) {
		SubscriptionOptionInto(transformer, option.get(), options);
	}
}

static void SubscriptionWithInto(PEGTransformer &transformer, OptionalParseResult &with,
                                 case_insensitive_map_t<Value> &options) {
	if (!with.HasResult()) {
		return;
	}
	SubscriptionOptionListInto(transformer, with.GetResult().Cast<ListParseResult>().GetChild(1), options);
}

static vector<string> SubscriptionPublications(PEGTransformer &transformer, ParseResult &list) {
	vector<string> result;
	for (auto &publication : PEGTransformerFactory::ExtractParseResultsFromList(list)) {
		result.push_back(transformer.Transform<string>(publication.get()));
	}
	return result;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateSubscriptionStatement(PEGTransformer &transformer,
                                                                                     ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto info = make_uniq<CreateSubscriptionInfo>();
	info->SetName(Identifier(transformer.Transform<string>(list_pr.GetChild(2))));
	info->conninfo = transformer.Transform<string>(list_pr.GetChild(4));
	info->publications = SubscriptionPublications(transformer, list_pr.GetChild(6));
	SubscriptionWithInto(transformer, list_pr.Child<OptionalParseResult>(7), info->options);
	auto result = make_uniq<CreateStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropSubscriptionStatement(PEGTransformer &transformer,
                                                                                   ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::SUBSCRIPTION_ENTRY;
	info->SetName(Identifier(transformer.Transform<string>(list_pr.GetChild(3))));
	if (list_pr.Child<OptionalParseResult>(2).HasResult()) {
		info->if_not_found = OnEntryNotFound::RETURN_NULL;
	}
	transformer.TransformOptional<bool>(list_pr, 4, info->cascade);
	auto result = make_uniq<DropStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterSubscriptionStatement(PEGTransformer &transformer,
                                                                                    ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	Identifier name(transformer.Transform<string>(list_pr.GetChild(2)));
	auto result = make_uniq<AlterStatement>();
	auto &action = SubscriptionUnwrap(list_pr.GetChild(3));
	if (action.type == ParseResultType::KEYWORD) {
		result->info = make_uniq<AlterSubscriptionInfo>(
		    SubscriptionKeyword(action) == "enable" ? AlterSubscriptionType::ENABLE : AlterSubscriptionType::DISABLE,
		    name);
		return std::move(result);
	}
	auto &action_list = action.Cast<ListParseResult>();
	if (action_list.Name() == "SubscriptionOwnerAction") {
		auto &role = SubscriptionUnwrap(action_list.GetChild(2));
		auto info = make_uniq<AlterPermissionsInfo>(CatalogType::SUBSCRIPTION_ENTRY, QualifiedName(name));
		info->new_owner = role.type == ParseResultType::KEYWORD
		                      ? StringUtil::Upper(string(role.Cast<KeywordParseResult>().keyword))
		                      : transformer.Transform<string>(role);
		result->info = std::move(info);
		return std::move(result);
	}
	unique_ptr<AlterSubscriptionInfo> info;
	if (action_list.Name() == "SubscriptionConnectionAction") {
		info = make_uniq<AlterSubscriptionInfo>(AlterSubscriptionType::SET_CONNECTION, name);
		info->connection = transformer.Transform<string>(action_list.GetChild(1));
	} else if (action_list.Name() == "SubscriptionPublicationAction") {
		auto mode = SubscriptionKeyword(action_list.GetChild(0));
		info = make_uniq<AlterSubscriptionInfo>(mode == "set"   ? AlterSubscriptionType::SET_PUBLICATION
		                                        : mode == "add" ? AlterSubscriptionType::ADD_PUBLICATION
		                                                        : AlterSubscriptionType::DROP_PUBLICATION,
		                                        name);
		info->publications = SubscriptionPublications(transformer, action_list.GetChild(2));
		SubscriptionWithInto(transformer, action_list.Child<OptionalParseResult>(3), info->options);
	} else if (action_list.Name() == "SubscriptionRefreshAction") {
		info = make_uniq<AlterSubscriptionInfo>(AlterSubscriptionType::REFRESH_PUBLICATION, name);
		SubscriptionWithInto(transformer, action_list.Child<OptionalParseResult>(2), info->options);
	} else if (action_list.Name() == "SubscriptionSkipAction") {
		info = make_uniq<AlterSubscriptionInfo>(AlterSubscriptionType::SKIP, name);
		SubscriptionOptionInto(transformer, PEGTransformerFactory::ExtractResultFromParens(action_list.GetChild(1)),
		                       info->options);
	} else if (action_list.Name() == "SubscriptionRenameAction") {
		info = make_uniq<AlterSubscriptionInfo>(AlterSubscriptionType::RENAME, name);
		info->new_name = Identifier(transformer.Transform<string>(action_list.GetChild(2)));
	} else {
		info = make_uniq<AlterSubscriptionInfo>(AlterSubscriptionType::SET_OPTIONS, name);
		SubscriptionOptionListInto(transformer, action_list.GetChild(1), info->options);
	}
	result->info = std::move(info);
	return std::move(result);
}

} // namespace duckdb
