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
}

namespace duckdb_shell {

struct DocsRequest {
	duckdb::vector<duckdb::string> args;
	duckdb::idx_t width = 80;
	bool color = false;
	bool interactive = false;
	duckdb::DatabaseInstance *instance = nullptr;
};

struct DocsCompletion {
	duckdb::string text;
	duckdb::string label;
	bool selected = false;
};

struct DocsBackend {
	std::function<bool(const DocsRequest &request, duckdb::string &out)> run;
	std::function<duckdb::vector<DocsCompletion>(duckdb::DatabaseInstance *instance, const duckdb::string &argument)>
	    complete;
	std::function<bool()> offered;
	std::function<void(duckdb::ClientContext &context)> load;
};

void RegisterDocsBackend(DocsBackend backend);

void LoadDocsBackend(duckdb::ClientContext &context);

bool DocsCompletions(const char *line, duckdb::idx_t length, duckdb::idx_t &argument_start,
                     duckdb::vector<DocsCompletion> &completions);

} // namespace duckdb_shell
