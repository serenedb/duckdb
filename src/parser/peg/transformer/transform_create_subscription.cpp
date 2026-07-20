#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// A keyword-only rule (e.g. SubscriptionEnableAction <- 'ENABLE' / 'DISABLE')
// can resolve to a bare KEYWORD or a CHOICE/LIST wrapper; descend to the
// matched keyword regardless.
static KeywordParseResult &SubscriptionFirstKeyword(ParseResult &node) {
	ParseResult *cur = &node;
	for (;;) {
		switch (cur->type) {
		case ParseResultType::KEYWORD:
			return cur->Cast<KeywordParseResult>();
		case ParseResultType::CHOICE:
			cur = &cur->Cast<ChoiceParseResult>().GetResult();
			break;
		case ParseResultType::OPTIONAL:
			cur = &cur->Cast<OptionalParseResult>().GetResult();
			break;
		case ParseResultType::LIST:
			cur = &cur->Cast<ListParseResult>().GetChild(0);
			break;
		default:
			return cur->Cast<KeywordParseResult>();
		}
	}
}

// CreateSubscriptionStatement <- 'CREATE' 'SUBSCRIPTION' Identifier SubscriptionConnection?
// SubscriptionConnection      <- 'CONNECTION' StringLiteral SubscriptionPublication?
// SubscriptionPublication     <- 'PUBLICATION' List(Identifier)
//
// Transforms to:  PRAGMA create_subscription('sub_name')
//                 PRAGMA create_subscription('sub_name', connection := 'connstr')
//                 PRAGMA create_subscription('sub_name', connection := 'connstr', publications := 'pub1,pub2')
//
// The pragma handler itself is not yet implemented (catalog work is out of scope here).
// Parsing must succeed so that higher-level code can detect the statement type.
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateSubscriptionStatement(PEGTransformer &transformer,
                                                                                     ParseResult &parse_result) {
	// Layout of the list children (literals are counted as positions):
	//  0: 'CREATE'
	//  1: 'SUBSCRIPTION'
	//  2: Identifier  (subscription name)
	//  3: SubscriptionConnection?
	auto &list_pr = parse_result.Cast<ListParseResult>();

	auto &sub_name = list_pr.Child<IdentifierParseResult>(2).identifier;

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "create_subscription";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(sub_name.GetIdentifierName())));

	auto &conn_opt = list_pr.Child<OptionalParseResult>(3);
	if (conn_opt.HasResult()) {
		// SubscriptionConnection <- 'CONNECTION' StringLiteral SubscriptionPublication?
		// List children:
		//  0: 'CONNECTION'
		//  1: StringLiteral (connection string)
		//  2: SubscriptionPublication?
		auto &conn_list = conn_opt.GetResult().Cast<ListParseResult>();
		auto connection_str = conn_list.Child<StringLiteralParseResult>(1).result;
		result->info->named_parameters["connection"] = make_uniq<ConstantExpression>(Value(connection_str));

		auto &pub_opt = conn_list.Child<OptionalParseResult>(2);
		if (pub_opt.HasResult()) {
			// SubscriptionPublication <- 'PUBLICATION' List(Identifier)
			// List children:
			//  0: 'PUBLICATION'
			//  1: the List(...) inline expansion — a ListParseResult of Identifier results
			auto &pub_list = pub_opt.GetResult().Cast<ListParseResult>();
			auto pub_results = ExtractParseResultsFromList(pub_list.Child<ListParseResult>(1));
			string publications_value;
			for (idx_t i = 0; i < pub_results.size(); i++) {
				if (i > 0) {
					publications_value += ",";
				}
				publications_value += pub_results[i].get().Cast<IdentifierParseResult>().identifier.GetIdentifierName();
			}
			result->info->named_parameters["publications"] = make_uniq<ConstantExpression>(Value(publications_value));

			// SubscriptionPublication <- 'PUBLICATION' List(Identifier) SubscriptionWith?
			// SubscriptionWith        <- 'WITH' Parens(List(SubscriptionOption))
			// SubscriptionOption      <- ColLabel SubscriptionOptionArg?
			// SubscriptionOptionArg   <- '=' DefArg
			auto &with_opt = pub_list.Child<OptionalParseResult>(2);
			if (with_opt.HasResult()) {
				auto &with_list = with_opt.GetResult().Cast<ListParseResult>();
				auto &list_inside = ExtractResultFromParens(with_list.GetChild(1));
				for (auto &opt_ref : ExtractParseResultsFromList(list_inside)) {
					auto &opt_pr = opt_ref.get().Cast<ListParseResult>();
					auto opt_name = transformer.Transform<string>(opt_pr.GetChild(0));
					auto &arg_opt = opt_pr.Child<OptionalParseResult>(1);
					unique_ptr<ParsedExpression> value_expr;
					if (arg_opt.HasResult()) {
						auto &arg_list = arg_opt.GetResult().Cast<ListParseResult>();
						value_expr = transformer.Transform<unique_ptr<ParsedExpression>>(arg_list.GetChild(1));
					} else {
						value_expr = make_uniq<ConstantExpression>(Value::BOOLEAN(true));
					}
					auto [_, inserted] = result->info->named_parameters.emplace(opt_name, std::move(value_expr));
					if (!inserted) {
						throw InvalidInputException("conflicting or redundant options: \"%s\" specified more than once",
						                            opt_name);
					}
				}
			}
		}
	}

	return std::move(result);
}

// DropSubscriptionStatement <- 'DROP' 'SUBSCRIPTION' IfExists? Identifier
//
// Transforms to:  PRAGMA drop_subscription('sub_name', if_exists)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropSubscriptionStatement(PEGTransformer &transformer,
                                                                                   ParseResult &parse_result) {
	// 0:'DROP' 1:'SUBSCRIPTION' 2:IfExists? 3:Identifier
	auto &list_pr = parse_result.Cast<ListParseResult>();
	bool if_exists = list_pr.Child<OptionalParseResult>(2).HasResult();
	auto &sub_name = list_pr.Child<IdentifierParseResult>(3).identifier;

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "drop_subscription";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(sub_name.GetIdentifierName())));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(if_exists)));
	return std::move(result);
}

// AlterSubscriptionStatement <- 'ALTER' 'SUBSCRIPTION' Identifier SubscriptionAlterAction
// SubscriptionAlterAction    <- SubscriptionEnableAction / SubscriptionSetOptions
// SubscriptionEnableAction   <- 'ENABLE' / 'DISABLE'
// SubscriptionSetOptions     <- 'SET' Parens(List(SubscriptionOption))
//
// Transforms to:  PRAGMA alter_subscription('sub_name', enabled := <bool>)
//                 PRAGMA alter_subscription('sub_name', binary := <bool>)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterSubscriptionStatement(PEGTransformer &transformer,
                                                                                    ParseResult &parse_result) {
	// 0:'ALTER' 1:'SUBSCRIPTION' 2:Identifier 3:SubscriptionAlterAction
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &sub_name = list_pr.Child<IdentifierParseResult>(2).identifier;
	auto &action = list_pr.GetChild(3);
	auto &kw = SubscriptionFirstKeyword(action);
	string action_kw = StringUtil::Upper(string(kw.keyword));

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "alter_subscription";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(sub_name.GetIdentifierName())));

	if (action_kw == "ENABLE" || action_kw == "DISABLE") {
		result->info->named_parameters["enabled"] =
		    make_uniq<ConstantExpression>(Value::BOOLEAN(action_kw == "ENABLE"));
		return std::move(result);
	}

	// Unwrap the SubscriptionAlterAction wrapper (a 1-child list) and the choice
	// down to the matched action rule, then dispatch on the rule name.
	ParseResult *node = &action;
	for (;;) {
		if (node->type == ParseResultType::CHOICE) {
			node = &node->Cast<ChoiceParseResult>().GetResult();
		} else if (node->type == ParseResultType::LIST && node->Cast<ListParseResult>().GetChildren().size() == 1) {
			node = &node->Cast<ListParseResult>().GetChild(0);
		} else {
			break;
		}
	}
	auto &act = node->Cast<ListParseResult>();

	if (act.name == "SubscriptionConnectionAction") {
		// 0:'CONNECTION' 1:StringLiteral
		result->info->named_parameters["connection"] =
		    make_uniq<ConstantExpression>(Value(act.Child<StringLiteralParseResult>(1).result));
	} else if (act.name == "SubscriptionPublicationAction") {
		// 0:SubscriptionPublicationMode 1:'PUBLICATION' 2:List(Identifier)
		auto &mode_kw = SubscriptionFirstKeyword(act.GetChild(0));
		result->info->named_parameters["pub_mode"] =
		    make_uniq<ConstantExpression>(Value(StringUtil::Lower(string(mode_kw.keyword))));
		auto pub_results = ExtractParseResultsFromList(act.Child<ListParseResult>(2));
		string publications_value;
		for (idx_t i = 0; i < pub_results.size(); i++) {
			if (i > 0) {
				publications_value += ",";
			}
			publications_value += pub_results[i].get().Cast<IdentifierParseResult>().identifier.GetIdentifierName();
		}
		result->info->named_parameters["publications"] = make_uniq<ConstantExpression>(Value(publications_value));
	} else if (act.name == "SubscriptionRenameAction") {
		// 0:'RENAME' 1:'TO' 2:Identifier
		result->info->named_parameters["new_name"] =
		    make_uniq<ConstantExpression>(Value(act.Child<IdentifierParseResult>(2).identifier.GetIdentifierName()));
	} else if (act.name == "SubscriptionOwnerAction") {
		// 0:'OWNER' 1:'TO' 2:Identifier
		result->info->named_parameters["owner"] =
		    make_uniq<ConstantExpression>(Value(act.Child<IdentifierParseResult>(2).identifier.GetIdentifierName()));
	} else { // SubscriptionSetOptions: 0:'SET' 1:Parens(List(SubscriptionOption))
		auto &list_inside = ExtractResultFromParens(act.GetChild(1));
		for (auto &opt_ref : ExtractParseResultsFromList(list_inside)) {
			auto &opt_pr = opt_ref.get().Cast<ListParseResult>();
			auto opt_name = transformer.Transform<string>(opt_pr.GetChild(0));
			auto &arg_opt = opt_pr.Child<OptionalParseResult>(1);
			unique_ptr<ParsedExpression> value_expr;
			if (arg_opt.HasResult()) {
				auto &arg_list = arg_opt.GetResult().Cast<ListParseResult>();
				value_expr = transformer.Transform<unique_ptr<ParsedExpression>>(arg_list.GetChild(1));
			} else {
				value_expr = make_uniq<ConstantExpression>(Value::BOOLEAN(true));
			}
			result->info->named_parameters[opt_name] = std::move(value_expr);
		}
	}
	return std::move(result);
}

} // namespace duckdb
