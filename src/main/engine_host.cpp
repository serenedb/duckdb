#include "duckdb/main/engine_host.hpp"

#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/optimizer/cte_filter_pusher.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/planner/planner_extension.hpp"

namespace duckdb {

namespace {

class RegisteredExtensions final : public EngineHost {
public:
	void LoadExtensions(DuckDB &db) override {
		ExtensionHelper::LoadAllExtensions(db);
	}

	std::span<const DefaultType> GetTypes() override {
		return {};
	}

	void OnConnectionOpened(ClientContext &context) override {
		for (auto &callback : ExtensionCallback::Iterate(context)) {
			callback->OnConnectionOpened(context);
		}
	}

	void OnConnectionClosed(ClientContext &context) override {
		for (auto &callback : ExtensionCallback::Iterate(context)) {
			callback->OnConnectionClosed(context);
		}
	}

	void OnBeginExtensionLoad(DatabaseInstance &db, const string &name) override {
		for (auto &callback : ExtensionCallback::Iterate(db)) {
			callback->OnBeginExtensionLoad(db, name);
		}
	}

	void OnExtensionLoaded(DatabaseInstance &db, const string &name) override {
		for (auto &callback : ExtensionCallback::Iterate(db)) {
			callback->OnExtensionLoaded(db, name);
		}
	}

	void OnExtensionLoadFail(DatabaseInstance &db, const string &name, const ErrorData &error) override {
		for (auto &callback : ExtensionCallback::Iterate(db)) {
			callback->OnExtensionLoadFail(db, name, error);
		}
	}

	BoundStatement BindStatement(ClientContext &context, Binder &binder, SQLStatement &statement) override {
		for (auto &extension : OperatorExtension::Iterate(context)) {
			auto bound_statement = extension->Bind(context, binder, extension->operator_info.get(), statement);
			if (bound_statement.plan) {
				return bound_statement;
			}
		}
		return BoundStatement();
	}

	void PostBind(ClientContext &context, Binder &binder, BoundStatement &statement) override {
		for (auto &extension : PlannerExtension::Iterate(context)) {
			if (extension.post_bind_function) {
				PlannerExtensionInput input {context, binder, extension.planner_info.get()};
				extension.post_bind_function(input, statement);
			}
		}
	}

	GetSQLValueFunctionReturnType BindSQLValueFunction(ClientContext &context, Binder &binder, const string &name,
	                                                   unique_ptr<ParsedExpression> &result) override {
		for (auto &extension : PlannerExtension::Iterate(context)) {
			if (!extension.get_sql_value_function) {
				continue;
			}
			PlannerExtensionInput input {context, binder, extension.planner_info.get()};
			auto result_type = extension.get_sql_value_function(input, name, result);
			if (result_type == GetSQLValueFunctionReturnType::FINISH_BINDING || result) {
				return result_type;
			}
		}
		return GetSQLValueFunctionReturnType::CONTINUE_BINDING;
	}

	unique_ptr<LogicalExtensionOperator> DeserializeOperator(ClientContext &context, const string &extension_name,
	                                                         Deserializer &deserializer) override {
		for (auto &extension : OperatorExtension::Iterate(context)) {
			if (extension->GetName() == extension_name) {
				return extension->Deserialize(deserializer);
			}
		}
		return nullptr;
	}

	void PreOptimize(Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) override {
		for (auto &extension : OptimizerExtension::Iterate(optimizer.GetContext())) {
			if (extension.anchor != OptimizerType::INVALID) {
				continue;
			}
			optimizer.RunOptimizer(OptimizerType::EXTENSION, [&]() {
				OptimizerExtensionInput input {optimizer.GetContext(), optimizer, extension.optimizer_info.get()};
				if (extension.pre_optimize_function) {
					CTEFilterPusher::ClearDependencies(*plan);
					extension.pre_optimize_function(input, plan);
				}
			});
		}
	}

	void BeforeOptimizer(OptimizerType type, Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) override {
		RunAnchored(type, OptimizerHookPosition::Before, optimizer, plan);
	}

	void AfterOptimizer(OptimizerType type, Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) override {
		RunAnchored(type, OptimizerHookPosition::After, optimizer, plan);
	}

	void PostOptimize(Optimizer &optimizer, unique_ptr<LogicalOperator> &plan) override {
		for (auto &extension : OptimizerExtension::Iterate(optimizer.GetContext())) {
			if (extension.anchor != OptimizerType::INVALID) {
				continue;
			}
			optimizer.RunOptimizer(OptimizerType::EXTENSION, [&]() {
				OptimizerExtensionInput input {optimizer.GetContext(), optimizer, extension.optimizer_info.get()};
				if (extension.optimize_function) {
					extension.optimize_function(input, plan);
				}
			});
		}
	}

	bool IsRewriteBarrier(ClientContext &context, const Expression &parent, bool parent_frozen) override {
		for (auto &extension : OptimizerExtension::Iterate(context)) {
			if (extension.rewrite_barrier && extension.rewrite_barrier(parent, parent_frozen)) {
				return true;
			}
		}
		return false;
	}

private:
	static void RunAnchored(OptimizerType type, OptimizerHookPosition where, Optimizer &optimizer,
	                        unique_ptr<LogicalOperator> &plan) {
		for (auto &extension : OptimizerExtension::Iterate(optimizer.GetContext())) {
			if (!extension.rule || extension.anchor != type || extension.where != where) {
				continue;
			}
			OptimizerExtensionInput input {optimizer.GetContext(), optimizer, extension.optimizer_info.get()};
			extension.rule(input, plan);
		}
	}
};

constinit RegisteredExtensions registered_extensions;
constinit EngineHost *installed_host = nullptr;

} // namespace

EngineHost &EngineHost::Get() {
	if (installed_host) {
		return *installed_host;
	}
	return registered_extensions;
}

void EngineHost::Install(EngineHost &host) {
	if (installed_host) {
		throw InternalException("An engine host is already installed");
	}
	installed_host = &host;
}

bool EngineHost::IsInstalled() {
	return installed_host;
}

} // namespace duckdb
