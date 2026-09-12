#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

static string QualifiedNameToDottedString(const QualifiedName &name) {
	string result;
	if (!name.Catalog().empty()) {
		result += name.Catalog();
		result += ".";
	}
	if (!name.Schema().empty()) {
		result += name.Schema();
		result += ".";
	}
	result += name.Name();
	return result;
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateTSDictionaryStatement(PEGTransformer &transformer,
                                                                                     ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	bool if_not_exists = list_pr.Child<OptionalParseResult>(4).HasResult();
	auto qname = transformer.Transform<QualifiedName>(list_pr.Child<ListParseResult>(5));
	auto full_name = QualifiedNameToDottedString(qname);

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "create_text_search_dictionary";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(full_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(if_not_exists)));

	auto &expr_pr = list_pr.Child<ListParseResult>(6);
	auto expr = transformer.Transform<unique_ptr<ParsedExpression>>(expr_pr.GetChild(1));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(expr->ToString())));

	auto &with_opt = expr_pr.Child<OptionalParseResult>(2);
	if (!with_opt.HasResult()) {
		return std::move(result);
	}
	auto &definition_pr = with_opt.GetResult().Cast<ListParseResult>().Child<ListParseResult>(1);
	auto &list_inside = ExtractResultFromParens(definition_pr.GetChild(0));
	auto elements = ExtractParseResultsFromList(list_inside);
	for (auto &elem_ref : elements) {
		auto &elem_pr = elem_ref.get().Cast<ListParseResult>();
		auto opt_name = transformer.Transform<string>(elem_pr.GetChild(0));
		auto &arg_opt = elem_pr.Child<OptionalParseResult>(1);
		unique_ptr<ParsedExpression> value_expr;
		if (arg_opt.HasResult()) {
			auto &arg_list = arg_opt.GetResult().Cast<ListParseResult>();
			value_expr = transformer.Transform<unique_ptr<ParsedExpression>>(arg_list.GetChild(1));
		} else {
			value_expr = make_uniq<ConstantExpression>(Value::BOOLEAN(true));
		}
		auto [_, inserted] = result->info->named_parameters.emplace(opt_name, std::move(value_expr));
		if (!inserted) {
			throw InvalidInputException("conflicting or redundant options: \"%s\" specified more than once", opt_name);
		}
	}
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropTSDictionaryStatement(PEGTransformer &transformer,
                                                                                   ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	bool if_exists = list_pr.Child<OptionalParseResult>(4).HasResult();
	auto qname = transformer.Transform<QualifiedName>(list_pr.Child<ListParseResult>(5));
	auto full_name = QualifiedNameToDottedString(qname);

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "drop_text_search_dictionary";
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value(full_name)));
	result->info->parameters.push_back(make_uniq<ConstantExpression>(Value::BOOLEAN(if_exists)));
	return std::move(result);
}

} // namespace duckdb
