#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"

namespace duckdb {

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateTypeStmt(PEGTransformer &transformer,
                                                                           const optional<bool> &if_not_exists,
                                                                           const QualifiedName &qualified_name,
                                                                           unique_ptr<CreateTypeInfo> create_type) {
	auto result = make_uniq<CreateStatement>();
	create_type->SetQualifiedName(qualified_name);
	create_type->on_conflict =
	    if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	result->info = std::move(create_type);
	return result;
}

unique_ptr<CreateTypeInfo> PEGTransformerFactory::TransformCreateTypeFromType(PEGTransformer &transformer,
                                                                              const LogicalType &type) {
	auto result = make_uniq<CreateTypeInfo>();
	result->type = type;
	return result;
}

unique_ptr<CreateTypeInfo>
PEGTransformerFactory::TransformCreateTypeComposite(PEGTransformer &transformer,
                                                    const child_list_t<LogicalType> &col_id_type_list) {
	auto result = make_uniq<CreateTypeInfo>();
	identifier_set_t seen;
	vector<unique_ptr<ParsedExpression>> struct_children;
	for (auto &col : col_id_type_list) {
		if (!seen.insert(col.first).second) {
			throw ParserException("column \"%s\" specified more than once", col.first.GetIdentifierName());
		}
		auto &type_expr = UnboundType::GetTypeExpression(col.second);
		auto new_type_expr = type_expr->Copy();
		new_type_expr->SetAlias(col.first);
		struct_children.push_back(std::move(new_type_expr));
	}
	result->type = LogicalType::UNBOUND(make_uniq<TypeExpression>(Identifier("STRUCT"), std::move(struct_children)));
	return result;
}

unique_ptr<CreateTypeInfo>
PEGTransformerFactory::TransformEnumSelectType(PEGTransformer &transformer,
                                               unique_ptr<SelectStatement> select_statement_internal) {
	auto result = make_uniq<CreateTypeInfo>();
	result->query = std::move(select_statement_internal);
	result->type = LogicalType::INVALID;
	return result;
}

unique_ptr<CreateTypeInfo>
PEGTransformerFactory::TransformEnumStringLiteralList(PEGTransformer &transformer,
                                                      const optional<vector<string>> &string_literal) {
	auto result = make_uniq<CreateTypeInfo>();
	idx_t enum_count = string_literal ? string_literal->size() : 0;
	Vector enum_vector(LogicalType::VARCHAR, enum_count);
	auto string_data = FlatVector::Writer<string_t>(enum_vector, enum_count);
	if (string_literal) {
		for (auto &literal : *string_literal) {
			string_data.WriteValue(string_t(literal));
		}
	}
	result->type = LogicalType::ENUM(enum_vector, enum_count);
	return result;
}

} // namespace duckdb
