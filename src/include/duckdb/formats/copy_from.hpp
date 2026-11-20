//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/copy_from.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/formats/copy_options.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class CopyNotices : public ClientContextState {
public:
	static constexpr const char *NAME = "copy_notices";

	virtual void Notice(const string &message) = 0;
};

struct CopyFromRules {
	CopyFormatOptions options;
	string table;
	vector<string> names;
	vector<bool> force_not_null;
	vector<bool> force_null;
	vector<unique_ptr<Expression>> defaults;

	bool Text() const {
		return options.format == CopyTextFormat::TEXT;
	}
};

unique_ptr<FunctionData> PostgresCopyFromBind(ClientContext &context, CopyFromFunctionBindInput &input,
                                              vector<Identifier> &expected_names, vector<LogicalType> &expected_types,
                                              CopyTextFormat format);

bool CopyFromKeepsOrder(ClientContext &context, const CopyInfo &info);

} // namespace duckdb
