#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

namespace {

// current_query
void CurrentQueryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	Value val(state.GetContext().GetCurrentQuery());
	result.Reference(val, count_t(input.size()));
}

template <class VISIT>
bool VisitUsableSearchPath(ClientContext &context, const CatalogSearchPath &search_path, const RoleAccess &role,
                           VISIT &&visit) {
	auto current_catalog = DatabaseManager::GetDefaultDatabase(context);
	bool searched_current_catalog = false;
	for (auto &entry : search_path.GetResolvedSetPaths()) {
		if (!entry.GetCatalog().empty() && entry.GetCatalog() != current_catalog) {
			continue;
		}
		searched_current_catalog = true;
		auto schema_entry =
		    Catalog::GetSchema(context, entry.GetCatalog(), entry.GetSchema(), OnEntryNotFound::RETURN_NULL);
		if (schema_entry && CanUseSchema(role, *schema_entry) && !visit(entry.GetSchema())) {
			break;
		}
	}
	return searched_current_catalog;
}

const CatalogSearchPath &SearchPathOf(ClientContext &context, const shared_ptr<CatalogSearchPath> &function_path) {
	return function_path ? *function_path : *ClientData::Get(context).catalog_search_path;
}

struct CurrentSchemaBindData : public FunctionData {
	explicit CurrentSchemaBindData(shared_ptr<CatalogSearchPath> function_path_p)
	    : function_path(std::move(function_path_p)) {
	}

	shared_ptr<CatalogSearchPath> function_path;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CurrentSchemaBindData>(function_path);
	}
	bool Equals(const FunctionData &other_p) const override {
		return function_path == other_p.Cast<CurrentSchemaBindData>().function_path;
	}
};

unique_ptr<FunctionData> CurrentSchemaBind(BindScalarFunctionInput &input) {
	return make_uniq<CurrentSchemaBindData>(input.HasBinder() ? input.GetBinder().EntryRetriever().FunctionSearchPath()
	                                                          : nullptr);
}

// current_schema
// PG-compliant: returns the first schema in the search path that actually
// exists, or NULL if none of them do.
void CurrentSchemaFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &search_path = SearchPathOf(context, func_expr.BindInfo()->Cast<CurrentSchemaBindData>().function_path);
	auto current_catalog = DatabaseManager::GetDefaultDatabase(context);
	bool found = false;
	const bool searched_current_catalog = VisitUsableSearchPath(
	    context, search_path, context.CallerAccess(func_expr.CallerRole()), [&](const Identifier &schema) {
		    result.Reference(Value(schema), count_t(input.size()));
		    found = true;
		    return false;
	    });
	if (found) {
		return;
	}
	// Only when the search path names no schema in the current catalog at all -- an
	// embedded DuckDB rather than a serened session, which always sets one. A search
	// path that DOES name one which happens not to exist must still report NULL, as
	// PostgreSQL does (`SET search_path = nonexistent` -> current_schema() IS NULL),
	// so the fallback must not swallow that case.
	if (!searched_current_catalog) {
		auto default_schema = search_path.GetDefaultSchema(context, current_catalog);
		if (default_schema &&
		    Catalog::GetSchema(context, current_catalog, *default_schema, OnEntryNotFound::RETURN_NULL)) {
			result.Reference(Value(*default_schema), count_t(input.size()));
			return;
		}
	}
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, true);
}

// current_database
void CurrentDatabaseFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	Value val(DatabaseManager::GetDefaultDatabase(state.GetContext()));
	result.Reference(val, count_t(input.size()));
}

struct CurrentSchemasBindData : public FunctionData {
	explicit CurrentSchemasBindData(Value result_value) : result(std::move(result_value)) {
	}

	Value result;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CurrentSchemasBindData>(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CurrentSchemasBindData>();
		return Value::NotDistinctFrom(result, other.result);
	}
};

unique_ptr<FunctionData> CurrentSchemasBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
		throw BinderException("current_schemas requires a boolean input");
	}
	Value schema_value = input.GetConstant(0);
	Value result_val;
	if (schema_value.IsNull()) {
		// null
		result_val = Value(LogicalType::LIST(LogicalType::VARCHAR));
	} else {
		auto implicit_schemas = BooleanValue::Get(schema_value);
		vector<Value> schema_list;
		auto &search_path = SearchPathOf(
		    context, input.HasBinder() ? input.GetBinder().EntryRetriever().FunctionSearchPath() : nullptr);
		// PG-compliant implicit prefix: pg_temp (if the session has any temp
		// objects), then pg_catalog. Then user-set schemas in the current DB.
		if (implicit_schemas) {
			auto temp_db = DatabaseManager::Get(context).GetDatabase(context, TEMP_CATALOG);
			if (temp_db) {
				// Sentinel exception to abort Scan on the first hit. SchemaCatalogEntry::Scan
				// has no early-exit callback, so throw/catch is the cheapest way to bail out.
				struct FoundTempObject : std::exception {};
				bool has_temp_objects = false;
				try {
					for (auto &schema_ref : temp_db->GetCatalog().GetSchemas(context)) {
						schema_ref.get().Scan(context, CatalogType::TABLE_ENTRY, [](CatalogEntry &entry) {
							if (!entry.internal) {
								throw FoundTempObject {};
							}
						});
					}
				} catch (FoundTempObject &) {
					has_temp_objects = true;
				}
				if (has_temp_objects) {
					schema_list.emplace_back("pg_temp");
				}
			}
			auto &set_paths = search_path.GetSetPaths();
			if (std::none_of(set_paths.begin(), set_paths.end(),
			                 [](const CatalogSearchEntry &entry) { return entry.GetSchema() == "pg_catalog"; })) {
				schema_list.emplace_back("pg_catalog");
			}
		}
		VisitUsableSearchPath(context, search_path,
		                      input.HasBinder() ? input.GetBinder().CurrentRole() : context.CurrentRole(),
		                      [&](const Identifier &schema) {
			                      schema_list.emplace_back(schema);
			                      return true;
		                      });
		result_val = Value::LIST(LogicalType::VARCHAR, schema_list);
	}
	return make_uniq<CurrentSchemasBindData>(std::move(result_val));
}

// current_schemas
void CurrentSchemasFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<CurrentSchemasBindData>();
	result.Reference(info.result, count_t(input.size()));
}

// in_search_path
void InSearchPathFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &search_path = ClientData::Get(context).catalog_search_path;
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    input.data[0], input.data[1], result, [&](string_t db_name, string_t schema_name) {
		    return search_path->SchemaInSearchPath(context, Identifier(db_name.GetString()),
		                                           Identifier(schema_name.GetString()));
	    });
}

// txid_current
void TransactionIdCurrent(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &catalog = Catalog::GetCatalog(context, DatabaseManager::GetDefaultDatabase(context));
	auto &transaction = DuckTransaction::Get(context, catalog);
	auto val = Value::UBIGINT(transaction.GetTransactionId() - TRANSACTION_ID_START);
	result.Reference(val, count_t(input.size()));
}

// version
void VersionFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &product_version = DBConfig::GetConfig(state.GetContext()).options.product_version;
	auto val = Value(product_version.empty() ? string(DuckDB::LibraryVersion()) : product_version);
	result.Reference(val, count_t(input.size()));
}

} // namespace

ScalarFunction CurrentQueryFun::GetFunction() {
	ScalarFunction current_query({}, LogicalType::VARCHAR, CurrentQueryFunction);
	current_query.SetStability(FunctionStability::VOLATILE);
	return current_query;
}

ScalarFunction CurrentSchemaFun::GetFunction() {
	ScalarFunction current_schema({}, LogicalType::VARCHAR, CurrentSchemaFunction, CurrentSchemaBind);
	current_schema.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return current_schema;
}

ScalarFunction CurrentDatabaseFun::GetFunction() {
	ScalarFunction current_database({}, LogicalType::VARCHAR, CurrentDatabaseFunction);
	current_database.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return current_database;
}

ScalarFunction CurrentSchemasFun::GetFunction() {
	auto varchar_list_type = LogicalType::LIST(LogicalType::VARCHAR);
	ScalarFunction current_schemas({}, varchar_list_type, CurrentSchemasFunction, CurrentSchemasBind);
	current_schemas.GetSignature().AddParameter("include_implicit", LogicalType::BOOLEAN);
	current_schemas.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return current_schemas;
}

ScalarFunction InSearchPathFun::GetFunction() {
	ScalarFunction in_search_path({}, LogicalType::BOOLEAN, InSearchPathFunction);
	in_search_path.GetSignature()
	    .AddParameter("database_name", LogicalType::VARCHAR)
	    .AddParameter("schema_name", LogicalType::VARCHAR);
	in_search_path.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return in_search_path;
}

ScalarFunction CurrentTransactionIdFun::GetFunction() {
	ScalarFunction txid_current({}, LogicalType::UBIGINT, TransactionIdCurrent);
	txid_current.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return txid_current;
}

ScalarFunction VersionFun::GetFunction() {
	return ScalarFunction({}, LogicalType::VARCHAR, VersionFunction);
}

} // namespace duckdb
