#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

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

	// ServerOptions <- 'OPTIONS' Parens(List(ServerOption))
	auto &options_opt = list_pr.Child<OptionalParseResult>(8);
	if (options_opt.HasResult()) {
		auto &options_list = options_opt.GetResult().Cast<ListParseResult>();
		auto &list_inside = ExtractResultFromParens(options_list.Child<ListParseResult>(1));
		auto elements = ExtractParseResultsFromList(list_inside);
		for (auto &elem_ref : elements) {
			auto &elem_pr = elem_ref.get().Cast<ListParseResult>();
			// ServerOption <- ColLabel StringLiteral. Pass raw children to
			// Transform<string> so it dispatches on each node's actual type
			// (ColLabel resolves a keyword/identifier; StringLiteral a literal).
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
	auto &behavior = list_pr.Child<OptionalParseResult>(4);
	if (behavior.HasResult()) {
		cascade = transformer.Transform<bool>(behavior.GetResult());
	}

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "drop_foreign_server";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(server_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(missing_ok)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(cascade)));
	return std::move(result);
}

} // namespace duckdb
