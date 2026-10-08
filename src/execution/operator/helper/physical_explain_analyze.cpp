#include "duckdb/execution/operator/helper/physical_explain_analyze.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb {

void AppendExplainLines(const string &text, DataChunk &chunk, ColumnDataCollection &collection) {
	idx_t pos = 0;
	while (pos < text.size()) {
		auto nl = text.find('\n', pos);
		auto line = text.substr(pos, nl == string::npos ? string::npos : nl - pos);
		pos = nl == string::npos ? text.size() : nl + 1;
		if (line.empty()) {
			continue;
		}
		chunk.SetValue(0, chunk.size(), Value(std::move(line)));
		chunk.SetCardinality(chunk.size() + 1);
		if (chunk.size() == STANDARD_VECTOR_SIZE) {
			collection.Append(chunk);
			chunk.Reset();
		}
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class ExplainAnalyzeStateGlobalState : public GlobalSinkState {
public:
	ExplainAnalyzeStateGlobalState(ClientContext &context, const vector<LogicalType> &types)
	    : collection(context, types) {
	}

	ColumnDataCollection collection;
};

SinkResultType PhysicalExplainAnalyze::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType PhysicalExplainAnalyze::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<ExplainAnalyzeStateGlobalState>();
	auto &profiler = QueryProfiler::Get(context);
	profiler.FinalizeMetrics();
	auto analyzed_plan = profiler.ToString(format);

	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), types);
	if (types.size() == 1) {
		// PG shape: one row per plan line.
		AppendExplainLines(analyzed_plan, chunk, gstate.collection);
	} else {
		// DuckDB native shape: a single {key, value} row.
		chunk.SetValue(0, 0, Value("analyzed_plan"));
		chunk.SetValue(1, 0, Value(std::move(analyzed_plan)));
		chunk.SetCardinality(1);
	}
	gstate.collection.Append(chunk);
	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> PhysicalExplainAnalyze::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<ExplainAnalyzeStateGlobalState>(context, types);
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class ExplainAnalyzeSourceState : public GlobalSourceState {
public:
	ColumnDataScanState scan_state;
	bool initialized = false;
};

unique_ptr<GlobalSourceState> PhysicalExplainAnalyze::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<ExplainAnalyzeSourceState>();
}

SourceResultType PhysicalExplainAnalyze::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<ExplainAnalyzeStateGlobalState>();
	auto &state = input.global_state.Cast<ExplainAnalyzeSourceState>();
	if (!state.initialized) {
		gstate.collection.InitializeScan(state.scan_state);
		state.initialized = true;
	}
	gstate.collection.Scan(state.scan_state, chunk);
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
