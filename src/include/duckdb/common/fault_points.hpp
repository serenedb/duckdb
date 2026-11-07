//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/fault_points.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

#include <string_view>

namespace duckdb {

struct FaultPoints {
	static bool Armed(std::string_view name) noexcept;
	static bool Arm(std::string_view name);
	static bool Disarm(std::string_view name);
	static void Clear() noexcept;
	static vector<string> List();
	static void WaitWhileArmed(std::string_view name);
	static void ParkOnce(std::string_view name);
};

} // namespace duckdb

#ifdef SDB_FAULT_INJECTION
#define SDB_IF_FAILURE(name)           if (::duckdb::FaultPoints::Armed(name))
#define SDB_WAIT_ON_FAILURE(name)      ::duckdb::FaultPoints::WaitWhileArmed(name)
#define SDB_PARK_ONCE_ON_FAILURE(name) ::duckdb::FaultPoints::ParkOnce(name)
#else
#define SDB_IF_FAILURE(name) if constexpr (false)
#define SDB_WAIT_ON_FAILURE(name)
#define SDB_PARK_ONCE_ON_FAILURE(name)
#endif
