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

namespace duckdb_shell {

struct DocsRequest {
	duckdb::vector<duckdb::string> args;
	duckdb::idx_t width = 80;
	bool color = false;
	std::function<bool(const duckdb::string &sql, duckdb::string &out)> query;
};

struct DocsBackend {
	std::function<bool(const DocsRequest &request, duckdb::string &out)> run;
	std::function<duckdb::vector<duckdb::string>(const duckdb::string &prefix)> complete;
};

void RegisterDocsBackend(DocsBackend backend);

bool HasDocsBackend();

const DocsBackend &GetDocsBackend();

bool DocsCompletions(const char *line, duckdb::idx_t length, duckdb::idx_t &argument_start,
                     duckdb::vector<duckdb::string> &completions);

} // namespace duckdb_shell
