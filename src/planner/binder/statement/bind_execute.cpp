#include "duckdb/planner/binder.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/parser/statement/execute_statement.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/operator/logical_execute.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

static bool OnlyPositionalParameters(const identifier_map_t<idx_t> &parameters) {
	for (auto &entry : parameters) {
		for (auto c : entry.first.GetIdentifierName()) {
			if (!StringUtil::CharacterIsDigit(c)) {
				return false;
			}
		}
	}
	return true;
}

static bool ResultTypesAreFixed(const PreparedStatementData &prepared,
                                const identifier_map_t<BoundParameterData> &values) {
	if (!prepared.properties.bound_all_parameters) {
		return false;
	}
	for (auto &entry : prepared.value_map) {
		auto value = values.find(entry.first);
		if (value != values.end() && value->second.GetValue().type() != entry.second->return_type) {
			return false;
		}
	}
	return true;
}

static void VerifyResultTypes(const ExecuteStatement &stmt, const PreparedStatementData &stored,
                              const PreparedStatementData &replanned,
                              const identifier_map_t<BoundParameterData> &values) {
	if (replanned.types != stored.types && !ClientContext::IsPreparedStatementHandle(stmt.name) &&
	    ResultTypesAreFixed(stored, values)) {
		throw NotImplementedException("cached plan must not change result type");
	}
}

BoundStatement Binder::Bind(ExecuteStatement &stmt) {
	auto parameter_count = stmt.named_param_map.size();

	// bind the prepared statement
	auto &client_data = ClientData::Get(context);

	auto entry = client_data.prepared_statements.find(stmt.name);
	if (entry == client_data.prepared_statements.end()) {
		throw BinderException::MissingPreparedStatement(stmt.name);
	}

	// check if we need to rebind the prepared statement
	// this happens if the catalog changes, since in this case e.g. tables we relied on may have been deleted
	auto prepared = entry->second;
	auto &named_param_map = prepared->unbound_statement->named_param_map;
	const auto provided = stmt.bound_values.empty() ? stmt.named_values.size() : stmt.bound_values.size();
	if (provided != named_param_map.size() && OnlyPositionalParameters(named_param_map)) {
		throw ParserException("wrong number of parameters for prepared statement \"%s\"",
		                      stmt.name.GetIdentifierName());
	}

	// values that were supplied pre-typed do not go through the literal binding below
	identifier_map_t<BoundParameterData> bind_values = stmt.bound_values;
	if (stmt.bound_values.empty()) {
		PreparedStatement::VerifyParameters(stmt.named_values, named_param_map, &context);
	} else {
		PreparedStatement::VerifyParameters(stmt.bound_values, named_param_map, &context);
	}

	auto &mapped_named_values = stmt.named_values;
	// bind any supplied parameters
	auto constant_binder = Binder::CreateBinder(context);
	constant_binder->SetCanContainNulls(true);
	for (auto &pair : mapped_named_values) {
		bool is_literal = pair.second->GetExpressionType() == ExpressionType::VALUE_CONSTANT;

		ConstantBinder cbinder(*constant_binder, context, "EXECUTE statement");
		auto bound_expr = cbinder.Bind(pair.second);
		BoundParameterData parameter_data;
		if (is_literal) {
			auto &constant = bound_expr->Cast<BoundConstantExpression>();
			LogicalType return_type;
			if (constant.GetReturnType() == LogicalTypeId::VARCHAR &&
			    StringType::GetCollation(constant.GetReturnType()).empty()) {
				return_type = LogicalTypeId::STRING_LITERAL;
			} else if (constant.GetReturnType().IsIntegral()) {
				return_type = LogicalType::INTEGER_LITERAL(constant.GetValue());
			} else {
				return_type = constant.GetValue().type();
			}
			parameter_data = BoundParameterData(constant.TakeValue(), std::move(return_type));
		} else {
			auto value = ExpressionExecutor::EvaluateScalar(context, *bound_expr, true);
			auto value_type = value.type();
			parameter_data = BoundParameterData(std::move(value), std::move(value_type));
		}
		bind_values[pair.first] = std::move(parameter_data);
	}
	for (auto &hint : prepared->parameter_type_hints) {
		auto value = bind_values.find(hint.first);
		if (value == bind_values.end() || value->second.return_type == hint.second) {
			continue;
		}
		auto cast = BoundCastExpression::AddCastToType(
		    context, make_uniq<BoundConstantExpression>(value->second.GetValue()), hint.second);
		value->second = BoundParameterData(ExpressionExecutor::EvaluateScalar(context, *cast), hint.second);
	}
	prepared->PopulateMissingParameterValues(context, bind_values);
	unique_ptr<LogicalOperator> rebound_plan;

	RebindQueryInfo rebind = RebindQueryInfo::DO_NOT_REBIND;
	if (prepared->RequireRebind(context, bind_values)) {
		rebind = RebindQueryInfo::ATTEMPT_TO_REBIND;
	}
	for (auto &state : context.registered_state->States()) {
		BindPreparedStatementCallbackInfo info {*prepared, bind_values};
		auto new_rebind = state->OnRebindPreparedStatement(context, info, rebind);
		if (new_rebind == RebindQueryInfo::ATTEMPT_TO_REBIND) {
			rebind = RebindQueryInfo::ATTEMPT_TO_REBIND;
		}
	}
	optional<Identifier> replaces;
	if (rebind != RebindQueryInfo::ATTEMPT_TO_REBIND && !prepared->DataIsCurrent(context)) {
		Planner generic_planner(context);
		generic_planner.parameter_type_hints = prepared->parameter_type_hints;
		auto replanned = generic_planner.PrepareSQLStatement(prepared->unbound_statement->Copy());
		if (replanned->properties.bound_all_parameters) {
			VerifyResultTypes(stmt, *entry->second, *replanned, bind_values);
			replanned->parameter_type_hints = prepared->parameter_type_hints;
			replanned->prepare_time = prepared->prepare_time;
			prepared = std::move(replanned);
			rebound_plan = std::move(generic_planner.plan);
			replaces = stmt.name;
			global_binder_state->bound_tables = generic_planner.binder->global_binder_state->bound_tables;
		} else {
			rebind = RebindQueryInfo::ATTEMPT_TO_REBIND;
		}
	}
	if (rebind == RebindQueryInfo::ATTEMPT_TO_REBIND) {
		// catalog was modified or statement does not have clear types: rebind the statement before running the execute
		Planner prepared_planner(context);
		prepared_planner.parameter_data = bind_values;
		prepared_planner.parameter_type_hints = entry->second->parameter_type_hints;
		prepared = prepared_planner.PrepareSQLStatement(entry->second->unbound_statement->Copy());
		VerifyResultTypes(stmt, *entry->second, *prepared, bind_values);
		prepared->parameter_type_hints = entry->second->parameter_type_hints;
		rebound_plan = std::move(prepared_planner.plan);
		D_ASSERT(prepared->properties.bound_all_parameters);
		global_binder_state->bound_tables = prepared_planner.binder->global_binder_state->bound_tables;
	}
	// copy the properties of the prepared statement into the planner
	auto &properties = GetStatementProperties();
	properties = prepared->properties;
	properties.parameter_count = parameter_count;

	BoundStatement result;
	result.names = prepared->names;
	result.types = prepared->types;

	prepared->Bind(context, bind_values);
	if (rebound_plan) {
		auto execute_plan = make_uniq<LogicalExecute>(std::move(prepared));
		execute_plan->replaces = std::move(replaces);
		execute_plan->children.push_back(std::move(rebound_plan));
		result.plan = std::move(execute_plan);
	} else {
		result.plan = make_uniq<LogicalExecute>(std::move(prepared));
	}
	return result;
}

} // namespace duckdb
