//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/plan_data_version.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/winapi.hpp"

namespace duckdb {
class ClientContext;

class PlanDataVersion {
public:
	DUCKDB_API virtual ~PlanDataVersion() = default;

	DUCKDB_API virtual bool IsCurrent(ClientContext &context) const = 0;
};

} // namespace duckdb
