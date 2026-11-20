#include "duckdb/parser/parsed_data/domain_info.hpp"

namespace duckdb {

DomainConstraint DomainConstraint::Copy() const {
	DomainConstraint copy;
	copy.name = name;
	copy.type = type;
	copy.expression = expression ? expression->Copy() : nullptr;
	copy.validated = validated;
	return copy;
}

bool DomainInfo::IsNotNull() const {
	for (auto &constraint : constraints) {
		if (constraint.type == ConstraintType::NOT_NULL) {
			return true;
		}
	}
	return false;
}

unique_ptr<DomainInfo> DomainInfo::Copy() const {
	auto result = make_uniq<DomainInfo>();
	result->default_value = default_value ? default_value->Copy() : nullptr;
	result->collation = collation;
	result->base_type = base_type;
	for (auto &constraint : constraints) {
		result->constraints.push_back(constraint.Copy());
	}
	return result;
}

} // namespace duckdb
