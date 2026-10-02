//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/policy_command.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class PolicyCommand : uint8_t { ALL = 0, SELECT = 1, INSERT = 2, UPDATE = 3, DELETE = 4 };

enum class RowSecurityAction : uint8_t { ENABLE = 0, DISABLE = 1, FORCE = 2, NO_FORCE = 3 };

enum class AlterPolicyType : uint8_t { RENAME = 0, SET_CLAUSES = 1 };

} // namespace duckdb
