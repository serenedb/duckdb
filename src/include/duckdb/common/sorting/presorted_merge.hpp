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

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/create_sort_key.hpp"

#include <functional>

namespace duckdb {

class TaskScheduler;

class PresortedMergeReader {
public:
	virtual ~PresortedMergeReader() = default;

	virtual void Read(idx_t begin, idx_t count, DataChunk &keys) = 0;
};

class PresortedMergeInput {
public:
	virtual ~PresortedMergeInput() = default;

	virtual idx_t Count() const = 0;
	virtual unique_ptr<PresortedMergeReader> CreateReader() const = 0;
};

struct PresortedRun {
	idx_t input;
	idx_t begin;
	idx_t count;
};

class PresortedMergeState {
public:
	virtual ~PresortedMergeState() = default;

	bool Done() const {
		return done;
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}

protected:
	bool done = false;
};

class PresortedMerge {
public:
	PresortedMerge(vector<LogicalType> key_types, vector<OrderModifiers> modifiers,
	               vector<shared_ptr<PresortedMergeInput>> inputs);
	explicit PresortedMerge(vector<shared_ptr<PresortedMergeInput>> inputs);
	~PresortedMerge();

	PresortedMerge(const PresortedMerge &) = delete;
	PresortedMerge &operator=(const PresortedMerge &) = delete;

	void Partition(idx_t max_partitions, bool keep_equal_keys = false);
	//! Partition with the reads of the inputs spread over at most `threads` tasks of `scheduler`
	void Partition(idx_t max_partitions, bool keep_equal_keys, TaskScheduler &scheduler, idx_t threads);
	idx_t PartitionCount() const;
	idx_t PartitionBegin(idx_t partition, idx_t input) const;
	unique_ptr<PresortedMergeState> BeginMerge(idx_t partition) const;
	bool Merge(PresortedMergeState &state, vector<PresortedRun> &runs, idx_t max_runs,
	           const std::function<bool()> &proceed, optional_idx &unsorted_input) const;
	bool Merge(idx_t partition, vector<PresortedRun> &runs, const std::function<bool()> &proceed,
	           optional_idx &unsorted_input) const;

	const vector<LogicalType> &KeyTypes() const {
		return key_types;
	}
	const vector<OrderModifiers> &Modifiers() const {
		return modifiers;
	}
	idx_t InputCount() const {
		return inputs.size();
	}

private:
	void PartitionOn(idx_t max_partitions, bool keep_equal_keys, optional_ptr<TaskScheduler> scheduler, idx_t threads);

	vector<LogicalType> key_types;
	vector<OrderModifiers> modifiers;
	vector<shared_ptr<PresortedMergeInput>> inputs;
	vector<vector<idx_t>> bounds;
	bool byte_keys = false;
};

} // namespace duckdb
