#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/parsed_data/create_foreign_server_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// ServerOptions <- 'OPTIONS' Parens(List(ServerOption)) — the optional child at `child_idx`.
static void TransformServerOptionsInto(PEGTransformer &transformer, ListParseResult &list_pr, idx_t child_idx,
                                       case_insensitive_map_t<string> &options) {
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
		auto opt_name = StringUtil::Lower(transformer.Transform<string>(elem_pr.GetChild(0)));
		auto opt_value = transformer.Transform<string>(elem_pr.GetChild(1));
		auto [_, inserted] = options.emplace(opt_name, std::move(opt_value));
		if (!inserted) {
			throw InvalidInputException("conflicting or redundant options: \"%s\" specified more than once", opt_name);
		}
	}
}

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
	auto info = make_uniq<CreateForeignServerInfo>();
	info->SetName(Identifier(transformer.Transform<string>(list_pr.GetChild(3))));
	// The fdw name is a plain Identifier leaf (not a choice/list rule).
	info->fdw_name = list_pr.Child<IdentifierParseResult>(7).identifier;
	info->on_conflict = list_pr.Child<OptionalParseResult>(2).HasResult() ? OnCreateConflict::IGNORE_ON_CONFLICT
	                                                                      : OnCreateConflict::ERROR_ON_CONFLICT;
	TransformServerOptionsInto(transformer, list_pr, 8, info->options);
	auto result = make_uniq<CreateStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropServerStatement(PEGTransformer &transformer,
                                                                             ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// Children:
	//   0: 'DROP', 1: 'SERVER'
	//   2: IfExists?
	//   3: ColId (server name)
	//   4: DropBehavior? (CASCADE / RESTRICT; RESTRICT/absent = false)
	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::FOREIGN_SERVER_ENTRY;
	result->info->SetQualifiedName(Identifier(), Identifier(),
	                               Identifier(transformer.Transform<string>(list_pr.GetChild(3))));
	result->info->if_not_found = list_pr.Child<OptionalParseResult>(2).HasResult() ? OnEntryNotFound::RETURN_NULL
	                                                                               : OnEntryNotFound::THROW_EXCEPTION;
	transformer.TransformOptional<bool>(list_pr, 4, result->info->cascade);
	return std::move(result);
}

} // namespace duckdb
