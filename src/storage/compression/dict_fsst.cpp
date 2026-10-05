#include "duckdb/common/enum_util.hpp"
#include "duckdb/storage/compression/dict_fsst/common.hpp"
#include "duckdb/storage/compression/dict_fsst/analyze.hpp"
#include "duckdb/storage/compression/dict_fsst/compression.hpp"
#include "duckdb/storage/compression/dict_fsst/decompression.hpp"
#include "duckdb/storage/compression/dict_fsst/sparse.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"

/*
Data layout per segment:
+-----------------------------------------------------+
|                  Header                             |
|   +---------------------------------------------+   |
|   |   dict_fsst_compression_header_t  header    |   |
|   +---------------------------------------------+   |
|                                                     |
+-----------------------------------------------------+
|             Selection Buffer               |
|   +------------------------------------+   |
|   |   uint16_t index_buffer_idx[]      |   |
|   +------------------------------------+   |
|      tuple index -> index buffer idx       |
|                                            |
+--------------------------------------------+
|               Index Buffer                 |
|   +------------------------------------+   |
|   |   uint16_t  dictionary_offset[]    |   |
|   +------------------------------------+   |
|  string_index -> offset in the dictionary  |
|                                            |
+--------------------------------------------+
|                Dictionary                  |
|   +------------------------------------+   |
|   |   uint8_t *raw_string_data         |   |
|   +------------------------------------+   |
|      the string data without lengths       |
|                                            |
+--------------------------------------------+
|             FSST Symbol Table (opt)        |
|   +------------------------------------+   |
|   |   duckdb_fsst_decoder_t table      |   |
|   +------------------------------------+   |
|                                            |
+--------------------------------------------+
*/

namespace duckdb {
namespace dict_fsst {

struct DictFSSTCompressionStorage {
	static unique_ptr<AnalyzeState> StringInitAnalyze(CompressionAnalyzeContext &ctx, PhysicalType type);
	static bool StringAnalyze(AnalyzeState &state_p, const Vector &input);
	static idx_t StringFinalAnalyze(AnalyzeState &state_p);

	static unique_ptr<CompressionState> InitCompression(ColumnDataCheckpointData &checkpoint_data,
	                                                    unique_ptr<AnalyzeState> state);
	static void Compress(CompressionState &state_p, const Vector &scan_vector);
	static void FinalizeCompress(CompressionState &state_p);

	static unique_ptr<SegmentScanState> StringInitScan(const QueryContext &context, ColumnSegment &segment);
	template <bool ALLOW_DICT_VECTORS>
	static void StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                              idx_t result_offset);
	static void StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result);
	static void StringFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result,
	                           idx_t result_idx);
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
unique_ptr<AnalyzeState> DictFSSTCompressionStorage::StringInitAnalyze(CompressionAnalyzeContext &ctx,
                                                                       PhysicalType type) {
	if (StorageManager::IsPriorToVersion(StorageVersion::V1_3_0, ctx.storage_version)) {
		// dict_fsst not introduced yet, disable it
		return nullptr;
	}

	return make_uniq<DictFSSTAnalyzeState>(ctx.block_manager);
}

bool DictFSSTCompressionStorage::StringAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &analyze_state = state_p.Cast<DictFSSTAnalyzeState>();
	return analyze_state.Analyze(input);
}

idx_t DictFSSTCompressionStorage::StringFinalAnalyze(AnalyzeState &state_p) {
	auto &analyze_state = state_p.Cast<DictFSSTAnalyzeState>();
	return analyze_state.FinalAnalyze();
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
unique_ptr<CompressionState> DictFSSTCompressionStorage::InitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                                         unique_ptr<AnalyzeState> state) {
	return make_uniq<DictFSSTCompressionState>(checkpoint_data,
	                                           unique_ptr_cast<AnalyzeState, DictFSSTAnalyzeState>(std::move(state)));
}

void DictFSSTCompressionStorage::Compress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = state_p.Cast<DictFSSTCompressionState>();
	state.Compress(scan_vector);
}

void DictFSSTCompressionStorage::FinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<DictFSSTCompressionState>();
	state.Flush(true);
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
unique_ptr<SegmentScanState> DictFSSTCompressionStorage::StringInitScan(const QueryContext &context,
                                                                        ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto state = make_uniq<CompressedStringScanState>(segment, buffer_manager.Pin(segment.GetBlockHandle()));
	state->Initialize(false);
	state->deferred_dictionary = state->mode != DictFSSTMode::FSST_ONLY && state->mode != DictFSSTMode::FSST_PLUS;
	return std::move(state);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <bool ALLOW_DICT_VECTORS>
void DictFSSTCompressionStorage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                                   Vector &result, idx_t result_offset) {
	// clear any previously locked buffers and get the primary buffer handle
	auto &scan_state = state.scan_state->Cast<CompressedStringScanState>();
	scan_state.PrepareRead(scan_count, STANDARD_VECTOR_SIZE);

	auto start = state.GetPositionInSegment();
	if (!ALLOW_DICT_VECTORS || !scan_state.AllowDictionaryScan(scan_count)) {
		scan_state.ScanToFlatVector(result, result_offset, start, scan_count);
	} else {
		scan_state.ScanToDictionaryVector(segment, result, result_offset, start, scan_count);
	}
}

void DictFSSTCompressionStorage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                            Vector &result) {
	StringScanPartial<true>(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void DictFSSTCompressionStorage::StringFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id,
                                                Vector &result, idx_t result_idx) {
	// fetch a single row from the string segment
	CompressedStringScanState scan_state(segment, state.GetOrInsertHandle(segment));
	scan_state.Initialize(false);
	scan_state.ScanToFlatVector(result, result_idx, NumericCast<idx_t>(row_id), 1);
}

//===--------------------------------------------------------------------===//
// Select
//===--------------------------------------------------------------------===//
void DictFSSTSelect(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                    const SelectionVector &sel, idx_t sel_count) {
	auto &scan_state = state.scan_state->Cast<CompressedStringScanState>();
	if (scan_state.mode == DictFSSTMode::FSST_ONLY || scan_state.mode == DictFSSTMode::FSST_PLUS) {
		// for the no-selection-buffer per-row modes
		auto start = state.GetPositionInSegment();
		scan_state.Select(result, start, sel, sel_count);
		return;
	}
	scan_state.PrepareRead(sel_count, vector_count);
	if (scan_state.dictionary) {
		scan_state.SelectDictionary(result, state.GetPositionInSegment(), vector_count, sel, sel_count);
		return;
	}
	if (scan_state.deferred_dictionary) {
		scan_state.SelectEntries(result, state.GetPositionInSegment(), vector_count, sel, sel_count);
		return;
	}
	// fallback: scan + slice
	DictFSSTCompressionStorage::StringScan(segment, state, vector_count, result);
	result.Slice(sel, sel_count);
}

//===--------------------------------------------------------------------===//
// Filter
//===--------------------------------------------------------------------===//
static void DictFSSTResolveNullFilter(CompressedStringScanState &scan_state, TableFilterState &filter_state) {
	Vector null_data(scan_state.dictionary->data, /*offset=*/0, /*end=*/1);
	SelectionVector null_sel;
	idx_t null_filter_count = 1;
	ColumnSegment::FilterSelection(null_sel, null_data, filter_state, 1, null_filter_count);
	scan_state.filter_result[0] = null_filter_count == 1;
	scan_state.null_filter_result_initialized = true;
}

static void DictFSSTFilter(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                           SelectionVector &sel, idx_t &sel_count, const TableFilter &filter,
                           TableFilterState &filter_state) {
	auto &scan_state = state.scan_state->Cast<CompressedStringScanState>();
	auto start = state.GetPositionInSegment();
	if (DictFSSTSparse::TryFilter(scan_state, start, vector_count, result, sel, sel_count, filter_state)) {
		return;
	}
	if (scan_state.deferred_dictionary) {
		scan_state.MaterializeDictionary();
	}
	if (scan_state.dictionary && scan_state.mode != DictFSSTMode::FSST_ONLY &&
	    scan_state.mode != DictFSSTMode::FSST_PLUS) {
		// only pushdown filters on dictionaries
		if (!scan_state.filter_result) {
			// no filter result yet - apply filter to the dictionary
			// initialize the filter result - setting everything to false
			scan_state.filter_result = make_unsafe_uniq_array<bool>(scan_state.dict_count);

			// Slot zero represents NULL and is not necessarily referenced by any row.
			idx_t non_null_count = scan_state.dict_count - 1;
			Vector dict_data(scan_state.dictionary->data, /*offset=*/1, scan_state.dict_count);
			SelectionVector dict_sel;
			idx_t filter_count = non_null_count;
			ColumnSegment::FilterSelection(dict_sel, dict_data, filter_state, non_null_count, filter_count);

			// now set all matching tuples to true
			for (idx_t i = 0; i < filter_count; i++) {
				auto idx = dict_sel.get_index(i) + 1;
				scan_state.filter_result[idx] = true;
			}
			scan_state.filter_match_count = filter_count;
		}
		// Till now, we have a filter result for all non-NULL values.
		if (scan_state.null_filter_result_initialized) {
			const idx_t match_count = scan_state.filter_match_count + scan_state.filter_result[0];
			if (match_count == 0) {
				// early-out, no dictionary entry matches the filter so the filter can never pass
				sel_count = 0;
				return;
			}
			if (match_count == scan_state.dict_count) {
				// every dictionary entry matches (nulls live in the dictionary too, so a
				// null-rejecting filter never takes this path): all candidate rows pass as-is
				result.Dictionary(scan_state.dictionary, scan_state.GetSelVec(start, vector_count), vector_count);
				return;
			}
		}
		auto &dict_sel = scan_state.GetSelVec(start, vector_count);
		const sel_t *codes = dict_sel.data();
		const sel_t *rows = sel.data();
		const bool *passes = scan_state.filter_result.get();
		bool null_resolved = scan_state.null_filter_result_initialized;
		auto row_at = [rows](idx_t i) {
			return rows ? idx_t(rows[i]) : i;
		};
		auto row_passes = [&](idx_t row_idx) {
			const auto code = codes[row_idx];
			if (DUCKDB_UNLIKELY(code == 0 && !null_resolved)) {
				DictFSSTResolveNullFilter(scan_state, filter_state);
				null_resolved = true;
			}
			return passes[code];
		};
		// the selection is only rebuilt from the first entry that actually drops - a window
		// whose candidate rows all land on matching dictionary entries costs no copy
		idx_t idx = 0;
		for (; idx < sel_count; idx++) {
			if (!row_passes(row_at(idx))) {
				break;
			}
		}
		if (idx < sel_count) {
			// materialize the kept prefix, then rebuild from the first dropped entry
			SelectionVector matching_sel(sel_count);
			auto out_sel = matching_sel.data();
			idx_t approved_tuple_count = idx;
			for (idx_t i = 0; i < idx; i++) {
				out_sel[i] = UnsafeNumericCast<sel_t>(row_at(i));
			}
			for (idx++; idx < sel_count; idx++) {
				auto row_idx = row_at(idx);
				if (row_passes(row_idx)) {
					out_sel[approved_tuple_count++] = UnsafeNumericCast<sel_t>(row_idx);
				}
			}
			sel.Initialize(matching_sel);
			sel_count = approved_tuple_count;
		}
		result.Dictionary(scan_state.dictionary, dict_sel, vector_count);
		return;
	}
	// fallback: scan + filter
	DictFSSTCompressionStorage::StringScan(segment, state, vector_count, result);
	ColumnSegment::FilterSelection(sel, result, filter_state, vector_count, sel_count);
}

//===--------------------------------------------------------------------===//
// GetSegmentInfo
//===--------------------------------------------------------------------===//
static InsertionOrderPreservingMap<string> DictFSSTGetSegmentInfo(QueryContext, ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto state = make_uniq<CompressedStringScanState>(segment, buffer_manager.Pin(segment.GetBlockHandle()));
	state->Initialize(false);

	const auto tuple_count = segment.count.load();

	InsertionOrderPreservingMap<string> result;
	result[EnumUtil::ToChars(state->mode)] = StringUtil::Format("%d", tuple_count);
	return result;
}

} // namespace dict_fsst

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction DictFSSTCompressionFun::GetFunction(PhysicalType data_type) {
	auto res = CompressionFunction(
	    CompressionType::COMPRESSION_DICT_FSST, data_type, dict_fsst::DictFSSTCompressionStorage::StringInitAnalyze,
	    dict_fsst::DictFSSTCompressionStorage::StringAnalyze, dict_fsst::DictFSSTCompressionStorage::StringFinalAnalyze,
	    dict_fsst::DictFSSTCompressionStorage::InitCompression, dict_fsst::DictFSSTCompressionStorage::Compress,
	    dict_fsst::DictFSSTCompressionStorage::FinalizeCompress, dict_fsst::DictFSSTCompressionStorage::StringInitScan,
	    dict_fsst::DictFSSTCompressionStorage::StringScan,
	    dict_fsst::DictFSSTCompressionStorage::StringScanPartial<false>,
	    dict_fsst::DictFSSTCompressionStorage::StringFetchRow, UncompressedFunctions::EmptySkip,
	    UncompressedStringStorage::StringInitSegment);
	res.validity = CompressionValidity::NO_VALIDITY_REQUIRED;
	res.select = dict_fsst::DictFSSTSelect;
	res.filter = dict_fsst::DictFSSTFilter;
	res.get_segment_info = dict_fsst::DictFSSTGetSegmentInfo;
	return res;
}

bool DictFSSTCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	return physical_type == PhysicalType::VARCHAR;
}

} // namespace duckdb
