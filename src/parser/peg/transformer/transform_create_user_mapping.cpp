#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// UserMappingRole <- 'PUBLIC' / 'CURRENT_USER' / 'CURRENT_ROLE' / 'SESSION_USER' / 'USER' / ColId
// The special role keywords are leaves (emit the lowered keyword text so the
// handler can recognise them); anything else is a ColId and is transformed via
// Transform<string>. The referenced rule can materialise as
// a plain leaf, a single-child List, or a Choice depending on how the framework
// inlines the matched alternative, so unwrap those wrappers before classifying.
static string TransformUserMappingRole(PEGTransformer &transformer, ParseResult &role_result) {
	reference<ParseResult> node = role_result;
	for (;;) {
		auto &current = node.get();
		if (current.type == ParseResultType::CHOICE) {
			node = current.Cast<ChoiceParseResult>().GetResult();
			continue;
		}
		if (current.type == ParseResultType::LIST) {
			auto &list = current.Cast<ListParseResult>();
			auto children = list.GetChildren();
			// A UserMappingRole/ColLabel reference with a single child is just a
			// wrapper around the matched alternative — descend into it. A multi
			// child list is a real rule body that Transform<string> handles.
			if (children.size() == 1) {
				node = children[0];
				continue;
			}
			break;
		}
		break;
	}

	auto &matched = node.get();
	if (matched.type == ParseResultType::KEYWORD) {
		return StringUtil::Lower(matched.Cast<KeywordParseResult>().keyword);
	}
	if (matched.type == ParseResultType::IDENTIFIER) {
		return matched.Cast<IdentifierParseResult>().identifier;
	}
	// ColLabel / QualifiedName style nodes resolve through the registered string
	// transformer.
	return transformer.Transform<string>(matched);
}

// CREATE USER MAPPING [IF NOT EXISTS] FOR role SERVER name OPTIONS (k 'v', ...)
//   -> PRAGMA create_user_mapping('role', 'name', if_not_exists, k := 'v', ...)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateUserMappingStatement(PEGTransformer &transformer,
                                                                                    ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// Children:
	//   0: 'CREATE'
	//   1: 'USER'
	//   2: 'MAPPING'
	//   3: IfNotExists?
	//   4: 'FOR'
	//   5: UserMappingRole
	//   6: 'SERVER'
	//   7: ColId (server name -- a bare identifier, PG-style)
	//   8: ServerOptions?
	bool if_not_exists = list_pr.Child<OptionalParseResult>(3).HasResult();
	auto role = TransformUserMappingRole(transformer, list_pr.GetChild(5));
	auto server_name = transformer.Transform<string>(list_pr.GetChild(7));

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "create_user_mapping";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(role)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(server_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(if_not_exists)));

	// ServerOptions <- 'OPTIONS' Parens(List(ServerOption))
	auto &options_opt = list_pr.Child<OptionalParseResult>(8);
	if (options_opt.HasResult()) {
		auto &options_list = options_opt.GetResult().Cast<ListParseResult>();
		auto &list_inside = ExtractResultFromParens(options_list.Child<ListParseResult>(1));
		auto elements = ExtractParseResultsFromList(list_inside);
		for (auto &elem_ref : elements) {
			auto &elem_pr = elem_ref.get().Cast<ListParseResult>();
			// ServerOption <- ColLabel StringLiteral. Pass raw children to
			// Transform<string> so it dispatches on each node's actual type.
			auto opt_name = transformer.Transform<string>(elem_pr.GetChild(0));
			auto opt_value = transformer.Transform<string>(elem_pr.GetChild(1));
			auto value_expr = make_uniq<ConstantExpression>(Value(opt_value));
			auto [_, inserted] = result->info->named_parameters.emplace(opt_name, std::move(value_expr));
			if (!inserted) {
				throw InvalidInputException("conflicting or redundant options: \"%s\" specified more than once",
				                            opt_name);
			}
		}
	}

	return std::move(result);
}

// DROP USER MAPPING [IF EXISTS] FOR role SERVER name
//   -> PRAGMA drop_user_mapping('role', 'name', missing_ok)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropUserMappingStatement(PEGTransformer &transformer,
                                                                                  ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// Children:
	//   0: 'DROP'
	//   1: 'USER'
	//   2: 'MAPPING'
	//   3: IfExists?
	//   4: 'FOR'
	//   5: UserMappingRole
	//   6: 'SERVER'
	//   7: ColId (server name)
	bool missing_ok = list_pr.Child<OptionalParseResult>(3).HasResult();
	auto role = TransformUserMappingRole(transformer, list_pr.GetChild(5));
	auto server_name = transformer.Transform<string>(list_pr.GetChild(7));

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "drop_user_mapping";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(role)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(server_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(missing_ok)));
	return std::move(result);
}

} // namespace duckdb
