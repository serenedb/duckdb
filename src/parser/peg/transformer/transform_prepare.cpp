#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

bool IsPrepareableStatement(StatementType type) {
	switch (type) {
	case StatementType::SELECT_STATEMENT:
	case StatementType::INSERT_STATEMENT:
	case StatementType::UPDATE_STATEMENT:
	case StatementType::COPY_STATEMENT:
	case StatementType::DELETE_STATEMENT:
	case StatementType::MERGE_INTO_STATEMENT:
		return true;
	default:
		return false;
	}
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformPrepareStatement(PEGTransformer &transformer, const Identifier &identifier,
                                                 const optional<vector<LogicalType>> &type_list,
                                                 unique_ptr<SQLStatement> statement) {
	auto result = make_uniq<PrepareStatement>();
	result->name = identifier;
	if (!IsPrepareableStatement(statement->type)) {
		throw ParserException("%s is not a preparable statement", EnumUtil::ToString(statement->type));
	}
	if (type_list) {
		const bool named = transformer.last_param_type == PreparedParamType::NAMED;
		for (idx_t i = 0; i < type_list->size(); i++) {
			auto number = to_string(i + 1);
			if (!named) {
				statement->named_param_map.emplace(Identifier(number), i + 1);
			}
			result->parameter_type_hints.emplace(std::move(number), (*type_list)[i]);
		}
	}
	result->statement = std::move(statement);
	transformer.ClearParameters();
	return std::move(result);
}

vector<LogicalType> PEGTransformerFactory::TransformTypeList(PEGTransformer &transformer,
                                                             const vector<LogicalType> &type) {
	return type;
}

} // namespace duckdb
