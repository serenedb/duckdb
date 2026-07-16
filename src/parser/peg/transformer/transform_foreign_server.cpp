#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// ServerOptions <- 'OPTIONS' Parens(List(ServerOption)) — the optional child at `child_idx`,
// shared by CREATE SERVER and CREATE USER MAPPING. Emplaces each option into `info.named_parameters`.
static void TransformServerOptionsInto(PEGTransformer &transformer, ListParseResult &list_pr, idx_t child_idx,
                                       PragmaInfo &info) {
	auto &options_opt = list_pr.Child<OptionalParseResult>(child_idx);
	if (!options_opt.HasResult()) {
		return;
	}
	auto &options_list = options_opt.GetResult().Cast<ListParseResult>();
	auto &list_inside = PEGTransformerFactory::ExtractResultFromParens(options_list.Child<ListParseResult>(1));
	auto elements = PEGTransformerFactory::ExtractParseResultsFromList(list_inside);
	for (auto &elem_ref : elements) {
		auto &elem_pr = elem_ref.get().Cast<ListParseResult>();
		// ServerOption <- ColLabel StringLiteral. Pass raw children to
		// Transform<string> so it dispatches on each node's actual type
		// (ColLabel resolves a keyword/identifier; StringLiteral a literal).
		auto opt_name = transformer.Transform<string>(elem_pr.GetChild(0));
		auto opt_value = transformer.Transform<string>(elem_pr.GetChild(1));
		auto value_expr = make_uniq<ConstantExpression>(Value(opt_value));
		auto [_, inserted] = info.named_parameters.emplace(opt_name, std::move(value_expr));
		if (!inserted) {
			throw InvalidInputException("conflicting or redundant options: \"%s\" specified more than once", opt_name);
		}
	}
}

// UserMappingRole <- 'PUBLIC' / 'CURRENT_USER' / 'CURRENT_ROLE' / 'SESSION_USER' / 'USER' / ColId
// The special role keywords are leaves; emit the lowered keyword text so the handler
// can recognise them (Transform<string> would return the raw-cased keyword text).
// Anything else is a ColId identifier. The node always materialises as
// LIST -> CHOICE -> leaf, so unwrap those wrappers before classifying.
static string TransformUserMappingRole(ParseResult &role_result) {
	reference<ParseResult> node = role_result;
	for (;;) {
		auto &current = node.get();
		if (current.type == ParseResultType::CHOICE) {
			node = current.Cast<ChoiceParseResult>().GetResult();
			continue;
		}
		if (current.type == ParseResultType::LIST) {
			auto children = current.Cast<ListParseResult>().GetChildren();
			if (children.size() != 1) {
				throw InternalException("unexpected UserMappingRole parse shape");
			}
			node = children[0];
			continue;
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
	throw InternalException("unexpected UserMappingRole parse shape");
}

// CREATE SERVER [IF NOT EXISTS] name FOREIGN DATA WRAPPER fdw OPTIONS (k 'v', ...)
//   -> PRAGMA create_foreign_server('name', 'fdw', if_not_exists, k := 'v', ...)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateServerStatement(PEGTransformer &transformer,
                                                                               ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// Children:
	//   0: 'CREATE'
	//   1: 'SERVER'
	//   2: IfNotExists?
	//   3: ColId (server name -- a bare identifier, PG-style)
	//   4: 'FOREIGN'
	//   5: 'DATA'?
	//   6: 'WRAPPER'
	//   7: Identifier (fdw name)
	//   8: ServerOptions?
	bool if_not_exists = list_pr.Child<OptionalParseResult>(2).HasResult();
	auto server_name = transformer.Transform<string>(list_pr.GetChild(3));
	// The fdw name is a plain Identifier leaf (not a choice/list rule).
	auto fdw_name = list_pr.Child<IdentifierParseResult>(7).identifier;

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "create_foreign_server";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(server_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(fdw_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(if_not_exists)));
	TransformServerOptionsInto(transformer, list_pr, 8, *result->info);
	return std::move(result);
}

// DROP SERVER [IF EXISTS] name -> PRAGMA drop_foreign_server('name', missing_ok)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropServerStatement(PEGTransformer &transformer,
                                                                             ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// Children:
	//   0: 'DROP', 1: 'SERVER'
	//   2: IfExists?
	//   3: ColId (server name)
	//   4: DropBehavior? (CASCADE / RESTRICT; RESTRICT/absent = false)
	bool missing_ok = list_pr.Child<OptionalParseResult>(2).HasResult();
	auto server_name = transformer.Transform<string>(list_pr.GetChild(3));
	bool cascade = false;
	transformer.TransformOptional<bool>(list_pr, 4, cascade);

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "drop_foreign_server";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(server_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(missing_ok)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(cascade)));
	return std::move(result);
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
	auto role = TransformUserMappingRole(list_pr.GetChild(5));
	auto server_name = transformer.Transform<string>(list_pr.GetChild(7));

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "create_user_mapping";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(role)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(server_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(if_not_exists)));
	TransformServerOptionsInto(transformer, list_pr, 8, *result->info);
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
	auto role = TransformUserMappingRole(list_pr.GetChild(5));
	auto server_name = transformer.Transform<string>(list_pr.GetChild(7));

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "drop_user_mapping";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(role)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(server_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(missing_ok)));
	return std::move(result);
}

} // namespace duckdb
