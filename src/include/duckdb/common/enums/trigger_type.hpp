//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/trigger_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class TriggerTiming : uint8_t { BEFORE = 0, AFTER = 1, INSTEAD_OF = 2 };

enum class TriggerEventType : uint8_t { INSERT_EVENT = 0, DELETE_EVENT = 1, UPDATE_EVENT = 2 };

enum class TriggerForEach : uint8_t { STATEMENT = 0, ROW = 1 };

//! When a trigger fires, relative to the session's replication role
enum class TriggerFiring : uint8_t { ORIGIN = 0, DISABLED = 1, REPLICA = 2, ALWAYS = 3 };

//! The session_replication_role setting: replica sessions apply replicated changes
enum class ReplicationRole : uint8_t { ORIGIN = 0, REPLICA = 1, LOCAL = 2 };

} // namespace duckdb
