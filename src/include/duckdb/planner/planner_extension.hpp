//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/planner_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_statement.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
struct DBConfig;
class Binder;
class ClientContext;

//! The PlannerExtensionInfo holds static information relevant to the planner extension
struct PlannerExtensionInfo {
	virtual ~PlannerExtensionInfo() {
	}
};

enum class GetSQLValueFunctionReturnType { CONTINUE_BINDING, FINISH_BINDING };

struct PlannerExtensionInput {
	ClientContext &context;
	Binder &binder;
	optional_ptr<PlannerExtensionInfo> info;
};

//! The post_bind function runs after binding succeeds, allowing modification of the bound statement
typedef void (*post_bind_function_t)(PlannerExtensionInput &input, BoundStatement &statement);

//! A dependent join (LATERAL or a correlated subquery) before decorrelation: `right` is its correlated side
struct LateralJoinInput {
	unique_ptr<LogicalOperator> &left;
	unique_ptr<LogicalOperator> &right;
	const CorrelatedColumns &correlated_columns;
	JoinType join_type;
	optional_ptr<Expression> condition;
	//! The join is an EXACT NEAREST BY
	bool exact_nearest;
};

//! Returns the plan of the whole join, or nullptr to leave the join to decorrelation
typedef unique_ptr<LogicalOperator> (*plan_lateral_join_function_t)(PlannerExtensionInput &input,
                                                                    LateralJoinInput &join);

typedef GetSQLValueFunctionReturnType (*get_sql_value_function_t)(PlannerExtensionInput &input,
                                                                  const string &column_name,
                                                                  unique_ptr<ParsedExpression> &result);

class PlannerExtension {
public:
	//! The post-bind function of the planner extension.
	//! Takes a bound statement as input, which it can modify in place.
	//! This runs after the binder has successfully bound the statement,
	//! allowing modification of the plan and result types.
	post_bind_function_t post_bind_function = nullptr;

	//! Override that allows registering a different callback for SQL value functions
	get_sql_value_function_t get_sql_value_function = nullptr;

	//! Runs on every dependent join before it is decorrelated
	plan_lateral_join_function_t plan_lateral_join_function = nullptr;

	//! Additional planner info passed to the functions
	shared_ptr<PlannerExtensionInfo> planner_info;

	static void Register(DBConfig &config, PlannerExtension extension);
	static ExtensionCallbackIteratorHelper<PlannerExtension> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).PlannerExtensions();
	}
};

} // namespace duckdb
