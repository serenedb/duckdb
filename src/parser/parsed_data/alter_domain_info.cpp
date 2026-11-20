#include "duckdb/parser/parsed_data/alter_domain_info.hpp"

#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

unique_ptr<AlterInfo> AlterDomainInfo::Copy() const {
	auto result = make_uniq<AlterDomainInfo>(alter_domain_type, GetAlterEntryData());
	result->default_value = default_value ? default_value->Copy() : nullptr;
	result->constraint = constraint.Copy();
	result->constraint_name = constraint_name;
	result->new_constraint_name = new_constraint_name;
	result->constraint_if_exists = constraint_if_exists;
	result->cascade = cascade;
	result->new_name = new_name;
	return std::move(result);
}

string AlterDomainInfo::ToString() const {
	string result = "ALTER DOMAIN " + GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	switch (alter_domain_type) {
	case AlterDomainType::SET_DEFAULT:
		result += " SET DEFAULT " + default_value->ToString();
		break;
	case AlterDomainType::DROP_DEFAULT:
		result += " DROP DEFAULT";
		break;
	case AlterDomainType::SET_NOT_NULL:
		result += " SET NOT NULL";
		break;
	case AlterDomainType::DROP_NOT_NULL:
		result += " DROP NOT NULL";
		break;
	case AlterDomainType::ADD_CONSTRAINT:
		result += " ADD";
		if (!constraint.name.empty()) {
			result += " CONSTRAINT " + SQLIdentifier(constraint.name);
		}
		if (constraint.type == ConstraintType::NOT_NULL) {
			result += " NOT NULL";
		} else {
			result += " CHECK (" + constraint.expression->ToString() + ")";
		}
		if (!constraint.validated) {
			result += " NOT VALID";
		}
		break;
	case AlterDomainType::DROP_CONSTRAINT:
		result += " DROP CONSTRAINT ";
		if (constraint_if_exists) {
			result += "IF EXISTS ";
		}
		result += SQLIdentifier(constraint_name);
		if (cascade) {
			result += " CASCADE";
		}
		break;
	case AlterDomainType::RENAME_CONSTRAINT:
		result += " RENAME CONSTRAINT " + SQLIdentifier(constraint_name) + " TO " + SQLIdentifier(new_constraint_name);
		break;
	case AlterDomainType::VALIDATE_CONSTRAINT:
		result += " VALIDATE CONSTRAINT " + SQLIdentifier(constraint_name);
		break;
	case AlterDomainType::RENAME:
		result += " RENAME TO " + SQLIdentifier(new_name);
		break;
	}
	return result + ";";
}

} // namespace duckdb
