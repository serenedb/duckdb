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

#include "duckdb/storage/compression/dict_fsst/sparse.hpp"

#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/storage/checkpoint/string_checkpoint_state.hpp"
#include "duckdb/storage/compression/dict_fsst/dictionary_cache.hpp"
#include "duckdb/storage/table/column_segment.hpp"

namespace duckdb {
namespace dict_fsst {

namespace {

constexpr idx_t SPARSE_SELECTIVITY = 4;
constexpr idx_t SPARSE_SEGMENT_RATIO = 32;

enum Verdict : uint8_t { UNKNOWN = 0, PASS = 1, FAIL = 2, PENDING = 3 };

//! Scan scratch vectors are reused across windows and may still be a DICTIONARY vector over another
//! segment's buffers, or carry a dirty validity mask: give flat writes their own clean buffer.
void PrepareFlatResult(Vector &result, idx_t count) {
	if (result.GetVectorType() != VectorType::FLAT_VECTOR) {
		result.Initialize(VectorDataInitialization::UNINITIALIZED, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	}
	FlatVector::ValidityMutable(result).SetAllValid(count);
}

bool UseSparse(const CompressedStringScanState &scan_state, idx_t vector_count, idx_t sel_count) {
	return scan_state.deferred_dictionary && sel_count * SPARSE_SELECTIVITY <= vector_count &&
	       scan_state.decoded_on_demand + sel_count <= scan_state.dict_count / 2;
}

} // namespace

void CompressedStringScanState::MaterializeDictionary() {
	D_ASSERT(deferred_dictionary && !dictionary);
	deferred_dictionary = false;
	optional_ptr<DictFSSTDictionaryCache> cache;
	if (decoder) {
		if (auto state = segment.GetSegmentState()) {
			cache = state->Cast<UncompressedStringSegmentState>().dictionary_cache;
		}
	}
	if (cache) {
		dictionary = cache->Get();
		if (dictionary) {
			return;
		}
	}
	dictionary = DictionaryVector::CreateReusableDictionary(segment.GetType(), dict_count);
	auto &dict_data = dictionary->data;
	auto dict_child_data = FlatVector::GetDataMutable<string_t>(dict_data);
	FlatVector::ValidityMutable(dict_data).SetInvalid(0);
	auto &allocator = StringVector::GetStringAllocator(dict_data);
	idx_t offset = 0;
	for (uint32_t i = 0; i < dict_count; i++) {
		auto len = entry_lengths[i];
		auto pid = prefix_count > 0 ? prefix_ids[i] : 0;
		dict_child_data[i] = ReconstructEntry(allocator, pid, len, char_ptr_cast(dict_ptr + offset));
		offset += len;
	}
	if (cache) {
		cache->Put(dictionary, dict_count * sizeof(string_t) + allocator.AllocationSize());
	}
}

void CompressedStringScanState::ChargeDecodes(idx_t count) {
	if (!deferred_dictionary) {
		return;
	}
	decoded_on_demand += count;
	if (decoded_on_demand >= dict_count) {
		MaterializeDictionary();
	}
}

void CompressedStringScanState::PrepareRead(idx_t count, idx_t span) {
	if (!deferred_dictionary) {
		return;
	}
	if (count * SPARSE_SELECTIVITY > span || count * SPARSE_SEGMENT_RATIO >= segment.count) {
		MaterializeDictionary();
		return;
	}
	ChargeDecodes(count);
}

void CompressedStringScanState::SelectEntries(Vector &result, idx_t start, idx_t span, const SelectionVector &sel,
                                              idx_t sel_count) {
	D_ASSERT(!dictionary);
	D_ASSERT(mode != DictFSSTMode::FSST_ONLY && mode != DictFSSTMode::FSST_PLUS);
	auto &codes = GetSelVec(start, span, sel, sel_count);
	PrepareFlatResult(result, sel_count);
	auto result_data = FlatVector::Writer<string_t>(result, sel_count);
	auto &allocator = StringVector::GetStringAllocator(result);
	for (idx_t i = 0; i < sel_count; i++) {
		auto string_number = codes.get_index(i);
		if (string_number == 0) {
			result_data.WriteNull();
			continue;
		}
		result_data.WriteStringRef(FetchEntry(allocator, string_number));
	}
	result.Verify();
}

bool DictFSSTSparse::TryFilter(CompressedStringScanState &scan_state, idx_t start, idx_t vector_count, Vector &result,
                               SelectionVector &sel, idx_t &sel_count, TableFilterState &filter_state) {
	if (!UseSparse(scan_state, vector_count, sel_count)) {
		return false;
	}
	if (!scan_state.sparse_verdicts) {
		scan_state.sparse_verdicts = make_unsafe_uniq_array<uint8_t>(scan_state.dict_count);
		memset(scan_state.sparse_verdicts.get(), UNKNOWN, scan_state.dict_count);
		scan_state.sparse_slots = make_unsafe_uniq_array<uint32_t>(scan_state.dict_count);
	}
	auto verdicts = scan_state.sparse_verdicts.get();
	auto slots = scan_state.sparse_slots.get();
	auto &codes = scan_state.GetSelVec(start, vector_count);
	PrepareFlatResult(result, vector_count);
	auto writer = FlatVector::Writer<string_t>(result, vector_count);
	auto &allocator = StringVector::GetStringAllocator(result);

	uint32_t pending[STANDARD_VECTOR_SIZE];
	idx_t pending_count = 0;
	for (idx_t i = 0; i < sel_count; i++) {
		auto code = codes.get_index(sel.get_index(i));
		if (verdicts[code] == UNKNOWN) {
			verdicts[code] = PENDING;
			slots[code] = UnsafeNumericCast<uint32_t>(pending_count);
			pending[pending_count++] = UnsafeNumericCast<uint32_t>(code);
		}
	}
	Vector batch(result.GetType(), MaxValue<idx_t>(pending_count, 1));
	if (pending_count > 0) {
		{
			auto writer = FlatVector::Writer<string_t>(batch, pending_count);
			for (idx_t i = 0; i < pending_count; i++) {
				if (pending[i] == 0) {
					writer.WriteNull();
				} else {
					writer.WriteStringRef(scan_state.FetchEntry(allocator, pending[i]));
				}
			}
		}
		for (idx_t i = 0; i < pending_count; i++) {
			verdicts[pending[i]] = FAIL;
		}
		SelectionVector batch_sel;
		idx_t match_count = pending_count;
		ColumnSegment::FilterSelection(batch_sel, batch, filter_state, pending_count, match_count);
		for (idx_t i = 0; i < match_count; i++) {
			verdicts[pending[batch_sel.get_index(i)]] = PASS;
		}
		scan_state.decoded_on_demand += pending_count;
	}

	SelectionVector matching_sel(sel_count);
	idx_t approved = 0;
	idx_t next = 0;
	for (idx_t row = 0; row < vector_count; row++) {
		if (next < sel_count && sel.get_index(next) == row) {
			next++;
			auto code = codes.get_index(row);
			if (verdicts[code] == PASS) {
				matching_sel.set_index(approved++, row);
				if (code == 0) {
					writer.WriteNull();
				} else if (slots[code] < pending_count && pending[slots[code]] == code) {
					writer.WriteStringRef(FlatVector::GetData<string_t>(batch)[slots[code]]);
				} else {
					writer.WriteStringRef(scan_state.FetchEntry(allocator, code));
				}
				continue;
			}
		}
		writer.WriteStringRef(string_t());
	}
	sel.Initialize(matching_sel);
	sel_count = approved;
	scan_state.decoded_on_demand += approved;
	return true;
}

} // namespace dict_fsst
} // namespace duckdb
