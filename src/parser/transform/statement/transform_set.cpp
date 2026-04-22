#include "duckdb/parser/statement/set_statement.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/transformer.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"

namespace duckdb {

namespace {

SetScope ToSetScope(duckdb_libpgquery::VariableSetScope pg_scope) {
	switch (pg_scope) {
	case duckdb_libpgquery::VariableSetScope::VAR_SET_SCOPE_LOCAL:
		return SetScope::LOCAL;
	case duckdb_libpgquery::VariableSetScope::VAR_SET_SCOPE_SESSION:
		return SetScope::SESSION;
	case duckdb_libpgquery::VariableSetScope::VAR_SET_SCOPE_GLOBAL:
		return SetScope::GLOBAL;
	case duckdb_libpgquery::VariableSetScope::VAR_SET_SCOPE_VARIABLE:
		return SetScope::VARIABLE;
	case duckdb_libpgquery::VariableSetScope::VAR_SET_SCOPE_DEFAULT:
		return SetScope::AUTOMATIC;
	default:
		throw InternalException("Unexpected pg_scope: %d", pg_scope);
	}
}

SetType ToSetType(duckdb_libpgquery::VariableSetKind pg_kind) {
	switch (pg_kind) {
	case duckdb_libpgquery::VariableSetKind::VAR_SET_VALUE:
		return SetType::SET;
	case duckdb_libpgquery::VariableSetKind::VAR_RESET:
		return SetType::RESET;
	default:
		throw NotImplementedException("Can only SET or RESET a variable");
	}
}

} // namespace

unique_ptr<SetStatement> Transformer::TransformSetSearchPath(duckdb_libpgquery::PGVariableSetStmt &stmt, string name,
                                                             SetScope scope) {
	// Produce one already-PG-quoted element from a single arg node.
	auto serialize = [&](duckdb_libpgquery::PGNode &arg) -> string {
		auto expr = TransformExpression(arg);
		if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
			// ColumnRefExpression::ToString applies PG quoting so names with
			// commas/dots survive ParseList as one atomic entry.
			return expr->ToString();
		}
		if (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			return expr->Cast<ConstantExpression>().value.ToString();
		}
		throw ParserException("SET search_path: expected identifier or string literal");
	};

	auto make_set = [&](string value) {
		return make_uniq<SetVariableStatement>(std::move(name), make_uniq<ConstantExpression>(Value(std::move(value))),
		                                       scope);
	};

	// Multi-arg: comma-join each element.
	//   SET search_path TO a, "b,c", cat.s  -> "a,\"b,c\",cat.s"
	if (stmt.args->length > 1) {
		string joined;
		for (auto cell = stmt.args->head; cell; cell = cell->next) {
			auto node = PGPointerCast<duckdb_libpgquery::PGNode>(cell->data.ptr_value);
			if (!joined.empty()) {
				joined += ",";
			}
			joined += serialize(*node);
		}
		return make_set(std::move(joined));
	}

	// Single-arg. Four sub-cases:
	//   - PG_A_Const string literal   -> wrap in double quotes so ParseList
	//                                    treats it as one atomic entry. Empty
	//                                    literal -> empty path (no wrap).
	//   - DEFAULT                     -> RESET.
	//   - identifier / numeric        -> serialize via the lambda.
	auto node = PGPointerCast<duckdb_libpgquery::PGNode>(stmt.args->head->data.ptr_value);
	if (node->type == duckdb_libpgquery::T_PGAConst) {
		auto const_val = PGPointerCast<duckdb_libpgquery::PGAConst>(node.get());
		if (const_val->val.type == duckdb_libpgquery::T_PGString) {
			string raw(const_val->val.val.str);
			if (raw.empty()) {
				return make_set(std::move(raw));
			}
			string wrapped = "\"" + StringUtil::Replace(raw, "\"", "\"\"") + "\"";
			return make_set(std::move(wrapped));
		}
	}
	auto expr = TransformExpression(node);
	if (expr->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
		return make_uniq<ResetVariableStatement>(std::move(name), scope);
	}
	return make_set(serialize(*node));
}

unique_ptr<SetStatement> Transformer::TransformSetVariable(duckdb_libpgquery::PGVariableSetStmt &stmt) {
	string name(stmt.name);
	D_ASSERT(!name.empty()); // parser protect us!
	auto scope = ToSetScope(stmt.scope);
	D_ASSERT(stmt.args->head && stmt.args->head->data.ptr_value);

	if (StringUtil::CIEquals(name, "search_path")) {
		return TransformSetSearchPath(stmt, std::move(name), scope);
	}

	if (stmt.args->length > 1) {
		throw ParserException("SET needs a single scalar value parameter");
	}

	auto arg_to_expr = [&](optional_ptr<duckdb_libpgquery::PGNode> node) -> unique_ptr<ParsedExpression> {
		auto expr = TransformExpression(node);
		if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
			auto &colref = expr->Cast<ColumnRefExpression>();
			Value val;
			if (!colref.IsQualified()) {
				val = Value(colref.GetColumnName());
			} else {
				val = Value(expr->ToString());
			}
			expr = make_uniq<ConstantExpression>(std::move(val));
		}
		return expr;
	};

	auto const_node = PGPointerCast<duckdb_libpgquery::PGNode>(stmt.args->head->data.ptr_value);
	auto expr = arg_to_expr(const_node);
	if (expr->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
		// set to default = reset
		return make_uniq<ResetVariableStatement>(std::move(name), scope);
	}
	return make_uniq<SetVariableStatement>(std::move(name), std::move(expr), scope);
}

unique_ptr<SetStatement> Transformer::TransformResetVariable(duckdb_libpgquery::PGVariableSetStmt &stmt) {
	D_ASSERT(stmt.kind == duckdb_libpgquery::VariableSetKind::VAR_RESET);

	string name(stmt.name);
	D_ASSERT(!name.empty()); // parser protect us!

	return make_uniq<ResetVariableStatement>(name, ToSetScope(stmt.scope));
}

unique_ptr<SQLStatement> Transformer::TransformSetTransaction(duckdb_libpgquery::PGVariableSetStmt &stmt) {
	D_ASSERT(stmt.kind == duckdb_libpgquery::VariableSetKind::VAR_SET_MULTI);
	string stmt_name(stmt.name);
	bool is_session = (stmt_name == "SESSION CHARACTERISTICS");
	string setting_name = is_session ? "default_transaction_isolation" : "transaction_isolation";

	for (auto cell = stmt.args->head; cell; cell = cell->next) {
		auto def = PGPointerCast<duckdb_libpgquery::PGDefElem>(cell->data.ptr_value);
		string opt_name(def->defname);
		if (opt_name == "transaction_isolation") {
			auto val = PGPointerCast<duckdb_libpgquery::PGAConst>(def->arg);
			string iso_level(val->val.val.str);
			return make_uniq<SetVariableStatement>(std::move(setting_name),
			                                       make_uniq<ConstantExpression>(Value(std::move(iso_level))),
			                                       SetScope::AUTOMATIC);
		}
		if (opt_name == "transaction_read_only") {
			auto val = PGPointerCast<duckdb_libpgquery::PGAConst>(def->arg);
			string read_only_setting = is_session ? "default_transaction_read_only" : "transaction_read_only";
			return make_uniq<SetVariableStatement>(std::move(read_only_setting),
			                                       make_uniq<ConstantExpression>(Value::BOOLEAN(val->val.val.ival)),
			                                       SetScope::AUTOMATIC);
		}
		if (opt_name == "transaction_deferrable") {
			throw NotImplementedException("DEFERRABLE transactions are not supported");
		}
	}
	throw ParserException("SET TRANSACTION requires at least one option");
}

unique_ptr<SQLStatement> Transformer::TransformSet(duckdb_libpgquery::PGVariableSetStmt &stmt) {
	D_ASSERT(stmt.type == duckdb_libpgquery::T_PGVariableSetStmt);

	if (stmt.kind == duckdb_libpgquery::VariableSetKind::VAR_RESET_ALL) {
		return make_uniq<ResetVariableStatement>(string {}, ToSetScope(stmt.scope));
	}

	if (stmt.kind == duckdb_libpgquery::VariableSetKind::VAR_SET_MULTI) {
		return TransformSetTransaction(stmt);
	}

	SetType set_type = ToSetType(stmt.kind);
	switch (set_type) {
	case SetType::SET:
		return TransformSetVariable(stmt);
	case SetType::RESET:
		return TransformResetVariable(stmt);
	default:
		throw InternalException("Type not implemented for SetType");
	}
}

} // namespace duckdb
