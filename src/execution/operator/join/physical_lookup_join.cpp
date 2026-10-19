#include "duckdb/execution/operator/join/physical_lookup_join.hpp"

#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"

namespace duckdb {

PhysicalLookupJoin::PhysicalLookupJoin(PhysicalPlan &physical_plan, vector<LogicalType> types, PhysicalOperator &ids,
                                       PhysicalTableScan &source, TableFunction lookup_p,
                                       vector<idx_t> row_id_columns_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::LOOKUP_JOIN, std::move(types), estimated_cardinality),
      lookup(std::move(lookup_p)), row_id_columns(std::move(row_id_columns_p)) {
	children.push_back(ids);
	children.push_back(source);
}

const PhysicalTableScan &PhysicalLookupJoin::Source() const {
	return children[1].get().Cast<PhysicalTableScan>();
}

class LookupJoinGlobalState : public GlobalOperatorState {
public:
	idx_t MaxThreads(idx_t source_max_threads) override {
		if (!global_state) {
			return source_max_threads;
		}
		return global_state->MaxThreads();
	}

	unique_ptr<GlobalTableFunctionState> global_state;
};

class LookupJoinLocalState : public OperatorState {
public:
	unique_ptr<LocalTableFunctionState> local_state;
	DataChunk ids;
	DataChunk fetched;
	unsafe_unique_array<idx_t> survivors;
	unsafe_unique_array<idx_t> slots;
	SelectionVector input_sel {STANDARD_VECTOR_SIZE};
	SelectionVector fetched_sel {STANDARD_VECTOR_SIZE};
};

unique_ptr<GlobalOperatorState> PhysicalLookupJoin::GetGlobalOperatorState(ClientContext &context) const {
	auto result = make_uniq<LookupJoinGlobalState>();
	if (lookup.init_global) {
		auto &source = Source();
		TableFunctionInitInput input(source.bind_data.get(), source.column_ids, source.projection_ids, nullptr,
		                             nullptr, this);
		result->global_state = lookup.init_global(context, input);
	}
	return std::move(result);
}

unique_ptr<OperatorState> PhysicalLookupJoin::GetOperatorState(ExecutionContext &context) const {
	auto &gstate = op_state->Cast<LookupJoinGlobalState>();
	auto &source = Source();
	auto result = make_uniq<LookupJoinLocalState>();
	if (lookup.init_local) {
		TableFunctionInitInput input(source.bind_data.get(), source.column_ids, source.projection_ids, nullptr,
		                             nullptr, this);
		result->local_state = lookup.init_local(context, input, gstate.global_state.get());
	}
	vector<LogicalType> id_types;
	auto &child_types = children[0].get().GetTypes();
	for (auto column : row_id_columns) {
		id_types.push_back(child_types[column]);
	}
	result->ids.InitializeEmpty(id_types);
	result->fetched.Initialize(context.client, source.GetTypes());
	result->survivors = make_unsafe_uniq_array_uninitialized<idx_t>(STANDARD_VECTOR_SIZE);
	result->slots = make_unsafe_uniq_array_uninitialized<idx_t>(STANDARD_VECTOR_SIZE);
	return std::move(result);
}

OperatorResultType PhysicalLookupJoin::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                               GlobalOperatorState &gstate_p, OperatorState &state_p) const {
	auto &gstate = gstate_p.Cast<LookupJoinGlobalState>();
	auto &state = state_p.Cast<LookupJoinLocalState>();
	const auto count = input.size();
	for (idx_t i = 0; i < row_id_columns.size(); i++) {
		state.ids.data[i].Reference(input.data[row_id_columns[i]]);
	}
	state.ids.SetCardinalityUnsafe(count);
	state.fetched.Reset();
	TableFunctionInput data(Source().bind_data.get(), state.local_state.get(), gstate.global_state.get());
	data.pk_survivors = std::span<idx_t>(state.survivors.get(), count);
	lookup.in_out_function(context, data, state.ids, state.fetched);

	const auto found = state.fetched.size();
	const auto input_columns = input.ColumnCount();
	bool ascending = true;
	for (idx_t r = 1; r < found && ascending; r++) {
		ascending = state.survivors[r - 1] < state.survivors[r];
	}
	if (ascending && found == count) {
		chunk.SetChildCardinality(found);
		for (idx_t c = 0; c < input_columns; c++) {
			chunk.data[c].Reference(input.data[c]);
		}
		for (idx_t c = 0; c < state.fetched.ColumnCount(); c++) {
			chunk.data[input_columns + c].Reference(state.fetched.data[c]);
		}
		return OperatorResultType::NEED_MORE_INPUT;
	}
	if (ascending) {
		for (idx_t r = 0; r < found; r++) {
			state.input_sel.set_index(r, state.survivors[r]);
			state.fetched_sel.set_index(r, r);
		}
	} else {
		std::fill_n(state.slots.get(), count, DConstants::INVALID_INDEX);
		for (idx_t r = 0; r < found; r++) {
			state.slots[state.survivors[r]] = r;
		}
		idx_t out = 0;
		for (idx_t pos = 0; pos < count; pos++) {
			if (state.slots[pos] == DConstants::INVALID_INDEX) {
				continue;
			}
			state.input_sel.set_index(out, pos);
			state.fetched_sel.set_index(out, state.slots[pos]);
			out++;
		}
		D_ASSERT(out == found);
	}
	chunk.Slice(input, state.input_sel, found);
	chunk.Slice(state.fetched, state.fetched_sel, found, input_columns);
	return OperatorResultType::NEED_MORE_INPUT;
}

InsertionOrderPreservingMap<string> PhysicalLookupJoin::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

void PhysicalLookupJoin::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	meta_pipeline.GetState().AddPipelineOperator(current, *this);
	children[0].get().BuildPipelines(current, meta_pipeline);
}

vector<const_reference<PhysicalOperator>> PhysicalLookupJoin::GetSources() const {
	return children[0].get().GetSources();
}

} // namespace duckdb
