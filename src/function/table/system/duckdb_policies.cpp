#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/policy_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/row_security.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

struct DuckDBPoliciesData : public GlobalTableFunctionState {
	DuckDBPoliciesData() : offset(0) {
	}

	vector<pair<reference<StandardEntry>, reference<PolicyCatalogEntry>>> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBPoliciesBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("policy_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("policy_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("relation_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("relation_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("relation_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("permissive");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("command");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("roles");
	return_types.emplace_back(LogicalType::LIST(LogicalType::BIGINT));

	names.emplace_back("using_expression");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("check_expression");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("temporary");
	return_types.emplace_back(LogicalType::BOOLEAN);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBPoliciesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBPoliciesData>();
	vector<reference<StandardEntry>> relations;
	for (auto &schema : Catalog::GetAllSchemas(context)) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY,
		                  [&](CatalogEntry &entry) { relations.push_back(entry.Cast<StandardEntry>()); });
	}
	for (auto &relation_ref : relations) {
		auto &relation = relation_ref.get();
		auto row_security = RowSecurity::Get(relation);
		if (!row_security) {
			continue;
		}
		auto transaction = CatalogTransaction(relation.ParentCatalog(), context);
		row_security->ScanPolicies(transaction,
		                           [&](PolicyCatalogEntry &policy) { result->entries.emplace_back(relation, policy); });
	}
	return std::move(result);
}

static Value ExpressionValue(const unique_ptr<ParsedExpression> &expr) {
	return expr ? Value(expr->ToString()) : Value();
}

void DuckDBPoliciesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBPoliciesData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &relation = data.entries[data.offset].first.get();
		auto &policy = data.entries[data.offset].second.get();
		data.offset++;

		idx_t col = 0;
		output.SetValue(col++, count, Value(policy.catalog.GetName()));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(policy.catalog.GetOid())));
		output.SetValue(col++, count, Value(policy.ParentSchemaName()));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(policy.ParentSchemaOid())));
		output.SetValue(col++, count, Value(policy.name));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(policy.oid)));
		output.SetValue(col++, count, Value(relation.name));
		output.SetValue(col++, count, Value::BIGINT(NumericCast<int64_t>(relation.oid)));
		output.SetValue(col++, count, Value(relation.type == CatalogType::VIEW_ENTRY ? "VIEW" : "TABLE"));
		output.SetValue(col++, count, Value::BOOLEAN(policy.permissive));
		output.SetValue(col++, count, Value(EnumUtil::ToString(policy.command)));
		vector<Value> roles;
		for (auto role : policy.roles) {
			roles.push_back(Value::BIGINT(NumericCast<int64_t>(role)));
		}
		output.SetValue(col++, count, Value::LIST(LogicalType::BIGINT, std::move(roles)));
		output.SetValue(col++, count, ExpressionValue(policy.using_expr));
		output.SetValue(col++, count, ExpressionValue(policy.check_expr));
		output.SetValue(col++, count, Value::BOOLEAN(policy.temporary));
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBPoliciesFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(
	    TableFunction("duckdb_policies", {}, DuckDBPoliciesFunction, DuckDBPoliciesBind, DuckDBPoliciesInit));
}

} // namespace duckdb
