//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/job_schedule_kind.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class JobScheduleKind : uint8_t { EVERY = 0, AFTER = 1 };

enum class AlterJobType : uint8_t { SUSPEND = 0, RESUME = 1, SET_SCHEDULE = 2 };

} // namespace duckdb
