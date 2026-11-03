//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/engine_host.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/planner/bound_statement.hpp"
#include "duckdb/planner/planner_extension.hpp"

#include <span>

namespace duckdb {
class Binder;
class ClientContext;
class DatabaseInstance;
class Deserializer;
class DuckDB;
class ErrorData;
class Expression;
class LogicalOperator;
class Optimizer;
class ParsedExpression;
class SQLStatement;
struct DefaultType;
struct LogicalExtensionOperator;

class EngineHost {
public:
	static EngineHost &Get();
	static void Install(EngineHost &host);
	static bool IsInstalled();

	virtual void LoadExtensions(DuckDB &db) = 0;
	virtual std::span<const DefaultType> GetTypes() = 0;

	virtual void OnConnectionOpened(ClientContext &context) = 0;
	virtual void OnConnectionClosed(ClientContext &context) = 0;
	virtual void OnBeginExtensionLoad(DatabaseInstance &db, const string &name) = 0;
	virtual void OnExtensionLoaded(DatabaseInstance &db, const string &name) = 0;
	virtual void OnExtensionLoadFail(DatabaseInstance &db, const string &name, const ErrorData &error) = 0;

	virtual BoundStatement BindStatement(ClientContext &context, Binder &binder, SQLStatement &statement) = 0;
	virtual void PostBind(ClientContext &context, Binder &binder, BoundStatement &statement) = 0;
	virtual GetSQLValueFunctionReturnType BindSQLValueFunction(ClientContext &context, Binder &binder,
	                                                           const string &name,
	                                                           unique_ptr<ParsedExpression> &result) = 0;
	virtual unique_ptr<LogicalExtensionOperator> DeserializeOperator(ClientContext &context, const string &extension,
	                                                                 Deserializer &deserializer) = 0;

	virtual void PreOptimize(Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) = 0;
	virtual void BeforeOptimizer(OptimizerType type, Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) = 0;
	virtual void AfterOptimizer(OptimizerType type, Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) = 0;
	virtual void PostOptimize(Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) = 0;
	virtual bool IsRewriteBarrier(ClientContext &context, const Expression &parent, bool parent_frozen) = 0;

protected:
	~EngineHost() = default;
};

} // namespace duckdb
