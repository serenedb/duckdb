#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/statement/call_statement.hpp"
#include "duckdb/parser/statement/execute_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformExecuteJobStatement(PEGTransformer &transformer,
                                                                             const QualifiedName &qualified_name) {
	vector<unique_ptr<ParsedExpression>> children;
	children.emplace_back(make_uniq<ConstantExpression>(Value(qualified_name.ToString())));
	auto function = make_uniq<FunctionExpression>("execute_job", std::move(children));
	function->SetQualifiedName(
	    QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), function->GetQualifiedName().Name()));
	auto result = make_uniq<CallStatement>();
	result->function = std::move(function);
	return std::move(result);
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformExecuteStatement(PEGTransformer &transformer, const Identifier &identifier,
                                                 optional<vector<FunctionArgument>> table_function_arguments) {
	auto result = make_uniq<ExecuteStatement>();
	result->name = identifier;
	if (!table_function_arguments) {
		return std::move(result);
	}
	idx_t param_idx = 0;
	auto &arguments = *table_function_arguments;
	for (idx_t i = 0; i < arguments.size(); i++) {
		auto &arg = arguments[i];
		if (!arguments[i].GetExpression().IsScalar()) {
			throw InvalidInputException("Only scalar parameters, named parameters or NULL supported for EXECUTE");
		}
		if (!arguments[i].GetName().empty() && param_idx != 0) {
			throw NotImplementedException("Mixing named parameters and positional parameters is not supported yet");
		}
		auto param_name = arg.GetName();
		if (arguments[i].GetName().empty()) {
			param_name = Identifier(std::to_string(param_idx + 1));
			if (param_idx != i) {
				throw NotImplementedException("Mixing named parameters and positional parameters is not supported yet");
			}
			param_idx++;
		}
		arg.GetExpressionMutable()->ClearAlias();
		result->named_values[param_name] = std::move(arg.GetExpressionMutable());
	}
	return std::move(result);
}
} // namespace duckdb
