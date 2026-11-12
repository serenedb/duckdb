#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

static bool SubstituteDatabaseName(unique_ptr<Expression> &expr, const ColumnBinding &database_column,
                                   const Value &name) {
	if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		if (expr->Cast<BoundColumnRefExpression>().Binding() != database_column) {
			return false;
		}
		expr = make_uniq<BoundConstantExpression>(name);
		return true;
	}
	bool only_database_name = true;
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		only_database_name = SubstituteDatabaseName(child, database_column, name) && only_database_name;
	});
	return only_database_name;
}

static bool IsDatabaseFilter(const Expression &filter, const ColumnBinding &database_column) {
	auto substituted = filter.Copy();
	return SubstituteDatabaseName(substituted, database_column, Value(LogicalType::VARCHAR)) &&
	       substituted->IsScalar() && substituted->IsFoldable();
}

unique_ptr<FunctionData> DuckDBSystemIncludeHiddenBindData::Copy() const {
	auto result = make_uniq<DuckDBSystemIncludeHiddenBindData>();
	result->include_hidden = include_hidden;
	for (auto &filter : database_filters) {
		result->database_filters.push_back(filter->Copy());
	}
	result->database_column = database_column;
	return std::move(result);
}

bool DuckDBSystemIncludeHiddenBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<DuckDBSystemIncludeHiddenBindData>();
	return include_hidden == other.include_hidden && database_column == other.database_column &&
	       Expression::ListEquals(database_filters, other.database_filters);
}

void DuckDBSystemIncludeHiddenBindData::PushdownDatabaseFilters(ClientContext &context, LogicalGet &get,
                                                                FunctionData *bind_data_p,
                                                                vector<unique_ptr<Expression>> &filters) {
	auto &column_ids = get.GetColumnIds();
	optional_idx database_column;
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto &column = column_ids[i];
		if (!column.IsVirtualColumn() && get.names[column.GetPrimaryIndex()].GetIdentifierName() == "database_name") {
			database_column = i;
			break;
		}
	}
	if (!database_column.IsValid()) {
		return;
	}
	auto &bind_data = bind_data_p->Cast<DuckDBSystemIncludeHiddenBindData>();
	bind_data.database_column = ColumnBinding(get.table_index, ProjectionIndex(database_column.GetIndex()));
	for (auto &filter : filters) {
		if (IsDatabaseFilter(*filter, bind_data.database_column)) {
			bind_data.database_filters.push_back(filter->Copy());
		}
	}
}

std::function<bool(AttachedDatabase &)>
DuckDBSystemIncludeHiddenBindData::DatabaseFilter(ClientContext &context) const {
	if (database_filters.empty()) {
		return nullptr;
	}
	return [this, &context](AttachedDatabase &database) {
		const Value name(database.GetName());
		for (auto &filter : database_filters) {
			auto substituted = filter->Copy();
			SubstituteDatabaseName(substituted, database_column, name);
			Value result;
			if (ExpressionExecutor::TryEvaluateScalar(context, *substituted, result) &&
			    (result.IsNull() || !result.GetValue<bool>())) {
				return false;
			}
		}
		return true;
	};
}

void BuiltinFunctions::RegisterSQLiteFunctions() {
	PragmaVersion::RegisterFunction(*this);
	PragmaPlatform::RegisterFunction(*this);
	PragmaCollations::RegisterFunction(*this);
	PragmaTableInfo::RegisterFunction(*this);
	PragmaStorageInfo::RegisterFunction(*this);
	PragmaMetadataInfo::RegisterFunction(*this);
	PragmaDatabaseSize::RegisterFunction(*this);
	PragmaUserAgent::RegisterFunction(*this);

	DuckDBConnectionCountFun::RegisterFunction(*this);
	DuckDBApproxDatabaseCountFun::RegisterFunction(*this);
	DuckDBColumnsFun::RegisterFunction(*this);
	DuckDBConstraintsFun::RegisterFunction(*this);
	DuckDBCoordinateSystemsFun::RegisterFunction(*this);
	DuckDBDatabasesFun::RegisterFunction(*this);
	DuckDBFunctionsFun::RegisterFunction(*this);
	DuckDBKeywordsFun::RegisterFunction(*this);
	DuckDBPreparedStatementsFun::RegisterFunction(*this);
	DuckDBLogFun::RegisterFunction(*this);
	DuckDBLogContextFun::RegisterFunction(*this);
	DuckDBIndexesFun::RegisterFunction(*this);
	DuckDBSchemasFun::RegisterFunction(*this);
	DuckDBDependenciesFun::RegisterFunction(*this);
	DuckDBExtensionsFun::RegisterFunction(*this);
	DuckDBExtensionRepositoriesFun::RegisterFunction(*this);
	RegisterExternalResourceTypeFun::RegisterFunction(*this);
	CreateExternalResourceFun::RegisterFunction(*this);
	DestroyExternalResourceFun::RegisterFunction(*this);
	RegisterExternalResourceFun::RegisterFunction(*this);
	DeregisterExternalResourceFun::RegisterFunction(*this);
	DuckDBExternalResourceTypesFun::RegisterFunction(*this);
	DuckDBExternalResourcesFun::RegisterFunction(*this);
	DuckDBMemoryFun::RegisterFunction(*this);
	DuckDBEvictionQueuesFun::RegisterFunction(*this);
	DuckDBExternalFileCacheFun::RegisterFunction(*this);
	DuckDBMetricsFun::RegisterFunction(*this);
	DuckDBOptimizersFun::RegisterFunction(*this);
	DuckDBSecretsFun::RegisterFunction(*this);
	DuckDBWhichSecretFun::RegisterFunction(*this);
	DuckDBSecretTypeParametersFun::RegisterFunction(*this);
	DuckDBSecretTypesFun::RegisterFunction(*this);
	DuckDBSequencesFun::RegisterFunction(*this);
	DuckDBTriggersFun::RegisterFunction(*this);
	DuckDBSettingsFun::RegisterFunction(*this);
	DuckDBTablesFun::RegisterFunction(*this);
	DuckDBTableSample::RegisterFunction(*this);
	DuckDBTemporaryFilesFun::RegisterFunction(*this);
	DuckDBTypesFun::RegisterFunction(*this);
	DuckDBVariablesFun::RegisterFunction(*this);
	DuckDBViewsFun::RegisterFunction(*this);
	EnableLoggingFun::RegisterFunction(*this);
	EnableProfilingFun::RegisterFunction(*this);
	TestAllTypesFun::RegisterFunction(*this);
	TestVectorTypesFun::RegisterFunction(*this);
}

} // namespace duckdb
