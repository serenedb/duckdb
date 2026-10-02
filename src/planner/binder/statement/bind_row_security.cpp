#include "duckdb/catalog/catalog_entry/policy_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/row_security.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/constraints/check_constraint.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_binder/projection_binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_security_barrier.hpp"

namespace duckdb {

optional_idx Binder::RowSecurityRole() {
	for (auto current = this; current; current = current->parent.get()) {
		if (current->bound_views.empty()) {
			continue;
		}
		auto &view = current->bound_views.begin()->get();
		if (view.security_invoker) {
			continue;
		}
		return optional_idx(view.permissions.owner);
	}
	return optional_idx();
}

static void RefuseTemporaryLookups(Binder &binder, StandardEntry &relation) {
	if (relation.temporary) {
		return;
	}
	auto lookup = binder.EntryRetriever().GetCallback();
	binder.SetCatalogLookupCallback([lookup](CatalogEntry &entry) {
		if (entry.temporary) {
			throw BinderException("policy expressions cannot use temporary object \"%s\"",
			                      entry.name.GetIdentifierName());
		}
		if (lookup) {
			lookup(entry);
		}
	});
}

bool Binder::RowSecurityApplies(StandardEntry &relation) {
	auto row_security = RowSecurity::Get(relation);
	if (!row_security || !row_security->enabled) {
		return false;
	}
	SetAlwaysRequireRebind();
	return !relation.ParentCatalog().BypassesRowSecurity(context, relation, RowSecurityRole());
}

unique_ptr<ParsedExpression> Binder::RowSecurityExpression(StandardEntry &relation, PolicyCommand command, bool check) {
	auto &catalog = relation.ParentCatalog();
	auto role = RowSecurityRole();
	vector<unique_ptr<ParsedExpression>> permissive;
	vector<unique_ptr<ParsedExpression>> restrictive;
	RowSecurity::Get(relation)->ScanPolicies(catalog.GetCatalogTransaction(context), [&](PolicyCatalogEntry &policy) {
		if (!policy.AppliesTo(command) || !catalog.IsRowSecurityMember(context, policy.roles, role)) {
			return;
		}
		auto &expr = check && policy.check_expr ? policy.check_expr : policy.using_expr;
		if (expr) {
			(policy.permissive ? permissive : restrictive).push_back(expr->Copy());
		}
	});
	unique_ptr<ParsedExpression> result;
	if (permissive.empty()) {
		result = make_uniq<ConstantExpression>(Value::BOOLEAN(false));
	} else if (permissive.size() == 1) {
		result = std::move(permissive[0]);
	} else {
		result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(permissive));
	}
	for (auto &expr : restrictive) {
		result = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(result), std::move(expr));
	}
	return result;
}

static shared_ptr<Binder> CreatePolicyBinder(Binder &binder, StandardEntry &relation,
                                             const std::function<void(BindContext &)> &add_binding) {
	auto policy_binder = Binder::CreateBinder(binder.context, &binder, BinderType::VIEW_BINDER);
	policy_binder->SetSearchPath(relation.ParentCatalog(), relation.ParentSchema(binder.context).name);
	RefuseTemporaryLookups(*policy_binder, relation);
	add_binding(policy_binder->bind_context);
	return policy_binder;
}

static void AddRowSecurityBinding(StandardEntry &relation, LogicalGet &get, BindContext &policy_context) {
	auto &column_ids = get.GetMutableColumnIds();
	if (relation.type == CatalogType::TABLE_ENTRY) {
		auto &table = relation.Cast<TableCatalogEntry>();
		vector<Identifier> table_names;
		for (auto &column : table.GetColumns().Logical()) {
			table_names.push_back(column.Name());
		}
		if (get.names == table_names) {
			policy_context.AddBaseTable(get.table_index, table.name, get.names, get.returned_types, column_ids, table);
			return;
		}
	}
	policy_context.AddBaseTable(get.table_index, relation.name, get.names, get.returned_types, column_ids,
	                            relation.name);
}

unique_ptr<LogicalOperator> Binder::ApplyRowSecurity(StandardEntry &relation, const vector<PolicyCommand> &commands,
                                                     unique_ptr<LogicalOperator> root,
                                                     const std::function<void(BindContext &)> &add_binding) {
	if (!RowSecurityApplies(relation)) {
		return root;
	}
	auto &expanding = global_binder_state->row_security_relations;
	if (expanding.find(relation) != expanding.end()) {
		throw BinderException("infinite recursion detected in policy for relation \"%s\"",
		                      relation.name.GetIdentifierName());
	}
	unique_ptr<ParsedExpression> qual;
	for (auto command : commands) {
		auto expr = RowSecurityExpression(relation, command, false);
		qual = qual
		           ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(qual), std::move(expr))
		           : std::move(expr);
	}

	auto policy_binder = CreatePolicyBinder(*this, relation, add_binding);
	expanding.insert(relation);
	WhereBinder where_binder(*policy_binder, context);
	auto condition = where_binder.Bind(qual);
	condition = BoundCastExpression::AddCastToType(context, std::move(condition), LogicalType::BOOLEAN);
	policy_binder->PlanSubqueries(condition, root);
	expanding.erase(relation);

	auto filter = make_uniq<LogicalFilter>(std::move(condition));
	filter->AddChild(std::move(root));
	return make_uniq<LogicalSecurityBarrier>(std::move(filter));
}

unique_ptr<LogicalOperator> Binder::ApplyRowSecurity(StandardEntry &relation, LogicalGet &get,
                                                     unique_ptr<LogicalOperator> root,
                                                     const vector<PolicyCommand> &commands) {
	return ApplyRowSecurity(relation, commands, std::move(root),
	                        [&](BindContext &policy_context) { AddRowSecurityBinding(relation, get, policy_context); });
}

static void VerifyNoSubqueries(const ParsedExpression &expr) {
	ParsedExpressionIterator::VisitExpression<SubqueryExpression>(expr, [&](const SubqueryExpression &) {
		throw NotImplementedException("row-level security WITH CHECK expressions with subqueries are not supported");
	});
}

static void BindValueFunctions(Binder &binder, const ColumnList &columns, unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (!colref.IsQualified() && !columns.ColumnExists(colref.GetColumnName())) {
			auto value_function = binder.GetSQLValueFunction(colref.GetColumnName());
			if (value_function) {
				expr = std::move(value_function);
			}
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { BindValueFunctions(binder, columns, child); });
}

void Binder::AddRowSecurityChecks(TableCatalogEntry &table, PolicyCommand command, bool select_visible,
                                  vector<unique_ptr<BoundConstraint>> &constraints) {
	if (!RowSecurityApplies(table)) {
		return;
	}
	vector<unique_ptr<ParsedExpression>> checks;
	checks.push_back(RowSecurityExpression(table, command, true));
	if (select_visible) {
		checks.push_back(RowSecurityExpression(table, PolicyCommand::SELECT, false));
	}
	for (auto &check : checks) {
		VerifyNoSubqueries(*check);
		BindValueFunctions(*this, table.GetColumns(), check);
		auto guarded = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(check),
		                                             make_uniq<ConstantExpression>(Value::BOOLEAN(false)));
		CheckConstraint constraint(std::move(guarded));
		auto bound = BindConstraint(constraint, table.name, table.GetColumns());
		auto &bound_check = bound->Cast<BoundCheckConstraint>();
		bound_check.violation_message = StringUtil::Format(
		    "new row violates row-level security policy for table \"%s\"", table.name.GetIdentifierName());
		constraints.push_back(std::move(bound));
	}
}

void Binder::BindRowSecurityChecks(TableCatalogEntry &table, LogicalGet &get, const vector<PolicyCommand> &commands,
                                   TableIndex proj_index, vector<unique_ptr<Expression>> &projection,
                                   vector<unique_ptr<BoundCheckConstraint>> &checks) {
	if (!RowSecurityApplies(table)) {
		return;
	}
	auto &expanding = global_binder_state->row_security_relations;
	if (expanding.find(table) != expanding.end()) {
		throw BinderException("infinite recursion detected in policy for relation \"%s\"",
		                      table.name.GetIdentifierName());
	}
	auto policy_binder = CreatePolicyBinder(
	    *this, table, [&](BindContext &policy_context) { AddRowSecurityBinding(table, get, policy_context); });
	expanding.insert(table);
	for (auto command : commands) {
		auto expr = RowSecurityExpression(table, command, false);
		ProjectionBinder binder(*policy_binder, context, proj_index, projection, "row-level security policy");
		binder.target_type = LogicalType::BOOLEAN;
		auto check = make_uniq<BoundCheckConstraint>();
		check->expression = binder.Bind(expr);
		check->violation_message = StringUtil::Format("target row violates row-level security policy for table \"%s\"",
		                                              table.name.GetIdentifierName());
		checks.push_back(std::move(check));
	}
	expanding.erase(table);
}

BoundStatement Binder::BindWithoutRowSecurity(TableRef &ref) {
	row_security_target = &ref;
	auto result = Bind(ref);
	row_security_target = nullptr;
	return result;
}

StandardEntry &Binder::BindPolicyRelation(BaseTableRef &base_table) {
	BindSchemaOrCatalog(base_table.GetQualifiedNameMutable());
	auto &entry = Catalog::GetEntry(context, EntryLookupInfo(CatalogType::TABLE_ENTRY, base_table.GetQualifiedName()));
	if (entry.type != CatalogType::TABLE_ENTRY && entry.type != CatalogType::VIEW_ENTRY) {
		throw BinderException("\"%s\" is not a table or view", base_table.Table().GetIdentifierName());
	}
	if (entry.internal) {
		throw CatalogException("Cannot create policies on internal catalog entry \"%s\"!",
		                       entry.name.GetIdentifierName());
	}
	return entry.Cast<StandardEntry>();
}

void Binder::BindPolicyClauses(StandardEntry &relation, PolicyCommand command,
                               optional_ptr<ParsedExpression> using_expr, optional_ptr<ParsedExpression> check_expr) {
	if (command == PolicyCommand::INSERT && using_expr) {
		throw BinderException("only WITH CHECK expression allowed for INSERT");
	}
	if ((command == PolicyCommand::SELECT || command == PolicyCommand::DELETE) && check_expr) {
		throw BinderException("WITH CHECK cannot be applied to SELECT or DELETE");
	}
	if (relation.type == CatalogType::VIEW_ENTRY) {
		if (command != PolicyCommand::SELECT && command != PolicyCommand::ALL) {
			throw BinderException("policies on views can only be created FOR SELECT or FOR ALL");
		}
		if (check_expr) {
			throw BinderException("WITH CHECK cannot be applied to policies on views");
		}
	}
	if (!using_expr && !check_expr) {
		return;
	}
	auto validation_binder = Binder::CreateBinder(context, this);
	BaseTableRef ref(TableDescription(
	    QualifiedName(relation.ParentCatalog().GetName(), relation.ParentSchemaName(), relation.name)));
	validation_binder->BindWithoutRowSecurity(ref);
	validation_binder->SetSearchPath(relation.ParentCatalog(), relation.ParentSchema(context).name);
	RefuseTemporaryLookups(*validation_binder, relation);
	for (auto expr : {using_expr, check_expr}) {
		if (!expr) {
			continue;
		}
		auto copy = expr->Copy();
		WhereBinder where_binder(*validation_binder, context);
		where_binder.Bind(copy);
	}
}

} // namespace duckdb
