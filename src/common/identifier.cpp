#include "duckdb/common/identifier.hpp"

#include "duckdb/common/types/uuid.hpp"

#include <ostream>

namespace duckdb {

Identifier GenerateInternalName(const char *prefix) {
	return Identifier(prefix + UUID::ToString(UUID::GenerateRandomUUID()));
}

std::ostream &operator<<(std::ostream &os, const Identifier &id) {
	return os << id.GetIdentifierName();
}

} // namespace duckdb
