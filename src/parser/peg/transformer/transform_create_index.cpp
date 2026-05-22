#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateIndexStmt(
    PEGTransformer &transformer, const optional<bool> &unique_index, const optional<bool> &if_not_exists,
    const optional<Identifier> &index_name, unique_ptr<BaseTableRef> base_table_name,
    const optional<vector<string>> &insert_column_list, const optional<Identifier> &index_type,
    optional<vector<IndexElementDefinition>> index_element, optional<vector<IncludedColumnDefinition>> include_clause,
    optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> with_list,
    optional<unique_ptr<ParsedExpression>> where_clause) {
	auto result = make_uniq<CreateStatement>();
	auto index_info = make_uniq<CreateIndexInfo>();
	index_info->constraint_type = unique_index ? IndexConstraintType::UNIQUE : IndexConstraintType::NONE;
	index_info->on_conflict =
	    if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	if (!index_name) {
		throw NotImplementedException("Please provide an index name, e.g., CREATE INDEX my_name ...");
	}
	index_info->table = base_table_name->Table();
	index_info->SetQualifiedName(QualifiedName(base_table_name->GetQualifiedName().Catalog(),
	                                           base_table_name->GetQualifiedName().Schema(), *index_name));
	index_info->index_type = index_type ? index_type->GetIdentifierName() : "ART";
	if (insert_column_list) {
		for (auto &column : *insert_column_list) {
			index_info->expressions.push_back(
			    make_uniq<ColumnRefExpression>(Identifier(column), base_table_name->Table()));
			index_info->parsed_expressions.push_back(
			    make_uniq<ColumnRefExpression>(Identifier(column), base_table_name->Table()));
			index_info->column_opclasses.push_back("");
			index_info->column_opclass_options.push_back(std::nullopt);
		}
	}
	if (index_element) {
		for (auto &element : *index_element) {
			if (element.expr->GetExpressionType() == ExpressionType::COLLATE) {
				throw NotImplementedException("Index with collation not supported yet!");
			}
			index_info->column_opclasses.push_back(element.opclass);
			index_info->column_opclass_options.push_back(element.opclass_options);
			index_info->expressions.push_back(element.expr->Copy());
			index_info->parsed_expressions.push_back(std::move(element.expr));
		}
	}
	// INCLUDE clause: store payload-only columns with opclass "included" (or the
	// per-column opclass if specified). The catalog treats "included" as
	// "store but don't tokenize/index".
	if (include_clause) {
		for (auto &included : *include_clause) {
			index_info->expressions.push_back(
			    make_uniq<ColumnRefExpression>(Identifier(included.name), base_table_name->Table()));
			index_info->parsed_expressions.push_back(
			    make_uniq<ColumnRefExpression>(Identifier(included.name), base_table_name->Table()));
			index_info->column_opclasses.push_back(included.opclass);
			index_info->column_opclass_options.push_back(std::move(included.opclass_options));
		}
	}
	if (where_clause) {
		throw NotImplementedException("Creating partial indexes is not supported currently");
	}
	if (with_list) {
		for (auto &option_entry : *with_list) {
			if (option_entry.second->GetExpressionClass() != ExpressionClass::CONSTANT) {
				throw InvalidInputException("Create index option must be a constant value");
			}
			index_info->options[option_entry.first] = option_entry.second->Cast<ConstantExpression>().GetValue();
		}
	}
	result->info = std::move(index_info);
	return result;
}

string PEGTransformerFactory::TransformDottedIdentifierString(PEGTransformer &transformer,
                                                              const vector<string> &dotted_identifier) {
	return StringUtil::Join(dotted_identifier, ".");
}

Identifier PEGTransformerFactory::TransformIndexType(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}

// IndexElement <- Expression IndexOpclass? DescOrAsc? NullsFirstOrLast?
IndexElementDefinition
PEGTransformerFactory::TransformIndexElement(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
                                             optional<IndexOpclassDefinition> index_opclass,
                                             const optional<OrderType> &desc_or_asc,
                                             const optional<OrderByNullType> &nulls_first_or_last) {
	// TODO(Dtenwolde): We currently ignore desc_or_asc and nulls_first_or_last
	IndexElementDefinition result;
	result.expr = std::move(expression);
	if (index_opclass) {
		result.opclass = std::move(index_opclass->name);
		result.opclass_options = std::move(index_opclass->options);
	}
	return result;
}

// IncludedColumn <- ColId IndexOpclass?
// Payload-only columns default to the "included" opclass sentinel; the catalog
// requires that opclass to carry an (at least empty) options map.
IncludedColumnDefinition
PEGTransformerFactory::TransformIncludedColumn(PEGTransformer &transformer, const Identifier &col_id,
                                               optional<IndexOpclassDefinition> index_opclass) {
	IncludedColumnDefinition result;
	result.name = col_id.GetIdentifierName();
	if (index_opclass) {
		result.opclass = std::move(index_opclass->name);
		result.opclass_options = std::move(index_opclass->options);
	} else {
		result.opclass = "included";
		result.opclass_options = case_insensitive_map_t<Value>();
	}
	return result;
}

bool PEGTransformerFactory::TransformUniqueIndex(PEGTransformer &transformer) {
	return true;
}

// IndexOpclass <- Identifier ('.' Identifier)? IndexOpclassOptions?
IndexOpclassDefinition
PEGTransformerFactory::TransformIndexOpclass(PEGTransformer &transformer, const Identifier &identifier,
                                             const optional<Identifier> &identifier_1,
                                             optional<case_insensitive_map_t<Value>> index_opclass_options) {
	IndexOpclassDefinition result;
	result.name = identifier.GetIdentifierName();
	if (identifier_1) {
		result.name += ".";
		result.name += identifier_1->GetIdentifierName();
	}
	result.options = std::move(index_opclass_options);
	return result;
}

// IndexOpclassOptions <- Parens(List(IndexOpclassOption)?)
// nullopt is reserved for "no parens"; here parens are present, so an empty
// list yields an empty map.
case_insensitive_map_t<Value>
PEGTransformerFactory::TransformIndexOpclassOptions(PEGTransformer &transformer,
                                                    const optional<vector<pair<string, Value>>> &index_opclass_option) {
	case_insensitive_map_t<Value> result;
	if (index_opclass_option) {
		for (auto &option : *index_opclass_option) {
			result[StringUtil::Lower(option.first)] = option.second;
		}
	}
	return result;
}

// IndexOpclassOption <- ColLabel ('=' DefArg)?
pair<string, Value> PEGTransformerFactory::TransformIndexOpclassOption(PEGTransformer &transformer,
                                                                       const string &col_label,
                                                                       optional<unique_ptr<ParsedExpression>> def_arg) {
	if (!def_arg) {
		return {col_label, Value::BOOLEAN(true)};
	}
	auto &expr = *def_arg;
	if (expr->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw InvalidInputException("Opclass option must be a constant value");
	}
	return {col_label, expr->Cast<ConstantExpression>().GetValue()};
}

case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformWithList(PEGTransformer &transformer,
                                         case_insensitive_map_t<unique_ptr<ParsedExpression>> rel_option_or_oids) {
	return rel_option_or_oids;
}

case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformRelOptionList(PEGTransformer &transformer,
                                              vector<pair<Identifier, unique_ptr<ParsedExpression>>> rel_option) {
	case_insensitive_map_t<unique_ptr<ParsedExpression>> result;
	for (auto &option : rel_option) {
		result.insert({option.first.GetIdentifierName(), std::move(option.second)});
	}
	return result;
}

// Oids <- WithOrWithoutOids 'OIDS'
case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformOids(PEGTransformer &transformer, const bool &with_or_without_oids) {
	throw NotImplementedException("OIDS for index are not yet implemented.");
}

// WithOids <- 'WITH'
bool PEGTransformerFactory::TransformWithOids(PEGTransformer &transformer) {
	return true;
}

// WithoutOids <- 'WITHOUT'
bool PEGTransformerFactory::TransformWithoutOids(PEGTransformer &transformer) {
	return false;
}

Identifier PEGTransformerFactory::TransformRelOptionName(PEGTransformer &transformer, const string &child) {
	return Identifier(child);
}

pair<Identifier, unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformRelOption(PEGTransformer &transformer, const Identifier &rel_option_name,
                                          optional<unique_ptr<ParsedExpression>> rel_option_argument_opt) {
	if (!rel_option_argument_opt) {
		return {rel_option_name, make_uniq<ConstantExpression>(Value())};
	}
	return {rel_option_name, std::move(*rel_option_argument_opt)};
}

// RelOptionArgumentOpt <- '=' DefArg
unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRelOptionArgumentOpt(PEGTransformer &transformer,
                                                     unique_ptr<ParsedExpression> def_arg) {
	return def_arg;
}

// DefArgNull <- NullLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefArgNull(PEGTransformer &transformer,
                                                                        const Value &null_literal) {
	return make_uniq<ConstantExpression>(Value());
}

// DefArgKeyword <- ReservedKeyword
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefArgKeyword(PEGTransformer &transformer,
                                                                           const string &reserved_keyword) {
	return make_uniq<ConstantExpression>(Value(reserved_keyword));
}

// DefArgStringLiteral <- StringLiteral
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefArgStringLiteral(PEGTransformer &transformer,
                                                                                 const string &string_literal) {
	return make_uniq<ConstantExpression>(Value(string_literal));
}

// NoneLiteral <- 'NONE'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformNoneLiteral(PEGTransformer &transformer) {
	return make_uniq<ConstantExpression>(Value());
}

} // namespace duckdb
