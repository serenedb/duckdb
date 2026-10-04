#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/parsed_data/create_tokenizer_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateTSDictionaryStatement(PEGTransformer &transformer,
                                                                                     ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto info = make_uniq<CreateTokenizerInfo>();
	info->SetQualifiedName(transformer.Transform<QualifiedName>(list_pr.Child<ListParseResult>(5)));
	info->on_conflict = list_pr.Child<OptionalParseResult>(4).HasResult() ? OnCreateConflict::IGNORE_ON_CONFLICT
	                                                                      : OnCreateConflict::ERROR_ON_CONFLICT;

	auto &expr_pr = list_pr.Child<ListParseResult>(6);
	info->definition = transformer.Transform<unique_ptr<ParsedExpression>>(expr_pr.GetChild(1))->ToString();
	transformer.ClearParameters();

	auto &with_opt = expr_pr.Child<OptionalParseResult>(2);
	if (with_opt.HasResult()) {
		auto &definition_pr = with_opt.GetResult().Cast<ListParseResult>().Child<ListParseResult>(1);
		auto &list_inside = ExtractResultFromParens(definition_pr.GetChild(0));
		for (auto &elem_ref : ExtractParseResultsFromList(list_inside)) {
			auto &elem_pr = elem_ref.get().Cast<ListParseResult>();
			auto opt_name = transformer.Transform<string>(elem_pr.GetChild(0));
			auto &arg_opt = elem_pr.Child<OptionalParseResult>(1);
			unique_ptr<ParsedExpression> value_expr;
			if (arg_opt.HasResult()) {
				auto &arg_list = arg_opt.GetResult().Cast<ListParseResult>();
				value_expr = transformer.Transform<unique_ptr<ParsedExpression>>(arg_list.GetChild(1));
			} else {
				value_expr = ConstantExpression::Boolean(true);
			}
			auto [_, inserted] = info->parsed_options.emplace(opt_name, std::move(value_expr));
			if (!inserted) {
				throw InvalidInputException("conflicting or redundant options: \"%s\" specified more than once",
				                            opt_name);
			}
		}
	}
	auto result = make_uniq<CreateStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropTSDictionaryStatement(PEGTransformer &transformer,
                                                                                   ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	bool if_exists = list_pr.Child<OptionalParseResult>(4).HasResult();
	auto qname = transformer.Transform<QualifiedName>(list_pr.Child<ListParseResult>(5));
	bool cascade = false;
	transformer.TransformOptional<bool>(list_pr, 6, cascade);

	auto result = make_uniq<DropStatement>();
	result->info->type = CatalogType::TOKENIZER_ENTRY;
	result->info->SetQualifiedName(qname);
	result->info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	result->info->cascade = cascade;
	return std::move(result);
}

} // namespace duckdb
