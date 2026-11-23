//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/role_spec.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

enum class RoleSpecType : uint8_t { NAME, CURRENT_ROLE, CURRENT_USER, SESSION_USER };

struct RoleSpec {
	RoleSpecType type = RoleSpecType::NAME;
	Identifier name;

	RoleSpec() = default;
	explicit RoleSpec(Identifier name_p) : name(std::move(name_p)) {
	}
	explicit RoleSpec(RoleSpecType type_p) : type(type_p) {
	}

	bool empty() const { // NOLINT: match std::string interface
		return type == RoleSpecType::NAME && name.empty();
	}
	bool IsPublic() const {
		return type == RoleSpecType::NAME && name.GetIdentifierName() == "public";
	}
	string ToString() const;
	static string ToString(const vector<RoleSpec> &roles);
};

} // namespace duckdb
