//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/read_duckdb.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {
struct ReplacementScanInput;
struct ReplacementScanData;

struct ReadDuckDBTableFunction {
	static TableFunction GetFunction();
	static unique_ptr<TableRef> ReplacementScan(ClientContext &context, ReplacementScanInput &input,
	                                            optional_ptr<ReplacementScanData> data);
};

//! Builds a standalone rowid-lookup TableFunction for read_duckdb. Shares
//! MultiFileBindData shape with read_duckdb (caller passes a pre-bound
//! bind_data via TableFunctionInput::bind_data). Its global state opens the
//! source database once; pk_lookups (sorted duckdb row ids) arrive per call
//! via TableFunctionInput and are fetched through DataTable::Fetch under the
//! caller's transaction. Rows deleted/compacted since CREATE INDEX surface as
//! NULLs rather than garbage.
TableFunction MakeDuckDBLookupTableFunction();

} // namespace duckdb
