#include "duckdb/formats/copy_from.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"

namespace duckdb {

namespace {

vector<unique_ptr<Expression>> BindDefaults(ClientContext &context, TableCatalogEntry &table,
                                            const vector<Identifier> &names) {
	auto binder = Binder::CreateTableBinder(context, table.ParentSchema(context));
	vector<unique_ptr<Expression>> defaults;
	defaults.reserve(names.size());
	for (auto &name : names) {
		auto &column = table.GetColumn(name);
		if (!column.HasDefaultValue()) {
			defaults.emplace_back();
			continue;
		}
		auto expr = column.DefaultValue().Copy();
		ConstantBinder default_binder(*binder, context, "DEFAULT value");
		default_binder.target_type = column.Type();
		defaults.push_back(default_binder.Bind(expr));
	}
	return defaults;
}

void DuckDBDefaults(CopyInfo &info) {
	auto &options = info.options;
	options.try_emplace("allow_quoted_nulls", vector<Value> {Value::BOOLEAN(false)});
	if (info.is_format_auto_detected || options.contains("auto_detect")) {
		return;
	}
	bool delimiter_given = false;
	for (auto &option : options) {
		delimiter_given |= option.first.StartsWith("delim") || option.first.StartsWith("sep");
	}
	if (!delimiter_given) {
		options.emplace("delimiter", vector<Value> {Value(",")});
	}
	auto quote = options.try_emplace("quote", vector<Value> {Value("\"")}).first->second;
	options.try_emplace("escape", std::move(quote));
	options.try_emplace("comment", vector<Value> {Value("")});
	options.try_emplace("skip", vector<Value> {Value::BIGINT(0)});
	options.try_emplace("header", vector<Value> {Value::BOOLEAN(false)});
}

} // namespace

unique_ptr<FunctionData> PostgresCopyFromBind(ClientContext &context, CopyFromFunctionBindInput &input,
                                              vector<Identifier> &expected_names, vector<LogicalType> &expected_types,
                                              CopyTextFormat format) {
	auto options = ParseCopyFormatOptions(format, true, input.info.options);
	if (!CopyFromKeepsOrder(context, input.info)) {
		input.tf.get_partition_data = nullptr;
		input.tf.order_preservation_type = OrderPreservationType::NO_ORDER;
	}
	if (!options.foreign.empty() || (format == CopyTextFormat::CSV && input.info.is_format_auto_detected)) {
		auto info = input.info.Copy();
		DuckDBDefaults(*info);
		CopyFromFunctionBindInput duckdb_input(*info, input.tf);
		return TableFunctionMultiFileWrapper::MultiFileBindCopy(context, duckdb_input, expected_names, expected_types);
	}
	CheckCopyFormatOptions(options, true);
	auto rules = make_shared_ptr<CopyFromRules>();
	rules->table = input.info.GetQualifiedName().Name().GetIdentifierName();
	rules->names.reserve(expected_names.size());
	for (auto &name : expected_names) {
		rules->names.push_back(name.GetIdentifierName());
	}
	rules->force_not_null = CopyColumnMask(options.force_not_null, rules->names, "FORCE_NOT_NULL");
	rules->force_null = CopyColumnMask(options.force_null, rules->names, "FORCE_NULL");
	if (options.has_default) {
		auto &table = Catalog::GetEntry<TableCatalogEntry>(context, input.info.GetQualifiedName());
		rules->defaults = BindDefaults(context, table, expected_names);
	}
	rules->options = std::move(options);
	return TableFunctionMultiFileWrapper::MultiFileBindCopyRules(context, input, expected_names, expected_types,
	                                                             std::move(rules));
}

bool CopyFromKeepsOrder(ClientContext &context, const CopyInfo &info) {
	const auto table =
	    Catalog::GetEntry<TableCatalogEntry>(context, info.GetQualifiedName(), OnEntryNotFound::RETURN_NULL);
	return table && table->IsDuckTable() && Settings::Get<PreserveInsertionOrderSetting>(context);
}

} // namespace duckdb
