#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropStatement(PEGTransformer &transformer,
                                                                       unique_ptr<DropStatement> drop_entries,
                                                                       const optional<bool> &drop_behavior) {
	if (drop_behavior) {
		drop_entries->info->cascade = *drop_behavior;
		for (auto &target : drop_entries->info->additional_drops) {
			target->cascade = *drop_behavior;
		}
	}
	return std::move(drop_entries);
}

unique_ptr<DropStatement> PEGTransformerFactory::MakeDropStatement(unique_ptr<DropInfo> info,
                                                                   vector<QualifiedName> names) {
	for (idx_t i = 1; i < names.size(); i++) {
		auto target = info->Copy();
		target->SetQualifiedName(std::move(names[i]));
		info->additional_drops.push_back(std::move(target));
	}
	info->SetQualifiedName(std::move(names[0]));
	auto result = make_uniq<DropStatement>();
	result->info = std::move(info);
	return result;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropTable(PEGTransformer &transformer,
                                                                    const CatalogType &table_or_view,
                                                                    const optional<bool> &if_exists,
                                                                    vector<unique_ptr<BaseTableRef>> base_table_name) {
	auto info = make_uniq<DropInfo>();
	info->type = table_or_view;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	vector<QualifiedName> names;
	for (auto &base_table : base_table_name) {
		names.push_back(base_table->GetQualifiedName());
	}
	return MakeDropStatement(std::move(info), std::move(names));
}

CatalogType PEGTransformerFactory::TransformMaterializedViewEntry(PEGTransformer &transformer) {
	throw NotImplementedException("Cannot drop MATERIALIZED VIEW yet");
}

bool PEGTransformerFactory::TransformFunctionTypeMacroKeyword(PEGTransformer &transformer) {
	return false;
}

bool PEGTransformerFactory::TransformFunctionTypeFunction(PEGTransformer &transformer) {
	return false;
}

bool PEGTransformerFactory::TransformFunctionTypeProcedure(PEGTransformer &transformer) {
	return true;
}

unique_ptr<DropStatement>
PEGTransformerFactory::TransformDropTableFunction(PEGTransformer &transformer, const CatalogType &comment_macro_table,
                                                  const optional<bool> &if_exists,
                                                  const vector<QualifiedName> &qualified_name) {
	auto info = make_uniq<DropInfo>();
	info->type = comment_macro_table;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	return MakeDropStatement(std::move(info), qualified_name);
}

// DropFunction <- FunctionTypeMacro IfExists? List(DropFunctionItem)
// DropFunctionItem <- FunctionIdentifier DropFunctionArgs?
// DropFunctionArgs <- '(' List(Type)? ')'
unique_ptr<DropStatement> PEGTransformerFactory::TransformDropFunction(PEGTransformer &transformer,
                                                                       ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto result = make_uniq<DropStatement>();
	// FunctionTypeMacro forwards a bool meaning is_procedure (PROCEDURE vs FUNCTION/MACRO).
	const bool is_procedure = transformer.Transform<bool>(list_pr.Child<ListParseResult>(0));
	bool if_exists = list_pr.Child<OptionalParseResult>(1).HasResult();
	auto function_list = ExtractParseResultsFromList(list_pr.Child<ListParseResult>(2));
	for (idx_t i = 0; i < function_list.size(); i++) {
		auto info = make_uniq<DropInfo>();
		info->type = CatalogType::MACRO_ENTRY;
		info->is_procedure = is_procedure;
		auto &item_pr = function_list[i].get().Cast<ListParseResult>();
		auto function = transformer.Transform<QualifiedName>(item_pr.Child<ListParseResult>(0));
		info->SetQualifiedName(function);
		auto &args_opt = item_pr.Child<OptionalParseResult>(1);
		if (args_opt.HasResult()) {
			info->has_func_args = true;
			auto &args_list = args_opt.GetResult().Cast<ListParseResult>();
			// args_list children: '(', List(Type)?, ')'
			auto &inner_opt = args_list.Child<OptionalParseResult>(1);
			if (inner_opt.HasResult()) {
				auto type_results = ExtractParseResultsFromList(inner_opt.GetResult());
				for (auto type_ref : type_results) {
					info->func_parameters.push_back(transformer.Transform<LogicalType>(type_ref));
				}
			}
		}
		info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
		if (i == 0) {
			result->info = std::move(info);
		} else {
			result->info->additional_drops.push_back(std::move(info));
		}
	}
	return result;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropSchema(PEGTransformer &transformer,
                                                                     const optional<bool> &if_exists,
                                                                     const vector<QualifiedName> &qualified_name) {
	auto info = make_uniq<DropInfo>();
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::SCHEMA_ENTRY;
	return MakeDropStatement(std::move(info), qualified_name);
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropIndex(PEGTransformer &transformer,
                                                                    const optional<bool> &if_exists,
                                                                    const vector<QualifiedName> &qualified_index_name) {
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::INDEX_ENTRY;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	return MakeDropStatement(std::move(info), qualified_index_name);
}

QualifiedName PEGTransformerFactory::TransformQualifiedIndexNameString(PEGTransformer &transformer,
                                                                       const Identifier &index_name) {
	QualifiedName result(index_name);
	return result;
}

QualifiedName PEGTransformerFactory::TransformSchemaReservedIndex(PEGTransformer &transformer,
                                                                  const Identifier &schema_qualification,
                                                                  const Identifier &reserved_index_name) {
	QualifiedName result({schema_qualification}, reserved_index_name);
	return result;
}

QualifiedName PEGTransformerFactory::TransformCatalogReservedSchemaIndex(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const Identifier &reserved_schema_qualification, const Identifier &reserved_index_name) {
	QualifiedName result(catalog_qualification, reserved_schema_qualification, reserved_index_name);
	return result;
}

unique_ptr<DropStatement>
PEGTransformerFactory::TransformDropSequence(PEGTransformer &transformer, const optional<bool> &if_exists,
                                             const vector<QualifiedName> &qualified_sequence_name) {
	auto info = make_uniq<DropInfo>();
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::SEQUENCE_ENTRY;
	return MakeDropStatement(std::move(info), qualified_sequence_name);
}

Identifier PEGTransformerFactory::TransformCollationName(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropCollation(PEGTransformer &transformer,
                                                                        const optional<bool> &if_exists,
                                                                        const vector<Identifier> &collation_name) {
	throw NotImplementedException("Cannot drop collation yet");
	/*
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	if (collation_name.size() > 1) {
	    throw NotImplementedException("Can only drop one object at a time");
	}
	auto collation = collation_name[0];
	info->catalog = INVALID_CATALOG;
	info->schema = INVALID_SCHEMA;
	info->name = collation;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::COLLATION_ENTRY;
	result->info = std::move(info);
	return result;
	*/
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropType(PEGTransformer &transformer,
                                                                   const optional<bool> &if_exists,
                                                                   const vector<QualifiedName> &qualified_type_name) {
	auto info = make_uniq<DropInfo>();
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->type = CatalogType::TYPE_ENTRY;
	return MakeDropStatement(std::move(info), qualified_type_name);
}

bool PEGTransformerFactory::TransformCascadeDropBehavior(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformRestrictDropBehavior(PEGTransformer &transformer) {
	return false;
}

bool PEGTransformerFactory::TransformIfExists(PEGTransformer &transformer) {
	return true;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropSecret(PEGTransformer &transformer,
                                                                     const optional<SecretPersistType> &temporary,
                                                                     const optional<bool> &if_exists,
                                                                     const Identifier &secret_name,
                                                                     const optional<Identifier> &drop_secret_storage) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::SECRET_ENTRY;
	auto extra_drop_info = make_uniq<ExtraDropSecretInfo>();
	if (temporary) {
		extra_drop_info->persist_mode = *temporary;
	}

	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	info->SetName(secret_name);
	if (drop_secret_storage) {
		extra_drop_info->secret_storage = drop_secret_storage->GetIdentifierName();
	}
	info->extra_drop_info = std::move(extra_drop_info);
	result->info = std::move(info);
	return result;
}

Identifier PEGTransformerFactory::TransformDropSecretStorage(PEGTransformer &transformer,
                                                             const Identifier &identifier) {
	return identifier;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropTrigger(PEGTransformer &transformer,
                                                                      const optional<bool> &if_exists,
                                                                      const Identifier &trigger_name,
                                                                      unique_ptr<BaseTableRef> base_table_name) {
	auto result = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::TRIGGER_ENTRY;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;

	info->SetName(trigger_name);

	auto extra_info = make_uniq<ExtraDropTriggerInfo>();
	extra_info->base_table = std::move(base_table_name);
	info->extra_drop_info = std::move(extra_info);

	result->info = std::move(info);
	return result;
}

unique_ptr<DropStatement> PEGTransformerFactory::TransformDropJob(PEGTransformer &transformer,
                                                                  const optional<bool> &if_exists,
                                                                  const vector<QualifiedName> &qualified_name) {
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::JOB_ENTRY;
	info->if_not_found = if_exists ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
	return MakeDropStatement(std::move(info), qualified_name);
}

} // namespace duckdb
