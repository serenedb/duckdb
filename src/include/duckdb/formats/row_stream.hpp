//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/row_stream.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/formats/row_ranges.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

class RowStreamState : public ClientContextState {
public:
	explicit RowStreamState(shared_ptr<RowSource> source_p) : source(std::move(source_p)) {
	}

	shared_ptr<RowSource> source;
};

inline string RowStreamKey(const string &path) {
	return "row_stream:" + path;
}

inline shared_ptr<RowSource> FindRowStream(ClientContext &context, const string &path) {
	auto state = context.registered_state->Get<RowStreamState>(RowStreamKey(path));
	return state ? state->source : nullptr;
}

} // namespace duckdb
