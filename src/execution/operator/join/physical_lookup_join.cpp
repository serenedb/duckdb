#include "duckdb/execution/operator/join/physical_lookup_join.hpp"

#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"

#include <numeric>

namespace duckdb {

PhysicalLookupJoin::PhysicalLookupJoin(PhysicalPlan &physical_plan, vector<LogicalType> types, PhysicalOperator &ids,
                                       PhysicalTableScan &source, TableFunction lookup_p,
                                       vector<idx_t> row_id_columns_p, vector<string> key_names_p,
                                       idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::LOOKUP_JOIN, std::move(types), estimated_cardinality),
      lookup(std::move(lookup_p)), row_id_columns(std::move(row_id_columns_p)), key_names(std::move(key_names_p)),
      output_columns(this->types.size()) {
	std::iota(output_columns.begin(), output_columns.end(), 0);
	children.push_back(ids);
	children.push_back(source);
}

const PhysicalTableScan &PhysicalLookupJoin::Source() const {
	return children[1].get().Cast<PhysicalTableScan>();
}

class LookupJoinGlobalSinkState : public GlobalSinkState {
public:
	vector<unique_ptr<DataChunk>> input;
	idx_t count = 0;
	unique_ptr<GlobalTableFunctionState> lookup_state;
	DataChunk ids;
	vector<sel_t> order;
	idx_t batch_size = 0;
	idx_t batch_count = 0;
	atomic<idx_t> next_batch {0};
	mutex lock;
	DataChunk fetched;
	idx_t found = 0;
	vector<idx_t> fetched_row;
};

unique_ptr<GlobalSinkState> PhysicalLookupJoin::GetGlobalSinkState(ClientContext &context) const {
	auto result = make_uniq<LookupJoinGlobalSinkState>();
	if (lookup.init_global) {
		auto &source = Source();
		TableFunctionInitInput input(source.bind_data.get(), source.column_ids, source.projection_ids, nullptr, nullptr,
		                             this);
		result->lookup_state = lookup.init_global(context, input);
	}
	return std::move(result);
}

SinkResultType PhysicalLookupJoin::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &sink = input.global_state.Cast<LookupJoinGlobalSinkState>();
	auto copy = make_uniq<DataChunk>();
	copy->Initialize(context.client, chunk.GetTypes(), chunk.size());
	chunk.Copy(*copy);
	sink.count += copy->size();
	sink.input.push_back(std::move(copy));
	return SinkResultType::NEED_MORE_INPUT;
}

class LookupJoinFetchTask : public ExecutorTask {
public:
	LookupJoinFetchTask(shared_ptr<Event> event_p, ClientContext &context, const PhysicalLookupJoin &op_p,
	                    LookupJoinGlobalSinkState &sink_p, Pipeline &pipeline_p)
	    : ExecutorTask(context, std::move(event_p), op_p), lookup_join(op_p), sink(sink_p), pipeline(pipeline_p) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		ExecutionContext execution(pipeline.GetClientContext(), *thread_context, &pipeline);
		lookup_join.FetchBatches(execution, sink);
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "LookupJoinFetchTask";
	}

private:
	const PhysicalLookupJoin &lookup_join;
	LookupJoinGlobalSinkState &sink;
	Pipeline &pipeline;
};

class LookupJoinFetchEvent : public BasePipelineEvent {
public:
	LookupJoinFetchEvent(const PhysicalLookupJoin &op_p, LookupJoinGlobalSinkState &sink_p, Pipeline &pipeline_p)
	    : BasePipelineEvent(pipeline_p), lookup_join(op_p), sink(sink_p) {
	}

	void Schedule() override {
		auto &context = pipeline->GetClientContext();
		auto tasks = MinValue<idx_t>(sink.batch_count, TaskScheduler::QueryThreads(context));
		if (sink.lookup_state) {
			tasks = MinValue<idx_t>(tasks, sink.lookup_state->MaxThreads());
		}
		vector<shared_ptr<Task>> fetch_tasks;
		for (idx_t t = 0; t < MaxValue<idx_t>(tasks, 1); t++) {
			fetch_tasks.push_back(
			    make_uniq<LookupJoinFetchTask>(shared_from_this(), context, lookup_join, sink, *pipeline));
		}
		SetTasks(std::move(fetch_tasks));
	}

	void FinishEvent() override {
		sink.fetched.SetCardinality(sink.found);
	}

private:
	const PhysicalLookupJoin &lookup_join;
	LookupJoinGlobalSinkState &sink;
};

SinkFinalizeType PhysicalLookupJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                              OperatorSinkFinalizeInput &input) const {
	auto &sink = input.global_state.Cast<LookupJoinGlobalSinkState>();
	if (sink.count == 0) {
		return SinkFinalizeType::READY;
	}
	vector<LogicalType> id_types;
	vector<OrderModifiers> modifiers;
	for (auto column : row_id_columns) {
		id_types.push_back(children[0].get().GetTypes()[column]);
		modifiers.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);
	}
	sink.ids.Initialize(context, id_types, sink.count);
	idx_t offset = 0;
	for (auto &chunk : sink.input) {
		for (idx_t i = 0; i < row_id_columns.size(); i++) {
			VectorOperations::Copy(chunk->data[row_id_columns[i]], sink.ids.data[i], chunk->size(), 0, offset);
		}
		offset += chunk->size();
	}
	sink.ids.SetCardinality(sink.count);
	Vector keys(LogicalType::BLOB, sink.count);
	CreateSortKeyHelpers::CreateSortKey(sink.ids, modifiers, keys);
	auto key_data = FlatVector::GetData<string_t>(keys);
	sink.order.resize(sink.count);
	std::iota(sink.order.begin(), sink.order.end(), 0);
	std::sort(sink.order.begin(), sink.order.end(),
	          [&](sel_t a, sel_t b) { return LessThan::Operation(key_data[a], key_data[b]); });

	sink.batch_count = (sink.count + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
	sink.batch_size = (sink.count + sink.batch_count - 1) / sink.batch_count;
	sink.fetched.Initialize(context, Source().GetTypes(), sink.count);
	sink.fetched_row.assign(sink.count, DConstants::INVALID_INDEX);
	event.InsertEvent(make_shared_ptr<LookupJoinFetchEvent>(*this, sink, pipeline));
	return SinkFinalizeType::READY;
}

void PhysicalLookupJoin::FetchBatches(ExecutionContext &context, GlobalSinkState &sink_p) const {
	auto &sink = sink_p.Cast<LookupJoinGlobalSinkState>();
	auto &source = Source();
	unique_ptr<LocalTableFunctionState> local_state;
	if (lookup.init_local) {
		TableFunctionInitInput input(source.bind_data.get(), source.column_ids, source.projection_ids, nullptr, nullptr,
		                             this);
		local_state = lookup.init_local(context, input, sink.lookup_state.get());
	}
	DataChunk ids;
	ids.InitializeEmpty(sink.ids.GetTypes());
	DataChunk fetched;
	fetched.Initialize(context.client, source.GetTypes());
	auto survivors = make_unsafe_uniq_array_uninitialized<idx_t>(STANDARD_VECTOR_SIZE);
	for (auto batch = sink.next_batch++; batch < sink.batch_count; batch = sink.next_batch++) {
		const auto begin = batch * sink.batch_size;
		const auto count = MinValue<idx_t>(sink.batch_size, sink.count - begin);
		ids.Slice(sink.ids, SelectionVector(sink.order.data() + begin, count), count);
		fetched.Reset();
		TableFunctionInput data(source.bind_data.get(), local_state.get(), sink.lookup_state.get());
		data.pk_survivors = std::span<idx_t>(survivors.get(), count);
		lookup.in_out_function(context, data, ids, fetched);

		const auto found = fetched.size();
		if (context.thread.profiler.IsEnabled()) {
			context.thread.profiler.GetOperatorMetrics(source).elements_returned += found;
		}
		lock_guard<mutex> guard(sink.lock);
		for (idx_t c = 0; c < fetched.ColumnCount(); c++) {
			VectorOperations::Copy(fetched.data[c], sink.fetched.data[c], found, 0, sink.found);
		}
		for (idx_t r = 0; r < found; r++) {
			sink.fetched_row[sink.order[begin + survivors[r]]] = sink.found + r;
		}
		sink.found += found;
	}
}

class LookupJoinGlobalSourceState : public GlobalSourceState {
public:
	idx_t chunk_index = 0;
	idx_t position = 0;
	SelectionVector input_sel {STANDARD_VECTOR_SIZE};
	SelectionVector fetched_sel {STANDARD_VECTOR_SIZE};
};

unique_ptr<GlobalSourceState> PhysicalLookupJoin::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<LookupJoinGlobalSourceState>();
}

SourceResultType PhysicalLookupJoin::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                     OperatorSourceInput &input) const {
	auto &sink = sink_state->Cast<LookupJoinGlobalSinkState>();
	auto &state = input.global_state.Cast<LookupJoinGlobalSourceState>();
	while (state.chunk_index < sink.input.size()) {
		auto &ids = *sink.input[state.chunk_index++];
		idx_t found = 0;
		for (idx_t r = 0; r < ids.size(); r++) {
			const auto row = sink.fetched_row[state.position + r];
			if (row == DConstants::INVALID_INDEX) {
				continue;
			}
			state.input_sel.set_index(found, r);
			state.fetched_sel.set_index(found, row);
			found++;
		}
		state.position += ids.size();
		if (found == 0) {
			continue;
		}
		for (idx_t c = 0; c < output_columns.size(); c++) {
			const auto column = output_columns[c];
			if (column < ids.ColumnCount()) {
				chunk.data[c].Slice(ids.data[column], state.input_sel, found);
			} else {
				chunk.data[c].Slice(sink.fetched.data[column - ids.ColumnCount()], state.fetched_sel, found);
			}
		}
		chunk.SetCardinality(found);
		return state.chunk_index < sink.input.size() ? SourceResultType::HAVE_MORE_OUTPUT : SourceResultType::FINISHED;
	}
	return SourceResultType::FINISHED;
}

InsertionOrderPreservingMap<string> PhysicalLookupJoin::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Key"] = StringUtil::Join(key_names, ", ");
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

void PhysicalLookupJoin::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();
	meta_pipeline.GetState().SetPipelineSource(current, *this);
	auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	child_meta_pipeline.Build(children[0].get());
}

vector<const_reference<PhysicalOperator>> PhysicalLookupJoin::GetSources() const {
	return {*this};
}

} // namespace duckdb
