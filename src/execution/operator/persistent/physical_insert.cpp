#include "duckdb/execution/operator/persistent/physical_insert.hpp"
#include "duckdb/catalog/catalog.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/persistent/collection_merger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"

namespace duckdb {

PhysicalInsert::PhysicalInsert(PhysicalPlan &physical_plan, vector<LogicalType> types_p, DuckTableEntry &table,
                               vector<unique_ptr<BoundConstraint>> bound_constraints_p, idx_t estimated_cardinality,
                               bool return_chunk, bool parallel)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::INSERT, std::move(types_p), estimated_cardinality),
      insert_table(&table), insert_types(table.GetTypes()), bound_constraints(std::move(bound_constraints_p)),
      return_chunk(return_chunk), parallel(parallel) {
}

PhysicalInsert::PhysicalInsert(PhysicalPlan &physical_plan, LogicalOperator &op, SchemaCatalogEntry &schema,
                               unique_ptr<BoundCreateTableInfo> info_p, idx_t estimated_cardinality, bool parallel)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::CREATE_TABLE_AS, op.types, estimated_cardinality),
      insert_table(nullptr), return_chunk(false), schema(&schema), info(std::move(info_p)), parallel(parallel) {
	GetInsertInfo(*info, insert_types);
}

void PhysicalInsert::GetInsertInfo(const BoundCreateTableInfo &info, vector<LogicalType> &insert_types) {
	auto &create_info = info.base->Cast<CreateTableInfo>();
	for (auto &col : create_info.columns.Physical()) {
		insert_types.push_back(col.GetType());
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

InsertGlobalState::InsertGlobalState(ClientContext &context, const vector<LogicalType> &return_types,
                                     DuckTableEntry &table)
    : table(table), insert_count(0), return_collection(context, return_types) {
	table.GetStorage().BindIndexes(context);
}

InsertLocalState::InsertLocalState(const vector<unique_ptr<BoundConstraint>> &bound_constraints)
    : collection_index(DConstants::INVALID_INDEX), bound_constraints(bound_constraints) {
}

ConstraintState &InsertLocalState::GetConstraintState(DataTable &table, TableCatalogEntry &table_ref) {
	if (!constraint_state) {
		constraint_state = table.InitializeConstraintState(table_ref, bound_constraints);
	}
	return *constraint_state;
}

unique_ptr<GlobalSinkState> PhysicalInsert::GetGlobalSinkState(ClientContext &context) const {
	optional_ptr<DuckTableEntry> table;
	if (info) {
		// CREATE TABLE AS
		D_ASSERT(!insert_table);
		auto &catalog = schema->catalog;
		table = &catalog.CreateTable(catalog.GetCatalogTransaction(context), *schema.get_mutable(), *info)
		             ->Cast<DuckTableEntry>();
	} else {
		D_ASSERT(insert_table);
		D_ASSERT(insert_table->IsDuckTable());
		table = insert_table.get_mutable();
	}
	auto result = make_uniq<InsertGlobalState>(context, GetTypes(), *table);
	return std::move(result);
}

unique_ptr<LocalSinkState> PhysicalInsert::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<InsertLocalState>(bound_constraints);
}

SinkResultType PhysicalInsert::Sink(ExecutionContext &context, DataChunk &insert_chunk,
                                    OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<InsertGlobalState>();
	auto &lstate = input.local_state.Cast<InsertLocalState>();

	auto &table = gstate.table;
	auto &storage = table.GetStorage();
	insert_chunk.Flatten();

	auto &progress_callback = ClientConfig::GetConfig(context.client).sink_progress_callback;
	if (progress_callback) {
		progress_callback(insert_chunk.size(), insert_chunk.GetAllocationSize());
	}

	if (!parallel) {
		gstate.insert_count += insert_chunk.size();
		if (return_chunk) {
			gstate.return_collection.Append(insert_chunk);
		}
		storage.LocalAppend(table, context.client, insert_chunk, bound_constraints, false);
		return SinkResultType::NEED_MORE_INPUT;
	}

	// Parallel append.
	D_ASSERT(!return_chunk);
	auto &data_table = gstate.table.GetStorage();
	if (!lstate.collection_index.IsValid()) {
		lock_guard<mutex> l(gstate.lock);
		lstate.optimistic_writer = make_uniq<OptimisticDataWriter>(context.client, data_table);
		// Create the local row group collection.
		auto optimistic_collection = lstate.optimistic_writer->CreateCollection(storage, insert_types);
		auto &collection = *optimistic_collection->collection;
		collection.InitializeEmpty();
		collection.InitializeAppend(lstate.local_append_state);

		lstate.collection_index =
		    data_table.CreateOptimisticCollection(context.client, std::move(optimistic_collection));
	}

	auto &local_storage = LocalStorage::Get(context.client, data_table.db);
	data_table.VerifyAppendConstraints(lstate.GetConstraintState(data_table, table), context.client, insert_chunk,
	                                   local_storage.GetStorage(data_table));

	auto &optimistic_collection = data_table.GetOptimisticCollection(context.client, lstate.collection_index);
	auto &collection = *optimistic_collection.collection;
	auto flushed_row_group_idx = collection.Append(insert_chunk, lstate.local_append_state);
	if (flushed_row_group_idx.IsValid()) {
		lstate.optimistic_writer->WriteNewRowGroup(optimistic_collection, flushed_row_group_idx.GetIndex());
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalInsert::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<InsertGlobalState>();
	auto &lstate = input.local_state.Cast<InsertLocalState>();
	auto &client_profiler = QueryProfiler::Get(context.client);
	context.thread.profiler.Flush(*this);
	client_profiler.Flush(context.thread.profiler);

	if (!parallel || !lstate.collection_index.IsValid()) {
		return SinkCombineResultType::FINISHED;
	}

	auto &table = gstate.table;
	auto &storage = table.GetStorage();
	const idx_t row_group_size = storage.GetRowGroupSize();

	// parallel append: finalize the append
	auto tdata = TransactionData::Unversioned();
	auto &data_table = gstate.table.GetStorage();
	auto &optimistic_collection = data_table.GetOptimisticCollection(context.client, lstate.collection_index);
	auto &collection = *optimistic_collection.collection;
	collection.FinalizeAppend(tdata, lstate.local_append_state);

	const auto append_count = collection.GetTotalRows();

	if (append_count >= row_group_size) {
		// flush thread-local optimistic data
		lstate.optimistic_writer->WriteUnflushedRowGroups(optimistic_collection);
		lstate.optimistic_writer->FinalFlush();
	}

	lock_guard<mutex> lock(gstate.lock);
	gstate.insert_count += append_count;
	if (append_count < row_group_size) {
		// we have few rows - defer merging to Finalize, where the leftovers of all threads are compacted together
		gstate.unmerged_collections.push_back(lstate.collection_index);
	} else {
		// we have written rows to disk optimistically - merge directly into the transaction-local storage
		gstate.table.GetStorage().LocalMerge(context.client, gstate.table, optimistic_collection);
		auto &optimistic_writer = gstate.table.GetStorage().GetOptimisticWriter(context.client);
		optimistic_writer.Merge(*lstate.optimistic_writer);
	}

	return SinkCombineResultType::FINISHED;
}

class MergeCollectionsTask : public ExecutorTask {
public:
	MergeCollectionsTask(ClientContext &context, shared_ptr<Event> event_p, CollectionMerger &merger,
	                     OptimisticDataWriter &writer, PhysicalIndex &result, const PhysicalOperator &op)
	    : ExecutorTask(context, std::move(event_p), op), merger(merger), writer(writer), result(result) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		result = merger.Flush(writer);
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "MergeCollectionsTask";
	}

private:
	CollectionMerger &merger;
	OptimisticDataWriter &writer;
	PhysicalIndex &result;
};

class MergeCollectionsEvent : public BasePipelineEvent {
public:
	MergeCollectionsEvent(Pipeline &pipeline_p, ClientContext &context, const PhysicalInsert &op, DuckTableEntry &table,
	                      vector<unique_ptr<CollectionMerger>> mergers_p)
	    : BasePipelineEvent(pipeline_p), context(context), op(op), table(table), mergers(std::move(mergers_p)),
	      merged_collections(mergers.size(), PhysicalIndex(DConstants::INVALID_INDEX)) {
	}

public:
	void Schedule() override {
		auto &data_table = table.GetStorage();
		vector<shared_ptr<Task>> tasks;
		for (idx_t i = 0; i < mergers.size(); i++) {
			writers.push_back(make_uniq<OptimisticDataWriter>(context, data_table));
			tasks.push_back(make_uniq<MergeCollectionsTask>(context, shared_from_this(), *mergers[i], *writers[i],
			                                                merged_collections[i], op));
		}
		SetTasks(std::move(tasks));
	}

	void FinishEvent() override {
		// merge the compacted collections into the transaction-local storage
		auto &data_table = table.GetStorage();
		auto &optimistic_writer = data_table.GetOptimisticWriter(context);
		for (idx_t i = 0; i < mergers.size(); i++) {
			auto &collection = data_table.GetOptimisticCollection(context, merged_collections[i]);
			data_table.LocalMerge(context, table, collection);
			data_table.ResetOptimisticCollection(context, merged_collections[i]);
			optimistic_writer.Merge(*writers[i]);
		}
		optimistic_writer.FinalFlush();
	}

private:
	ClientContext &context;
	const PhysicalInsert &op;
	DuckTableEntry &table;
	vector<unique_ptr<CollectionMerger>> mergers;
	vector<unique_ptr<OptimisticDataWriter>> writers;
	vector<PhysicalIndex> merged_collections;
};

SinkFinalizeType PhysicalInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                          OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<InsertGlobalState>();
	if (gstate.unmerged_collections.empty()) {
		return SinkFinalizeType::READY;
	}
	auto &table = gstate.table;
	auto &data_table = table.GetStorage();
	const idx_t row_group_size = data_table.GetRowGroupSize();

	if (gstate.insert_count < row_group_size) {
		// we are inserting a small amount of data - append directly to the transaction-local storage
		LocalAppendState append_state;
		data_table.InitializeLocalAppend(append_state, table, context, bound_constraints);
		auto &transaction = DuckTransaction::Get(context, table.catalog);
		for (auto &collection_index : gstate.unmerged_collections) {
			auto &optimistic_collection = data_table.GetOptimisticCollection(context, collection_index);
			for (auto &insert_chunk : optimistic_collection.collection->Chunks(transaction)) {
				data_table.LocalAppend(append_state, context, insert_chunk, false);
			}
			data_table.ResetOptimisticCollection(context, collection_index);
		}
		data_table.FinalizeLocalAppend(append_state);
		gstate.unmerged_collections.clear();
		return SinkFinalizeType::READY;
	}

	// group the leftover thread-local collections into row-group-sized merge sets
	vector<unique_ptr<CollectionMerger>> mergers;
	unique_ptr<CollectionMerger> current_merger;
	idx_t current_rows = 0;
	for (auto &collection_index : gstate.unmerged_collections) {
		if (!current_merger) {
			current_merger = make_uniq<CollectionMerger>(context, data_table);
		}
		current_merger->AddCollection(collection_index, RowGroupBatchType::NOT_FLUSHED);
		current_rows += data_table.GetOptimisticCollection(context, collection_index).collection->GetTotalRows();
		if (current_rows >= row_group_size) {
			mergers.push_back(std::move(current_merger));
			current_merger.reset();
			current_rows = 0;
		}
	}
	if (current_merger) {
		mergers.push_back(std::move(current_merger));
	}
	gstate.unmerged_collections.clear();

	// compact the merge sets in parallel through a new pipeline event
	auto merge_event = make_shared_ptr<MergeCollectionsEvent>(pipeline, context, *this, table, std::move(mergers));
	event.InsertEvent(std::move(merge_event));
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class InsertSourceState : public GlobalSourceState {
public:
	explicit InsertSourceState(const PhysicalInsert &op) : total_rows(1), rows_scanned(0) {
		if (op.return_chunk) {
			D_ASSERT(op.sink_state);
			auto &g = op.sink_state->Cast<InsertGlobalState>();
			g.return_collection.InitializeScan(scan_state);
			total_rows = g.return_collection.Count();
		}
	}

	ColumnDataScanState scan_state;
	idx_t total_rows;
	atomic<idx_t> rows_scanned;
};

unique_ptr<GlobalSourceState> PhysicalInsert::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<InsertSourceState>(*this);
}

ProgressData PhysicalInsert::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &state = gstate.Cast<InsertSourceState>();
	ProgressData progress;
	progress.total = double(MaxValue<idx_t>(state.total_rows, 1));
	progress.done = state.total_rows == 0 ? 1.0 : double(state.rows_scanned.load(std::memory_order_relaxed));
	return progress;
}

SourceResultType PhysicalInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<InsertSourceState>();
	auto &insert_gstate = sink_state->Cast<InsertGlobalState>();
	if (!return_chunk) {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(insert_gstate.insert_count)));
		state.rows_scanned.store(1, std::memory_order_relaxed);
		return SourceResultType::FINISHED;
	}

	insert_gstate.return_collection.Scan(state.scan_state, chunk);
	state.rows_scanned.fetch_add(chunk.size(), std::memory_order_relaxed);
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
