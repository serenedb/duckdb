#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/constraints/check_constraint.hpp"
#include "duckdb/parser/parsed_data/alter_domain_info.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"

namespace duckdb {

static DomainConstraint DomainConstraintFromEntry(ColumnConstraintEntry &entry) {
	DomainConstraint constraint;
	if (entry.constraint_name == "NotNullConstraint") {
		constraint.name = entry.constraint_type_info.name;
		constraint.type = ConstraintType::NOT_NULL;
	} else if (entry.constraint_name == "CheckConstraint") {
		auto &check = entry.constraint->Cast<CheckConstraint>();
		constraint.name = check.constraint_name;
		constraint.type = ConstraintType::CHECK;
		constraint.expression = std::move(check.expression);
	} else {
		throw InternalException("unexpected domain constraint %s", entry.constraint_name);
	}
	return constraint;
}

unique_ptr<CreateStatement>
PEGTransformerFactory::TransformCreateDomainStmt(PEGTransformer &transformer, const QualifiedName &qualified_name,
                                                 const bool &has_as, const LogicalType &type,
                                                 optional<vector<ColumnConstraintEntry>> domain_clause) {
	auto info = make_uniq<CreateTypeInfo>();
	info->SetQualifiedName(qualified_name);
	info->on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	info->type = type;
	info->domain_info = make_uniq<DomainInfo>();
	auto &domain = *info->domain_info;

	bool has_collation = false;
	bool has_explicit_null = false;
	if (domain_clause) {
		for (auto &entry : *domain_clause) {
			if (entry.constraint_name == "DefaultValue") {
				if (domain.default_value) {
					throw ParserException("multiple default expressions");
				}
				domain.default_value = std::move(entry.expression);
			} else if (entry.constraint_name == "ColumnCollation") {
				if (has_collation) {
					throw ParserException("multiple COLLATE clauses not allowed");
				}
				has_collation = true;
				domain.collation = entry.expression->Cast<ConstantExpression>().GetLiteral().ToValue().ToString();
			} else if (entry.constraint_name == "NotNullConstraint") {
				if (entry.constraint_type_info.type != ConstraintType::NOT_NULL) {
					has_explicit_null = true;
					continue;
				}
				domain.constraints.push_back(DomainConstraintFromEntry(entry));
			} else if (entry.constraint_name == "CheckConstraint") {
				domain.constraints.push_back(DomainConstraintFromEntry(entry));
			} else {
				throw InternalException("unexpected domain clause");
			}
		}
	}
	if (has_explicit_null && domain.IsNotNull()) {
		throw ParserException("conflicting NULL/NOT NULL constraints");
	}

	auto result = make_uniq<CreateStatement>();
	result->info = std::move(info);
	return result;
}

unique_ptr<AlterInfo> PEGTransformerFactory::TransformAddDomainConstraint(PEGTransformer &transformer,
                                                                          ColumnConstraintEntry domain_constraint,
                                                                          const optional<bool> &not_valid) {
	if (domain_constraint.constraint_name == "NotNullConstraint" &&
	    domain_constraint.constraint_type_info.type != ConstraintType::NOT_NULL) {
		throw ParserException("a domain constraint must be NOT NULL or CHECK");
	}
	auto result = make_uniq<AlterDomainInfo>(AlterDomainType::ADD_CONSTRAINT, AlterEntryData());
	result->constraint = DomainConstraintFromEntry(domain_constraint);
	result->constraint.validated = !(not_valid && *not_valid);
	return std::move(result);
}

ColumnConstraintEntry
PEGTransformerFactory::TransformNamedDomainConstraint(PEGTransformer &transformer,
                                                      const Identifier &constraint_name_clause,
                                                      ColumnConstraintEntry column_constraint_element) {
	return TransformNamedColumnConstraint(transformer, constraint_name_clause, std::move(column_constraint_element));
}

} // namespace duckdb
