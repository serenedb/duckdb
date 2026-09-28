//===----------------------------------------------------------------------===//
//                         DuckDB
//
// shell_docs.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

#include <functional>

namespace duckdb {
class ClientContext;
class DatabaseInstance;
} // namespace duckdb

namespace duckdb_shell {

struct DocsRequest {
	duckdb::vector<duckdb::string> args;
	duckdb::idx_t width = 80;
	bool color = false;
	duckdb::DatabaseInstance *instance = nullptr;
};

struct DocsBackend {
	std::function<bool(const DocsRequest &request, duckdb::string &out)> run;
	std::function<void(duckdb::ClientContext &context)> load;
};

void RegisterDocsBackend(DocsBackend backend);

void LoadDocsBackend(duckdb::ClientContext &context);

} // namespace duckdb_shell
