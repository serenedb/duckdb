//===----------------------------------------------------------------------===//
//                         DuckDB
//
// shell_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/planner_extension.hpp"

namespace duckdb {

class ShellExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;

	static void PostBind(PlannerExtensionInput &input, BoundStatement &statement);
};

} // namespace duckdb
