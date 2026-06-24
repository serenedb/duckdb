//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/statement_preprocessor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "duckdb/common/enums/current_transaction_state.hpp"

namespace duckdb {
class ClientContext;
class ClientContextLock;
class SQLStatement;
struct PragmaInfo;
//! Preprocesses parsed statements: expands pragmas, unpacks multi-statements, and wraps in transactions
class StatementPreprocessor {
public:
	explicit StatementPreprocessor(ClientContext &context);
	//! wrap_multi: when false, an unpacked multi-statement body is NOT wrapped in a BEGIN/COMMIT (or
	//! invalidation-policy) block -- the caller takes responsibility for the transaction (the pg-wire extended Parse
	//! runs the body in its own implicit block, so one user command that expands stays a single prepared unit).
	void Preprocess(ClientContextLock &lock, vector<unique_ptr<SQLStatement>> &statements,
	                CurrentTransactionState transaction_context_state, bool wrap_multi = true);
	void PreprocessInternal(ClientContextLock &lock, vector<unique_ptr<SQLStatement>> &statements,
	                        CurrentTransactionState transaction_context_state, bool wrap_multi = true);

private:
	ClientContext &context;

private:
	//! Handles a pragma statement, determines whether the statement needs reparsing, if it does, it returns the
	//! statement(s) to replace the current one. Otherwise, it just returns back the original statement in a vector.
	vector<unique_ptr<SQLStatement>> TryReparsePragma(unique_ptr<SQLStatement> statement) const;
};
} // namespace duckdb
