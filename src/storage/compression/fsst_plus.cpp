#include "duckdb/storage/compression/fsst_plus/common.hpp"
#include "duckdb/storage/compression/fsst_plus/analyze.hpp"
#include "duckdb/storage/compression/fsst_plus/compression.hpp"
#include "duckdb/storage/compression/fsst_plus/decompression.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"

/*
FSST+ segment layout (all three modes share it; the header mode field only
records how the dictionary was built at compress time):

+--------------------------------------------+
|  fsst_plus_compression_header_t            |
+--------------------------------------------+
|  Selection buffer (bitpacked)              |  row -> entry index (0 == NULL)
+--------------------------------------------+
|  FSST symbol table                         |
+--------------------------------------------+
|  Prefix lengths (bitpacked)                |  per distinct shared prefix
+--------------------------------------------+
|  Prefix bytes                              |  concatenated FSST-encoded prefixes
+--------------------------------------------+
|  Entry prefix ids (bitpacked)              |  entry -> prefix idx (== count: none)
+--------------------------------------------+
|  Entry suffix lengths (bitpacked)          |
+--------------------------------------------+
|  Suffix bytes                              |  concatenated FSST-encoded suffixes
+--------------------------------------------+
*/

namespace duckdb {
namespace fsst_plus {

struct FSSTPlusCompressionStorage {
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
unique_ptr<AnalyzeState> FSSTPlusCompressionStorage::StringInitAnalyze(CompressionAnalyzeContext &ctx,
                                                                       PhysicalType type) {
	if (StorageManager::IsPriorToVersion(StorageVersion::V1_3_0, ctx.storage_version)) {
		return nullptr;
	}
	return make_uniq<FSSTPlusAnalyzeState>(ctx.block_manager);
}

bool FSSTPlusCompressionStorage::StringAnalyze(AnalyzeState &state_p, const Vector &input) {
	return state_p.Cast<FSSTPlusAnalyzeState>().Analyze(input);
}

idx_t FSSTPlusCompressionStorage::StringFinalAnalyze(AnalyzeState &state_p) {
	return state_p.Cast<FSSTPlusAnalyzeState>().FinalAnalyze();
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
unique_ptr<CompressionState> FSSTPlusCompressionStorage::InitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                                         unique_ptr<AnalyzeState> state) {
	return make_uniq<FSSTPlusCompressionState>(checkpoint_data,
	                                           unique_ptr_cast<AnalyzeState, FSSTPlusAnalyzeState>(std::move(state)));
}

void FSSTPlusCompressionStorage::Compress(CompressionState &state_p, const Vector &scan_vector) {
	state_p.Cast<FSSTPlusCompressionState>().Compress(scan_vector);
}

void FSSTPlusCompressionStorage::FinalizeCompress(CompressionState &state_p) {
	state_p.Cast<FSSTPlusCompressionState>().FinalizeCompress();
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
unique_ptr<SegmentScanState> FSSTPlusCompressionStorage::StringInitScan(const QueryContext &context,
                                                                        ColumnSegment &segment) {
	auto &buffer_manager = BufferManager::GetBufferManager(segment.GetDatabase());
	auto state = make_uniq<CompressedStringScanState>(segment, buffer_manager.Pin(segment.GetBlockHandle()));
	state->Initialize(true);
	const auto &stats = segment.GetStats();
	if (stats.GetStatsType() == StatisticsType::STRING_STATS && StringStats::HasMaxStringLength(stats)) {
		state->all_values_inlined = StringStats::MaxStringLength(stats) <= string_t::INLINE_LENGTH;
	}
	return std::move(state);
}

template <bool ALLOW_DICT_VECTORS>
void FSSTPlusCompressionStorage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                                   Vector &result, idx_t result_offset) {
	auto &scan_state = state.scan_state->Cast<CompressedStringScanState>();
	auto start = state.GetPositionInSegment();
	if (!ALLOW_DICT_VECTORS || !scan_state.AllowDictionaryScan(scan_count)) {
		scan_state.ScanToFlatVector(result, result_offset, start, scan_count);
	} else {
		scan_state.ScanToDictionaryVector(segment, result, result_offset, start, scan_count);
	}
}

void FSSTPlusCompressionStorage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count,
                                            Vector &result) {
	StringScanPartial<true>(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void FSSTPlusCompressionStorage::StringFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id,
                                                Vector &result, idx_t result_idx) {
	CompressedStringScanState scan_state(segment, state.GetOrInsertHandle(segment));
	scan_state.Initialize(false);
	scan_state.ScanToFlatVector(result, result_idx, NumericCast<idx_t>(row_id), 1);
}

//===--------------------------------------------------------------------===//
// Select / Filter
//===--------------------------------------------------------------------===//
void FSSTPlusSelect(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                    const SelectionVector &sel, idx_t sel_count) {
	FSSTPlusCompressionStorage::StringScan(segment, state, vector_count, result);
	result.Slice(sel, sel_count);
}

static void FSSTPlusFilter(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
                           SelectionVector &sel, idx_t &sel_count, const TableFilter &filter,
                           TableFilterState &filter_state) {
	auto &scan_state = state.scan_state->Cast<CompressedStringScanState>();
	auto start = state.GetPositionInSegment();
	if (scan_state.AllowDictionaryScan(vector_count)) {
		if (!scan_state.filter_result) {
			scan_state.filter_result = make_unsafe_uniq_array<bool>(scan_state.dict_count);
			auto &dict_data = scan_state.dictionary->data;
			SelectionVector dict_sel;
			idx_t filter_count = scan_state.dict_count;
			ColumnSegment::FilterSelection(dict_sel, dict_data, filter_state, scan_state.dict_count, filter_count);
			for (idx_t i = 0; i < filter_count; i++) {
				scan_state.filter_result[dict_sel.get_index(i)] = true;
			}
			scan_state.filter_match_count = filter_count;
		}
		if (scan_state.filter_match_count == 0) {
			sel_count = 0;
			return;
		}
		auto &dict_sel = scan_state.GetSelVec(start, vector_count);
		if (scan_state.filter_match_count == scan_state.dict_count) {
			result.Dictionary(scan_state.dictionary, dict_sel, vector_count);
			return;
		}
		idx_t idx = 0;
		for (; idx < sel_count; idx++) {
			if (!scan_state.filter_result[dict_sel.get_index(sel.get_index(idx))]) {
				break;
			}
		}
		if (idx < sel_count) {
			SelectionVector matching_sel(sel_count);
			auto out_sel = matching_sel.data();
			idx_t approved = idx;
			for (idx_t i = 0; i < idx; i++) {
				out_sel[i] = UnsafeNumericCast<sel_t>(sel.get_index(i));
			}
			for (idx++; idx < sel_count; idx++) {
				auto row_idx = sel.get_index(idx);
				if (scan_state.filter_result[dict_sel.get_index(row_idx)]) {
					out_sel[approved++] = UnsafeNumericCast<sel_t>(row_idx);
				}
			}
			sel.Initialize(matching_sel);
			sel_count = approved;
		}
		result.Dictionary(scan_state.dictionary, dict_sel, vector_count);
		return;
	}
	FSSTPlusCompressionStorage::StringScan(segment, state, vector_count, result);
	ColumnSegment::FilterSelection(sel, result, filter_state, vector_count, sel_count);
}

} // namespace fsst_plus

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction FSSTPlusCompressionFun::GetFunction(PhysicalType data_type) {
	auto res = CompressionFunction(
	    CompressionType::COMPRESSION_FSST_PLUS, data_type, fsst_plus::FSSTPlusCompressionStorage::StringInitAnalyze,
	    fsst_plus::FSSTPlusCompressionStorage::StringAnalyze, fsst_plus::FSSTPlusCompressionStorage::StringFinalAnalyze,
	    fsst_plus::FSSTPlusCompressionStorage::InitCompression, fsst_plus::FSSTPlusCompressionStorage::Compress,
	    fsst_plus::FSSTPlusCompressionStorage::FinalizeCompress, fsst_plus::FSSTPlusCompressionStorage::StringInitScan,
	    fsst_plus::FSSTPlusCompressionStorage::StringScan,
	    fsst_plus::FSSTPlusCompressionStorage::StringScanPartial<false>,
	    fsst_plus::FSSTPlusCompressionStorage::StringFetchRow, UncompressedFunctions::EmptySkip,
	    UncompressedStringStorage::StringInitSegment);
	res.validity = CompressionValidity::NO_VALIDITY_REQUIRED;
	res.select = fsst_plus::FSSTPlusSelect;
	res.filter = fsst_plus::FSSTPlusFilter;
	return res;
}

bool FSSTPlusCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	return physical_type == PhysicalType::VARCHAR;
}

} // namespace duckdb
