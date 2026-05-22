#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"

namespace duckdb {

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateTypeStmt(PEGTransformer &transformer,
                                                                           const optional<bool> &if_not_exists,
                                                                           const QualifiedName &qualified_name,
                                                                           unique_ptr<CreateTypeInfo> create_type) {
	auto result = make_uniq<CreateStatement>();
	create_type->CatalogMutable() = qualified_name.Catalog();
	create_type->SchemaMutable() = qualified_name.Schema();
	create_type->SetTypeName(qualified_name.Name());
	create_type->on_conflict =
	    if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	result->info = std::move(create_type);
	return result;
}

unique_ptr<CreateTypeInfo> PEGTransformerFactory::TransformCreateTypeFromType(PEGTransformer &transformer,
                                                                              const LogicalType &type) {
	auto result = make_uniq<CreateTypeInfo>();
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &choice_pr = list_pr.Child<ChoiceParseResult>(0);
	if (choice_pr.GetResult().name == "EnumSelectType") {
		result->query = transformer.Transform<unique_ptr<SelectStatement>>(choice_pr.GetResult());
		result->type = LogicalType::INVALID;
	} else if (choice_pr.GetResult().name == "ColIdTypeList") {
		// PG-compat: `CREATE TYPE foo AS (col type, ...)` is shorthand for
		// `CREATE TYPE foo AS STRUCT(col type, ...)`. PG rejects composite
		// types with duplicate column names — enforce that here, since
		// LogicalType::STRUCT happily accepts duplicates.
		auto cols = transformer.Transform<child_list_t<LogicalType>>(choice_pr.GetResult());
		case_insensitive_set_t seen;
		for (auto &col : cols) {
			if (!seen.insert(col.first).second) {
				throw ParserException("column \"%s\" specified more than once", col.first);
			}
		}
		result->type = LogicalType::STRUCT(cols);
	} else {
		result->type = transformer.Transform<LogicalType>(choice_pr.GetResult());
	}
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
