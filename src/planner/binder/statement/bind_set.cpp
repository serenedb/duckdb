#include "duckdb/parser/statement/set_statement.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_set.hpp"
#include "duckdb/planner/operator/logical_reset.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"

namespace duckdb {

static void CheckSetAccess(ClientContext &context, const RoleAccess &role, const Identifier &name, SetScope scope,
                           const char *statement) {
	const auto check = [&](const SettingAccess &access, SetScope variable_scope) {
		if (!Settings::MayChange(context, role, name, access, variable_scope)) {
			PhysicalSet::WarnOutsideTransaction(context, scope, statement);
			Settings::CheckChange(context, role, name, access, variable_scope);
		}
	};
	if (auto option = DBConfig::GetOptionByName(name)) {
		check(option->access, PhysicalSet::GetSettingScope(*option, scope));
		return;
	}
	ExtensionOption extension_option;
	if (DBConfig::GetConfig(context).TryGetExtensionOption(name, extension_option)) {
		check(extension_option.access, PhysicalSet::GetSettingScope(extension_option, scope));
	}
}

BoundStatement Binder::Bind(SetVariableStatement &stmt) {
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	// evaluate the scalar value
	Value value;
	unique_ptr<LogicalOperator> op;
	if (stmt.scope != SetScope::VARIABLE) {
		CheckSetAccess(context, CurrentRole(), stmt.name, stmt.scope, "SET LOCAL");
		ConstantBinder default_binder(*this, context, "SET value");
		auto bound_value = default_binder.Bind(stmt.value);
		if (bound_value->HasParameter()) {
			throw NotImplementedException("SET statements cannot have parameters");
		}
		value = ExpressionExecutor::EvaluateScalar(context, *bound_value, true);
	} else {
		auto select_node = make_uniq<SelectNode>();
		select_node->select_list.push_back(std::move(stmt.value));
		select_node->from_table = make_uniq<EmptyTableRef>();
		auto bound_select = Bind(*select_node);
		if (bound_select.types.size() > 1) {
			throw BinderException("SET variable expected a single input");
		}
		op = std::move(bound_select.plan);
	}
	auto set = make_uniq<LogicalSet>(stmt.name, std::move(value), stmt.scope);
	result.plan = std::move(set);
	if (op) {
		result.plan->children.push_back(std::move(op));
	}
	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

BoundStatement Binder::Bind(ResetVariableStatement &stmt) {
	BoundStatement result;
	result.types = {LogicalType::BOOLEAN};
	result.names = {"Success"};

	if (stmt.scope != SetScope::VARIABLE && !stmt.name.empty()) {
		CheckSetAccess(context, CurrentRole(), stmt.name, stmt.scope, "RESET LOCAL");
	}
	auto reset = make_uniq<LogicalReset>(stmt.name, stmt.scope);
	result.plan = std::move(reset);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

BoundStatement Binder::Bind(SetStatement &stmt) {
	switch (stmt.set_type) {
	case SetType::SET: {
		auto &set_stmt = stmt.Cast<SetVariableStatement>();
		return Bind(set_stmt);
	}
	case SetType::RESET: {
		auto &set_stmt = stmt.Cast<ResetVariableStatement>();
		return Bind(set_stmt);
	}
	default:
		throw NotImplementedException("Type not implemented for SetType");
	}
}

} // namespace duckdb
