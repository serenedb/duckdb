#include "duckdb/storage/compression/fsst_plus/compression.hpp"
#include "duckdb/storage/compression/fsst_plus/builder.hpp"
#include "fsst.h"

#include <algorithm>

namespace duckdb {
namespace fsst_plus {

FSSTPlusCompressionState::FSSTPlusCompressionState(ColumnDataCheckpointData &checkpoint_data_p,
                                                   unique_ptr<FSSTPlusAnalyzeState> &&analyze_p)
    : StandardCompressionState(checkpoint_data_p, CompressionType::COMPRESSION_FSST_PLUS), stats_writer(GetType()),
      analyze(std::move(analyze_p)) {
	auto raw_mode = GetCompressionOptions().fsst_mode;
	if (raw_mode < static_cast<uint8_t>(FSSTPlusMode::COUNT)) {
		mode = static_cast<FSSTPlusMode>(raw_mode);
	}
	CreateEmptySegment();
}

FSSTPlusCompressionState::~FSSTPlusCompressionState() {
}

void FSSTPlusCompressionState::CreateEmptySegment() {
	CreateAndPinNewSegment();
	entry_heap.Destroy();
	dedup.clear();
	entries.clear();
	row_entry.clear();
	running_raw_bytes = 0;
	tuple_count = 0;
}

idx_t FSSTPlusCompressionState::EstimatedSize(idx_t extra_raw, idx_t extra_entries) const {
	idx_t dict_count = entries.size() + extra_entries + 1;
	idx_t rows = tuple_count + 1;
	auto indices_width = BitpackingPrimitives::MinimumBitWidth(dict_count);
	idx_t size = 0;
	size += AlignValue<idx_t>(FSSTPlusCompression::HEADER_SIZE);
	size += AlignValue<idx_t>(BitpackingPrimitives::GetRequiredSize(rows, indices_width));
	size += AlignValue<idx_t>(FSST_MAXHEADER); // pessimistic symbol table
	// pessimistic: encoded prefix+suffix bytes <= 2x raw, plus ~8B metadata/entry
	size += AlignValue<idx_t>(2 * (running_raw_bytes + extra_raw));
	size += AlignValue<idx_t>(dict_count * 8);
	return size;
}

void FSSTPlusCompressionState::Compress(const Vector &scan_vector) {
	UnifiedVectorFormat vdata;
	scan_vector.ToUnifiedFormat(vdata);
	auto strings = UnifiedVectorFormat::GetData<string_t>(vdata);
	const auto count = scan_vector.size();
	const bool dedup_mode = mode != FSSTPlusMode::FSST_PLUS && mode != FSSTPlusMode::FSST;
	const idx_t block_size = info.GetBlockSize();

	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		bool is_null = !vdata.validity.RowIsValid(idx);
		if (is_null) {
			row_entry.push_back(0);
			stats_writer.SetHasNull();
			tuple_count++;
			continue;
		}
		auto &str = strings[idx];
		auto str_len = str.GetSize();

		uint32_t entry_ref; // 1 + entry index
		if (dedup_mode) {
			string key(str.GetData(), str_len);
			auto it = dedup.find(key);
			if (it != dedup.end()) {
				entry_ref = it->second + 1;
				row_entry.push_back(entry_ref);
				stats_writer.Update(str);
				tuple_count++;
				continue;
			}
			// new unique entry: flush first if it would not fit
			if (!entries.empty() && EstimatedSize(str_len, 1) > block_size) {
				Flush(false);
			}
			auto stored = entry_heap.AddBlob(str);
			auto entry_idx = NumericCast<uint32_t>(entries.size());
			entries.push_back(stored);
			dedup.emplace(string(stored.GetData(), stored.GetSize()), entry_idx);
			running_raw_bytes += str_len;
			entry_ref = entry_idx + 1;
		} else {
			if (!entries.empty() && EstimatedSize(str_len, 1) > block_size) {
				Flush(false);
			}
			auto stored = entry_heap.AddBlob(str);
			auto entry_idx = NumericCast<uint32_t>(entries.size());
			entries.push_back(stored);
			running_raw_bytes += str_len;
			entry_ref = entry_idx + 1;
		}
		row_entry.push_back(entry_ref);
		stats_writer.Update(str);
		tuple_count++;
	}
}

idx_t FSSTPlusCompressionState::Finalize() {
	idx_t entry_n = entries.size();
	idx_t dict_count = entry_n + 1;

	// establish the on-disk entry order (SORTED sorts lexicographically by the
	// original bytes for better prefix sharing; other modes keep insertion order)
	vector<uint32_t> ordered_to_old(entry_n);
	for (idx_t i = 0; i < entry_n; i++) {
		ordered_to_old[i] = NumericCast<uint32_t>(i);
	}
	if (mode == FSSTPlusMode::SORTED_DICT_FSST_PLUS) {
		std::sort(ordered_to_old.begin(), ordered_to_old.end(), [&](uint32_t a, uint32_t b) {
			return entries[a].GetString() < entries[b].GetString();
		});
	}
	vector<string_t> ordered(entry_n);
	for (idx_t i = 0; i < entry_n; i++) {
		ordered[i] = entries[ordered_to_old[i]];
	}

	const bool enable_prefix = mode == FSSTPlusMode::FSST_PLUS || mode == FSSTPlusMode::DICT_FSST_PLUS ||
	                           mode == FSSTPlusMode::SORTED_DICT_FSST_PLUS;
	CleavedDictionary dict;
	if (!BuildCleavedDictionary(dict, ordered, enable_prefix)) {
		throw FatalException("FSST+ compression could not encode the dictionary (string too long for FSST+); "
		                     "select dict_fsst for this column");
	}

	auto indices_width = BitpackingPrimitives::MinimumBitWidth(NumericCast<uint32_t>(entry_n));
	auto prefix_lengths_width = dict.PrefixLengthsWidth();
	auto prefix_id_width = dict.PrefixIdWidth();
	auto suffix_lengths_width = dict.SuffixLengthsWidth();
	idx_t prefix_count = dict.prefixes.size();

	// old entry index -> new (cleave-order) dictionary index (1-based)
	vector<uint32_t> old_to_new(entry_n, 0);
	for (idx_t j = 0; j < dict.entries.size(); j++) {
		uint32_t old = ordered_to_old[dict.entries[j].original_index];
		old_to_new[old] = NumericCast<uint32_t>(j + 1);
	}
	vector<uint32_t> sel_new(tuple_count, 0);
	for (idx_t r = 0; r < tuple_count; r++) {
		uint32_t ref = row_entry[r];
		sel_new[r] = ref == 0 ? 0 : old_to_new[ref - 1];
	}

	auto layout = FSSTPlusLayout::Compute(tuple_count, dict_count, prefix_count, indices_width, prefix_lengths_width,
	                                      prefix_id_width, suffix_lengths_width, dict.symbol_table_size,
	                                      dict.prefix_bytes, dict.suffix_bytes);
	if (layout.total > info.GetBlockSize()) {
		throw FatalException("FSST+ segment size %llu exceeds block size %llu after cleaving", (unsigned long long)layout.total,
		                     (unsigned long long)info.GetBlockSize());
	}

	auto base_ptr = handle.GetDataMutable();
	auto header_ptr = reinterpret_cast<fsst_plus_compression_header_t *>(base_ptr);
	header_ptr->mode = mode;
	header_ptr->dictionary_indices_width = indices_width;
	header_ptr->prefix_id_width = prefix_id_width;
	header_ptr->suffix_lengths_width = suffix_lengths_width;
	header_ptr->prefix_lengths_width = prefix_lengths_width;
	header_ptr->padding0 = 0;
	header_ptr->padding1 = 0;
	header_ptr->dict_count = NumericCast<uint32_t>(dict_count);
	header_ptr->prefix_count = NumericCast<uint32_t>(prefix_count);
	header_ptr->symbol_table_size = NumericCast<uint32_t>(dict.symbol_table_size);
	header_ptr->prefix_bytes_size = NumericCast<uint32_t>(dict.prefix_bytes);
	header_ptr->suffix_bytes_size = NumericCast<uint32_t>(dict.suffix_bytes);

	// selection buffer
	if (indices_width > 0 && tuple_count > 0) {
		BitpackingPrimitives::PackBuffer<sel_t, false>(base_ptr + layout.selection_dest, (sel_t *)sel_new.data(),
		                                               tuple_count, indices_width);
	}
	// symbol table (re-export straight into the segment)
	if (dict.symbol_table_size > 0) {
		duckdb_fsst_export(reinterpret_cast<duckdb_fsst_encoder_t *>(dict.encoder), base_ptr + layout.symtab_dest);
	}
	// prefix lengths + prefix bytes
	if (prefix_count > 0) {
		vector<uint32_t> pl(prefix_count);
		for (idx_t i = 0; i < prefix_count; i++) {
			pl[i] = dict.prefixes[i].len;
		}
		if (prefix_lengths_width > 0) {
			BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + layout.prefix_lengths_dest, pl.data(),
			                                                  prefix_count, prefix_lengths_width);
		}
		idx_t off = 0;
		for (idx_t i = 0; i < prefix_count; i++) {
			memcpy(base_ptr + layout.prefix_bytes_dest + off, dict.prefixes[i].data, dict.prefixes[i].len);
			off += dict.prefixes[i].len;
		}
	}
	// per-entry prefix ids + suffix lengths + suffix bytes
	if (entry_n > 0) {
		vector<uint32_t> pid(entry_n);
		vector<uint32_t> sl(entry_n);
		for (idx_t j = 0; j < entry_n; j++) {
			pid[j] = dict.entries[j].prefix_id;
			sl[j] = dict.entries[j].suffix_len;
		}
		if (prefix_id_width > 0) {
			BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + layout.prefix_ids_dest, pid.data(), entry_n,
			                                                  prefix_id_width);
		}
		if (suffix_lengths_width > 0) {
			BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + layout.suffix_lengths_dest, sl.data(), entry_n,
			                                                  suffix_lengths_width);
		}
		idx_t off = 0;
		for (idx_t j = 0; j < entry_n; j++) {
			memcpy(base_ptr + layout.suffix_bytes_dest + off, dict.entries[j].suffix, dict.entries[j].suffix_len);
			off += dict.entries[j].suffix_len;
		}
	}
	return layout.total;
}

void FSSTPlusCompressionState::Flush(bool final) {
	if (tuple_count == 0) {
		if (final) {
			return;
		}
		CreateEmptySegment();
		return;
	}
	current_segment->count = tuple_count;
	auto segment_size = Finalize();
	FlushCurrentSegment(stats_writer, segment_size);
	total_tuple_count += tuple_count;
	if (!final) {
		CreateEmptySegment();
	}
}

void FSSTPlusCompressionState::FinalizeCompress() {
	Flush(true);
}

} // namespace fsst_plus
} // namespace duckdb
