////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "duckdb/execution/operator/order/physical_presorted_merge.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

namespace {

class PresortedMergeGlobalState : public GlobalSourceState {
public:
	explicit PresortedMergeGlobalState(idx_t partitions_p) : partitions(partitions_p) {
	}

	idx_t MaxThreads() override {
		return MaxValue<idx_t>(partitions, 1);
	}

	const idx_t partitions;
	atomic<idx_t> next {0};
};

class PresortedMergeLocalState : public LocalSourceState {
public:
	optional_idx partition;
	unique_ptr<PresortedMergeState> state;
	vector<PresortedRun> runs;
};

} // namespace

PhysicalPresortedMerge::PhysicalPresortedMerge(PhysicalPlan &physical_plan, shared_ptr<PresortedMerge> merge_p,
                                               idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, TYPE, RunTypes(), estimated_cardinality), merge(std::move(merge_p)) {
}

vector<LogicalType> PhysicalPresortedMerge::RunTypes() {
	return {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
}

unique_ptr<GlobalSourceState> PhysicalPresortedMerge::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<PresortedMergeGlobalState>(merge->PartitionCount());
}

unique_ptr<LocalSourceState> PhysicalPresortedMerge::GetLocalSourceState(ExecutionContext &context,
                                                                         GlobalSourceState &gstate) const {
	return make_uniq<PresortedMergeLocalState>();
}

SourceResultType PhysicalPresortedMerge::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<PresortedMergeGlobalState>();
	auto &lstate = input.local_state.Cast<PresortedMergeLocalState>();
	auto &client = context.client;
	const std::function<bool()> proceed = [&client]() {
		return !client.IsInterrupted();
	};
	lstate.runs.clear();
	while (lstate.runs.empty()) {
		if (!lstate.state || lstate.state->Done()) {
			const auto partition = gstate.next++;
			if (partition >= gstate.partitions) {
				return SourceResultType::FINISHED;
			}
			lstate.partition = partition;
			lstate.state = merge->BeginMerge(partition);
		}
		optional_idx unsorted_input;
		if (!merge->Merge(*lstate.state, lstate.runs, STANDARD_VECTOR_SIZE, proceed, unsorted_input)) {
			if (unsorted_input.IsValid()) {
				throw InvalidInputException("Input %llu of a presorted merge is not sorted by the merge keys",
				                            unsorted_input.GetIndex());
			}
			throw InterruptException();
		}
	}
	const auto count = lstate.runs.size();
	auto partitions = FlatVector::Writer<uint64_t>(chunk.data[0], count);
	auto inputs = FlatVector::Writer<uint64_t>(chunk.data[1], count);
	auto begins = FlatVector::Writer<uint64_t>(chunk.data[2], count);
	auto counts = FlatVector::Writer<uint64_t>(chunk.data[3], count);
	for (const auto &run : lstate.runs) {
		partitions.WriteValue(lstate.partition.GetIndex());
		inputs.WriteValue(run.input);
		begins.WriteValue(run.begin);
		counts.WriteValue(run.count);
	}
	chunk.SetCardinality(count);
	return SourceResultType::HAVE_MORE_OUTPUT;
}

OperatorPartitionData PhysicalPresortedMerge::GetPartitionData(ExecutionContext &context, DataChunk &chunk,
                                                               GlobalSourceState &gstate, LocalSourceState &lstate,
                                                               const OperatorPartitionInfo &partition_info) const {
	return OperatorPartitionData(lstate.Cast<PresortedMergeLocalState>().partition.GetIndex());
}

string PhysicalPresortedMerge::GetName() const {
	return "PRESORTED_MERGE";
}

InsertionOrderPreservingMap<string> PhysicalPresortedMerge::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Inputs"] = to_string(merge->InputCount());
	result["Partitions"] = to_string(merge->PartitionCount());
	return result;
}

} // namespace duckdb
