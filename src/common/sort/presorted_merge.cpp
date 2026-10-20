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

#include "duckdb/common/sorting/presorted_merge.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/parallel/task_executor.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace duckdb {

namespace {

constexpr idx_t SAMPLES_PER_PARTITION = 4;

int CompareSizes(const string_t &a, const string_t &b) {
	return a.GetSize() < b.GetSize() ? -1 : (a.GetSize() > b.GetSize() ? 1 : 0);
}

int CompareKeys(const string_t &a, const string_t &b) {
	const auto a_prefix = a.GetPrefixIntegerComparable();
	const auto b_prefix = b.GetPrefixIntegerComparable();
	if (a_prefix != b_prefix) {
		return a_prefix < b_prefix ? -1 : 1;
	}
	if (a.IsInlined() && b.IsInlined()) {
		const auto a_rest = BSwapIfLE(Load<uint64_t>(const_data_ptr_cast(a.GetPrefix()) + string_t::PREFIX_BYTES));
		const auto b_rest = BSwapIfLE(Load<uint64_t>(const_data_ptr_cast(b.GetPrefix()) + string_t::PREFIX_BYTES));
		if (a_rest != b_rest) {
			return a_rest < b_rest ? -1 : 1;
		}
		return CompareSizes(a, b);
	}
	const auto cmp = memcmp(a.GetData(), b.GetData(), MinValue(a.GetSize(), b.GetSize()));
	if (cmp != 0) {
		return cmp < 0 ? -1 : 1;
	}
	return CompareSizes(a, b);
}

string_t View(const string &key) {
	return string_t(key.data(), UnsafeNumericCast<uint32_t>(key.size()));
}

class KeyEncoder {
public:
	KeyEncoder(const vector<LogicalType> &types_p, const vector<OrderModifiers> &modifiers_p, bool byte_keys_p)
	    : types(types_p), modifiers(modifiers_p), byte_keys(byte_keys_p) {
		chunk.Initialize(Allocator::DefaultAllocator(), types);
	}

	unique_ptr<Vector> Load(PresortedMergeReader &reader, idx_t begin, idx_t count) {
		if (byte_keys) {
			DataChunk bytes;
			bytes.Initialize(Allocator::DefaultAllocator(), types, count);
			reader.Read(begin, count, bytes);
			bytes.data[0].Flatten();
			return make_uniq<Vector>(std::move(bytes.data[0]));
		}
		chunk.Reset();
		reader.Read(begin, count, chunk);
		auto keys = make_uniq<Vector>(LogicalType::BLOB, count);
		CreateSortKeyHelpers::CreateSortKey(chunk, modifiers, *keys);
		return keys;
	}

	string Encode(PresortedMergeReader &reader, idx_t row) {
		const auto keys = Load(reader, row, 1);
		const auto &key = FlatVector::GetData<string_t>(*keys)[0];
		return string(key.GetData(), key.GetSize());
	}

private:
	const vector<LogicalType> &types;
	const vector<OrderModifiers> &modifiers;
	const bool byte_keys;
	DataChunk chunk;
};

struct KeyRow {
	string key;
	idx_t input;
	idx_t row;
};

bool Before(const KeyRow &a, const KeyRow &b) {
	const auto cmp = CompareKeys(View(a.key), View(b.key));
	if (cmp != 0) {
		return cmp < 0;
	}
	return a.input != b.input ? a.input < b.input : a.row < b.row;
}

using InputWork = std::function<void(idx_t input, KeyEncoder &encoder)>;

class InputTask : public BaseExecutorTask {
public:
	InputTask(TaskExecutor &executor, atomic<idx_t> &next, idx_t count, const InputWork &work,
	          const vector<LogicalType> &types, const vector<OrderModifiers> &modifiers, bool byte_keys)
	    : BaseExecutorTask(executor), next(next), count(count), work(work), types(types), modifiers(modifiers),
	      byte_keys(byte_keys) {
	}

	void ExecuteTask() override {
		KeyEncoder encoder(types, modifiers, byte_keys);
		for (auto input = next++; input < count && !executor.HasError(); input = next++) {
			work(input, encoder);
		}
	}

	string TaskType() const override {
		return "PresortedPartitionTask";
	}

private:
	atomic<idx_t> &next;
	const idx_t count;
	const InputWork &work;
	const vector<LogicalType> &types;
	const vector<OrderModifiers> &modifiers;
	const bool byte_keys;
};

void ForEachInput(idx_t count, optional_ptr<TaskScheduler> scheduler, idx_t threads, const vector<LogicalType> &types,
                  const vector<OrderModifiers> &modifiers, bool byte_keys, const InputWork &work) {
	const auto tasks = MinValue(threads, count);
	if (!scheduler || tasks < 2) {
		KeyEncoder encoder(types, modifiers, byte_keys);
		for (idx_t input = 0; input < count; input++) {
			work(input, encoder);
		}
		return;
	}
	atomic<idx_t> next {0};
	TaskExecutor executor(*scheduler);
	for (idx_t task = 0; task < tasks; task++) {
		executor.ScheduleTask(make_uniq<InputTask>(executor, next, count, work, types, modifiers, byte_keys));
	}
	executor.WorkOnTasks();
}

struct Cursor {
	idx_t input;
	idx_t next;
	idx_t end;
	unique_ptr<PresortedMergeReader> reader;
	unique_ptr<Vector> keys;
	const string_t *data = nullptr;
	idx_t window_begin = 0;
	idx_t size = 0;
	idx_t at = 0;
	string carry;
	bool has_carry = false;
	bool done = false;

	const string_t &Head() const {
		return data[at];
	}
	idx_t HeadRow() const {
		return window_begin + at;
	}
	idx_t WindowEnd() const {
		return window_begin + size;
	}
};

class CursorMerge : public PresortedMergeState {
public:
	CursorMerge(vector<Cursor> cursors_p, const vector<LogicalType> &types, const vector<OrderModifiers> &modifiers,
	            bool byte_keys)
	    : cursors(std::move(cursors_p)), encoder(types, modifiers, byte_keys), losers(cursors.size()) {
		done = cursors.empty();
	}

	bool Run(vector<PresortedRun> &runs, idx_t max_runs, const std::function<bool()> &proceed_p,
	         optional_idx &unsorted_input) {
		if (done) {
			return true;
		}
		proceed = &proceed_p;
		if (!started) {
			started = true;
			for (auto &cursor : cursors) {
				if (!Load(cursor, unsorted_input)) {
					return false;
				}
			}
			winner = Build(1);
			last = cursors.size();
		}
		while (!cursors[winner].done && runs.size() < max_runs) {
			const auto current = winner;
			auto &cursor = cursors[current];
			const auto begin = cursor.HeadRow();
			idx_t end = begin + 1;
			string probe_carry;
			if (current == last) {
				end = RunEnd(cursor, RunnerUp(), probe_carry);
			}
			Append(runs, cursor.input, begin, end - begin);
			if (!Advance(cursor, end, probe_carry, unsorted_input)) {
				return false;
			}
			Replay();
			last = current;
		}
		done = cursors[winner].done;
		return true;
	}

private:
	bool Load(Cursor &cursor, optional_idx &unsorted_input) {
		if (cursor.next == cursor.end) {
			cursor.done = true;
			return true;
		}
		if (!(*proceed)()) {
			return false;
		}
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, cursor.end - cursor.next);
		cursor.keys = encoder.Load(*cursor.reader, cursor.next, count);
		cursor.data = FlatVector::GetData<string_t>(*cursor.keys);
		cursor.window_begin = cursor.next;
		cursor.size = count;
		cursor.at = 0;
		cursor.next += count;
		if (cursor.has_carry && CompareKeys(View(cursor.carry), cursor.data[0]) > 0) {
			unsorted_input = cursor.input;
			return false;
		}
		for (idx_t i = 1; i < count; i++) {
			if (CompareKeys(cursor.data[i - 1], cursor.data[i]) > 0) {
				unsorted_input = cursor.input;
				return false;
			}
		}
		return true;
	}

	bool Advance(Cursor &cursor, idx_t end, string &probe_carry, optional_idx &unsorted_input) {
		if (end < cursor.WindowEnd()) {
			cursor.at = end - cursor.window_begin;
			return true;
		}
		if (end == cursor.WindowEnd()) {
			const auto &last = cursor.data[cursor.size - 1];
			cursor.carry = string(last.GetData(), last.GetSize());
		} else {
			cursor.carry = std::move(probe_carry);
		}
		cursor.has_carry = true;
		cursor.next = end;
		return Load(cursor, unsorted_input);
	}

	static void Append(vector<PresortedRun> &runs, idx_t input, idx_t begin, idx_t count) {
		if (!runs.empty() && runs.back().input == input && runs.back().begin + runs.back().count == begin) {
			runs.back().count += count;
			return;
		}
		runs.push_back({input, begin, count});
	}

	bool HeadBefore(idx_t a, idx_t b) const {
		const auto &ca = cursors[a];
		const auto &cb = cursors[b];
		if (ca.done || cb.done) {
			return !ca.done;
		}
		const auto cmp = CompareKeys(ca.Head(), cb.Head());
		return cmp < 0 || (cmp == 0 && ca.input < cb.input);
	}

	idx_t Build(idx_t node) {
		if (node >= cursors.size()) {
			return node - cursors.size();
		}
		const auto left = Build(2 * node);
		const auto right = Build(2 * node + 1);
		if (HeadBefore(right, left)) {
			losers[node] = left;
			return right;
		}
		losers[node] = right;
		return left;
	}

	void Replay() {
		auto current = winner;
		for (auto node = (cursors.size() + current) / 2; node != 0; node /= 2) {
			if (HeadBefore(losers[node], current)) {
				std::swap(losers[node], current);
			}
		}
		winner = current;
	}

	idx_t RunnerUp() const {
		auto best = cursors.size();
		for (auto node = (cursors.size() + winner) / 2; node != 0; node /= 2) {
			if (best == cursors.size() || HeadBefore(losers[node], best)) {
				best = losers[node];
			}
		}
		return best;
	}

	idx_t RunEnd(Cursor &cursor, idx_t rival, string &probe_carry) {
		if (rival == cursors.size() || cursors[rival].done) {
			return cursor.end;
		}
		const auto &limit = cursors[rival].Head();
		const bool ties_first = cursor.input < cursors[rival].input;
		const auto before = [&](const string_t &key) {
			const auto cmp = CompareKeys(key, limit);
			return cmp < 0 || (cmp == 0 && ties_first);
		};
		if (cursor.at + 1 < cursor.size && !before(cursor.data[cursor.at + 1])) {
			return cursor.HeadRow() + 1;
		}
		if (!before(cursor.data[cursor.size - 1])) {
			idx_t lo = cursor.at + 2;
			idx_t hi = cursor.size - 1;
			while (lo < hi) {
				const auto mid = lo + (hi - lo) / 2;
				if (before(cursor.data[mid])) {
					lo = mid + 1;
				} else {
					hi = mid;
				}
			}
			return cursor.window_begin + lo;
		}
		idx_t known = cursor.WindowEnd();
		idx_t hi = cursor.end;
		for (idx_t step = STANDARD_VECTOR_SIZE; known < cursor.end; step *= 2) {
			const auto probe = MinValue(known + step - 1, cursor.end - 1);
			auto key = encoder.Encode(*cursor.reader, probe);
			if (!before(View(key))) {
				hi = probe;
				break;
			}
			probe_carry = std::move(key);
			known = probe + 1;
		}
		while (known < hi) {
			const auto mid = known + (hi - known) / 2;
			auto key = encoder.Encode(*cursor.reader, mid);
			if (before(View(key))) {
				probe_carry = std::move(key);
				known = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (probe_carry.empty()) {
			const auto &last = cursor.data[cursor.size - 1];
			probe_carry = string(last.GetData(), last.GetSize());
		}
		return known;
	}

	vector<Cursor> cursors;
	KeyEncoder encoder;
	const std::function<bool()> *proceed = nullptr;
	vector<idx_t> losers;
	idx_t winner = 0;
	idx_t last = 0;
	bool started = false;
};

} // namespace

PresortedMerge::PresortedMerge(vector<LogicalType> key_types_p, vector<OrderModifiers> modifiers_p,
                               vector<shared_ptr<PresortedMergeInput>> inputs_p)
    : key_types(std::move(key_types_p)), modifiers(std::move(modifiers_p)), inputs(std::move(inputs_p)) {
	D_ASSERT(key_types.size() == modifiers.size());
}

PresortedMerge::PresortedMerge(vector<shared_ptr<PresortedMergeInput>> inputs_p)
    : key_types({LogicalType::BLOB}), inputs(std::move(inputs_p)), byte_keys(true) {
}

PresortedMerge::~PresortedMerge() {
}

void PresortedMerge::Partition(idx_t max_partitions, bool keep_equal_keys) {
	PartitionOn(max_partitions, keep_equal_keys, nullptr, 1);
}

void PresortedMerge::Partition(idx_t max_partitions, bool keep_equal_keys, TaskScheduler &scheduler, idx_t threads) {
	PartitionOn(max_partitions, keep_equal_keys, &scheduler, threads);
}

void PresortedMerge::PartitionOn(idx_t max_partitions, bool keep_equal_keys, optional_ptr<TaskScheduler> scheduler,
                                 idx_t threads) {
	bounds.clear();
	bounds.emplace_back(inputs.size(), 0);
	idx_t total = 0;
	for (const auto &input : inputs) {
		total += input->Count();
	}
	if (max_partitions > 1 && total != 0) {
		const auto per_input = max_partitions * SAMPLES_PER_PARTITION;
		vector<unique_ptr<PresortedMergeReader>> readers(inputs.size());
		vector<vector<KeyRow>> sampled(inputs.size());
		ForEachInput(inputs.size(), scheduler, threads, key_types, modifiers, byte_keys,
		             [&](idx_t i, KeyEncoder &encoder) {
			             const auto count = inputs[i]->Count();
			             if (count == 0) {
				             return;
			             }
			             readers[i] = inputs[i]->CreateReader();
			             for (idx_t s = 0; s < per_input; s++) {
				             const auto row = (2 * s + 1) * count / (2 * per_input);
				             sampled[i].push_back({encoder.Encode(*readers[i], row), i, row});
			             }
		             });
		vector<reference<const KeyRow>> samples;
		for (const auto &rows : sampled) {
			samples.insert(samples.end(), rows.begin(), rows.end());
		}
		std::sort(samples.begin(), samples.end(), [](const KeyRow &a, const KeyRow &b) { return Before(a, b); });
		const auto step = static_cast<double>(total) / static_cast<double>(max_partitions);
		auto next = step;
		double weight = 0;
		vector<reference<const KeyRow>> splits;
		for (const KeyRow &sample : samples) {
			weight += static_cast<double>(inputs[sample.input]->Count()) / static_cast<double>(per_input);
			if (weight < next || splits.size() + 1 == max_partitions) {
				continue;
			}
			next += step;
			splits.push_back(sample);
		}
		vector<vector<idx_t>> located(splits.size(), vector<idx_t>(inputs.size(), 0));
		ForEachInput(
		    inputs.size(), scheduler, threads, key_types, modifiers, byte_keys, [&](idx_t i, KeyEncoder &encoder) {
			    if (!readers[i]) {
				    return;
			    }
			    const auto &own = sampled[i];
			    idx_t lo = 0;
			    for (idx_t s = 0; s < splits.size(); s++) {
				    const KeyRow &split = splits[s];
				    const auto before = [&](const KeyRow &row) {
					    return keep_equal_keys ? CompareKeys(View(row.key), View(split.key)) < 0 : Before(row, split);
				    };
				    const auto first = std::partition_point(own.begin(), own.end(), before);
				    if (first != own.begin()) {
					    lo = MaxValue(lo, std::prev(first)->row + 1);
				    }
				    auto hi = first != own.end() ? first->row : inputs[i]->Count();
				    while (lo < hi) {
					    const auto mid = lo + (hi - lo) / 2;
					    if (before(KeyRow {encoder.Encode(*readers[i], mid), i, mid})) {
						    lo = mid + 1;
					    } else {
						    hi = mid;
					    }
				    }
				    located[s][i] = lo;
			    }
		    });
		for (auto &positions : located) {
			bounds.push_back(std::move(positions));
		}
	}
	vector<idx_t> last(inputs.size());
	for (idx_t i = 0; i < inputs.size(); i++) {
		last[i] = inputs[i]->Count();
	}
	bounds.push_back(std::move(last));
}

idx_t PresortedMerge::PartitionCount() const {
	return bounds.empty() ? 0 : bounds.size() - 1;
}

idx_t PresortedMerge::PartitionBegin(idx_t partition, idx_t input) const {
	return bounds[partition][input];
}

unique_ptr<PresortedMergeState> PresortedMerge::BeginMerge(idx_t partition) const {
	vector<Cursor> cursors;
	for (idx_t i = 0; i < inputs.size(); i++) {
		const auto begin = bounds[partition][i];
		const auto end = bounds[partition + 1][i];
		if (begin == end) {
			continue;
		}
		Cursor cursor;
		cursor.input = i;
		cursor.next = begin;
		cursor.end = end;
		cursor.reader = inputs[i]->CreateReader();
		cursors.push_back(std::move(cursor));
	}
	return make_uniq<CursorMerge>(std::move(cursors), key_types, modifiers, byte_keys);
}

bool PresortedMerge::Merge(PresortedMergeState &state, vector<PresortedRun> &runs, idx_t max_runs,
                           const std::function<bool()> &proceed, optional_idx &unsorted_input) const {
	return state.Cast<CursorMerge>().Run(runs, max_runs, proceed, unsorted_input);
}

bool PresortedMerge::Merge(idx_t partition, vector<PresortedRun> &runs, const std::function<bool()> &proceed,
                           optional_idx &unsorted_input) const {
	const auto state = BeginMerge(partition);
	return Merge(*state, runs, NumericLimits<idx_t>::Maximum(), proceed, unsorted_input);
}

} // namespace duckdb
