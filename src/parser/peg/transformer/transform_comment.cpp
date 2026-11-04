#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace duckdb {

vector<string>
PEGTransformerFactory::TransformCommentStringLiteralIdentifier(PEGTransformer &transformer,
                                                               const Identifier &string_literal_identifier) {
	return {string_literal_identifier.GetIdentifierName()};
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCommentStatement(PEGTransformer &transformer,
                                                                          const CatalogType &comment_on_type,
                                                                          const vector<string> &comment_target,
                                                                          const Value &comment_value) {
	auto result = make_uniq<AlterStatement>();
	unique_ptr<AlterInfo> info;

	Identifier column_name;
	if (comment_on_type == CatalogType::INVALID) {
		// Column type returned
		column_name = Identifier(comment_target.back());
		if (comment_target.size() == 1) {
			throw ParserException("Invalid column reference: %s", SQLIdentifier(column_name));
		}
		auto qualified_name =
		    StringToQualifiedName(std::span<const string>(comment_target).first(comment_target.size() - 1));
		info = make_uniq<SetColumnCommentInfo>(qualified_name.Catalog(), qualified_name.Schema(), qualified_name.Name(),
		                                       column_name, comment_value, OnEntryNotFound::THROW_EXCEPTION);
	} else if (comment_on_type == CatalogType::DATABASE_ENTRY || comment_on_type == CatalogType::ROLE_ENTRY) {
		if (comment_target.size() != 1) {
			throw ParserException("syntax error at or near \".\"");
		}
		info = make_uniq<SetCommentInfo>(comment_on_type, Identifier(), Identifier(), Identifier(comment_target[0]),
		                                 comment_value, OnEntryNotFound::THROW_EXCEPTION);
	} else if (comment_on_type == CatalogType::SCHEMA_ENTRY) {
		throw NotImplementedException("Adding comments to schemas is not implemented");
	} else {
		auto qualified_name = StringToQualifiedName(comment_target);
		info = make_uniq<SetCommentInfo>(comment_on_type, qualified_name.Catalog(), qualified_name.Schema(),
		                                 qualified_name.Name(), comment_value, OnEntryNotFound::THROW_EXCEPTION);
	}
	if (!info) {
		throw NotImplementedException("Cannot comment on this type");
	}
	result->info = std::move(info);
	return std::move(result);
}

// CommentRoutineStatement <- 'COMMENT' 'ON' GrantRoutineKind RoutineSignature 'IS' CommentValue
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCommentRoutineStatement(PEGTransformer &transformer,
                                                                                 ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &signature = list_pr.Child<ListParseResult>(3);
	auto name = transformer.Transform<QualifiedName>(signature.GetChild(0));
	auto info =
	    make_uniq<SetCommentInfo>(CatalogType::MACRO_ENTRY, name.Catalog(), name.Schema(), name.Name(),
	                              transformer.Transform<Value>(list_pr.GetChild(5)), OnEntryNotFound::THROW_EXCEPTION);
	info->has_func_args =
	    TransformFunctionArgumentTypes(transformer, signature.Child<OptionalParseResult>(1), info->func_parameters);
	auto result = make_uniq<AlterStatement>();
	result->info = std::move(info);
	return std::move(result);
}

CatalogType PEGTransformerFactory::TransformCommentTable(PEGTransformer &transformer) {
	return CatalogType::TABLE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentSequence(PEGTransformer &transformer) {
	return CatalogType::SEQUENCE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentFunction(PEGTransformer &transformer) {
	return CatalogType::MACRO_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentMacroTable(PEGTransformer &transformer) {
	return CatalogType::TABLE_MACRO_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentMacro(PEGTransformer &transformer) {
	return CatalogType::MACRO_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentView(PEGTransformer &transformer) {
	return CatalogType::VIEW_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentDatabase(PEGTransformer &transformer) {
	return CatalogType::DATABASE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentIndex(PEGTransformer &transformer) {
	return CatalogType::INDEX_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentSchema(PEGTransformer &transformer) {
	return CatalogType::SCHEMA_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentType(PEGTransformer &transformer) {
	return CatalogType::TYPE_ENTRY;
}

CatalogType PEGTransformerFactory::TransformCommentColumn(PEGTransformer &transformer) {
	return CatalogType::INVALID;
}

CatalogType PEGTransformerFactory::TransformCommentRole(PEGTransformer &transformer) {
	return CatalogType::ROLE_ENTRY;
}

Value PEGTransformerFactory::TransformStringLiteralValue(PEGTransformer &transformer, const string &string_literal) {
	return Value(string_literal);
}

} // namespace duckdb
