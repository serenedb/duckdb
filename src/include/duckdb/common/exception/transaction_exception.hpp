//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/exception/transaction_exception.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"

namespace duckdb {
class TransactionException : public Exception {
public:
	DUCKDB_API explicit TransactionException(std::string_view msg);
	DUCKDB_API explicit TransactionException(const unordered_map<string, string> &extra_info, const string &msg);

	template <typename... ARGS>
	explicit TransactionException(std::string_view msg, ARGS &&...params)
	    : TransactionException(ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}

	template <typename... ARGS>
	explicit TransactionException(const unordered_map<string, string> &extra_info, const string &msg, ARGS &&...params)
	    : TransactionException(extra_info, ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}
};
} // namespace duckdb
