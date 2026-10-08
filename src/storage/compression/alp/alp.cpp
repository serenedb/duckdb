#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/planner/filter/zonemap_checker.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/compression/alp/alp_analyze.hpp"
#include "duckdb/storage/compression/alp/alp_compress.hpp"
#include "duckdb/storage/compression/alp/alp_fetch.hpp"
#include "duckdb/storage/compression/alp/alp_scan.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/table/column_segment.hpp"

namespace duckdb {

void ThrowAlpMetadataBeforeHeader() {
	throw DataCorruptionException("Corrupted ALP segment: metadata ends before the segment header");
}

void ThrowAlpMetadataTableOutOfBounds() {
	throw DataCorruptionException("Corrupted ALP segment: metadata offset table exceeds the segment");
}

void ThrowAlpVectorOffsetOutOfBounds() {
	throw DataCorruptionException("Corrupted ALP segment: vector offset is outside the data region");
}

void ThrowAlpVectorOffsetsInvalid() {
	throw DataCorruptionException("Corrupted ALP segment: vector offsets do not describe a data range");
}

void ThrowAlpExponentOutOfRange(AlpConstants::EXPONENT_TYPE exponent, AlpConstants::EXPONENT_TYPE max_exponent) {
	throw DataCorruptionException("Corrupted ALP segment: exponent %d exceeds the maximum %d", exponent, max_exponent);
}

void ThrowAlpFactorOutOfRange(AlpConstants::FACTOR_TYPE factor, AlpConstants::EXPONENT_TYPE exponent) {
	throw DataCorruptionException("Corrupted ALP segment: factor %d exceeds exponent %d", factor, exponent);
}

void ThrowAlpExceptionCountOutOfRange(AlpConstants::EXCEPTIONS_COUNT_TYPE exception_count, idx_t vector_size) {
	throw DataCorruptionException("Corrupted ALP segment: exception count %d exceeds vector size %d", exception_count,
	                              vector_size);
}

void ThrowAlpBitWidthOutOfRange(AlpConstants::BIT_WIDTH_TYPE bit_width) {
	throw DataCorruptionException("Corrupted ALP segment: bit width %d exceeds %d", bit_width,
	                              AlpConstants::MAX_BIT_WIDTH);
}

void ThrowAlpExceptionPositionOutOfRange(AlpConstants::EXCEPTION_POSITION_TYPE position, idx_t vector_size) {
	throw DataCorruptionException("Corrupted ALP segment: exception position %d is outside vector size %d", position,
	                              vector_size);
}

//===--------------------------------------------------------------------===//
// Filter
//===--------------------------------------------------------------------===//
//! Conservative [min, max] of one ALP vector, read from its header: digits = unpacked +
//! frame_of_reference (wrapping, like the scan), and the decode multiplies by a positive constant,
//! so the digit range's endpoints bound every decoded value; exceptions are raw values patched
//! over the decode, unioned in under duckdb float ordering (NaN greatest), so a NaN exception only
//! widens the upper bound. Returns false for uncompressed-mode vectors and digit ranges that cross
//! the signed wrap.
template <class T>
static bool AlpVectorBounds(const AlpScanState<T> &scan_state, idx_t vector_idx, idx_t vector_rows, T &minimum,
                            T &maximum) {
	auto vector_reader = scan_state.GetVectorReader(vector_idx);
	const auto v_exponent = vector_reader.template Read<AlpConstants::EXPONENT_TYPE>();
	if (v_exponent == AlpConstants::UNCOMPRESSED_MODE_SENTINEL) {
		// raw values, no header to bound them by
		return false;
	}
	const auto v_factor = vector_reader.template Read<AlpConstants::FACTOR_TYPE>();
	const auto exceptions_count = vector_reader.template Read<AlpConstants::EXCEPTIONS_COUNT_TYPE>();
	const auto frame_of_reference = vector_reader.template Read<AlpConstants::FRAME_OF_REFERENCE_TYPE>();
	const auto bit_width = vector_reader.template Read<AlpConstants::BIT_WIDTH_TYPE>();
	// an invalid header must degrade to decode-and-evaluate, where LoadVector reports it
	if (v_exponent > AlpTypedConstants<T>::MAX_EXPONENT || v_factor > v_exponent ||
	    bit_width > AlpConstants::MAX_BIT_WIDTH || bit_width >= sizeof(uint64_t) * 8 ||
	    exceptions_count > vector_rows) {
		return false;
	}

	const uint64_t packed_span = bit_width == 0 ? 0 : (uint64_t(1) << bit_width) - 1;
	const auto lo_digit = static_cast<int64_t>(frame_of_reference);
	const auto hi_digit = static_cast<int64_t>(frame_of_reference + packed_span);
	if (lo_digit > hi_digit) {
		return false;
	}
	alp::AlpEncodingIndices encoding_indices = {v_exponent, v_factor};
	minimum = alp::AlpCompression<T, true>::DecodeValue(lo_digit, encoding_indices);
	maximum = alp::AlpCompression<T, true>::DecodeValue(hi_digit, encoding_indices);

	if (exceptions_count > 0) {
		if (bit_width > 0) {
			vector_reader.Skip(BitpackingPrimitives::GetRequiredSize(vector_rows, bit_width));
		}
		auto exceptions = vector_reader.ReadBytes(exceptions_count * sizeof(T));
		for (idx_t i = 0; i < exceptions_count; i++) {
			T exception = Load<T>(exceptions.data() + i * sizeof(T));
			if (LessThan::Operation(exception, minimum)) {
				minimum = exception;
			}
			if (GreaterThan::Operation(exception, maximum)) {
				maximum = exception;
			}
		}
	}
	return true;
}

template <class T>
void AlpFilter(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result, SelectionVector &sel,
               idx_t &sel_count, const TableFilter &filter, TableFilterState &filter_state) {
	auto &scan_state = state.scan_state->Cast<AlpScanState<T>>();
	auto &checker = *filter_state.Cast<ExpressionFilterState>().zonemap_checker;
	auto verdict = FilterPropagateResult::NO_PRUNING_POSSIBLE;
	// Combine the verdicts of the ALP vectors the window touches: the vector bounds decide
	// whether the whole window skips the decode (ALWAYS_FALSE) or the predicate (ALWAYS_TRUE);
	// anything else decodes and evaluates as usual. Probes run per window, so only a fully
	// compiled check stays cheap enough.
	if (checker.IsFullyCompiled()) {
		if (!scan_state.filter_group_stats) {
			scan_state.filter_group_stats = NumericStats::CreateEmpty(segment.GetType()).ToUnique();
			scan_state.filter_group_stats->SetHasNoNullFast();
		}
		auto &group_stats = *scan_state.filter_group_stats;
		const idx_t total_rows = scan_state.count;
		idx_t vector_idx = scan_state.total_value_count / AlpConstants::ALP_VECTOR_SIZE;
		idx_t offset_in_vector = scan_state.total_value_count % AlpConstants::ALP_VECTOR_SIZE;
		idx_t remaining = vector_count;
		bool first = true;
		while (remaining > 0) {
			const idx_t vector_rows =
			    MinValue<idx_t>(AlpConstants::ALP_VECTOR_SIZE, total_rows - vector_idx * AlpConstants::ALP_VECTOR_SIZE);
			auto group_verdict = FilterPropagateResult::NO_PRUNING_POSSIBLE;
			T minimum;
			T maximum;
			if (AlpVectorBounds<T>(scan_state, vector_idx, vector_rows, minimum, maximum)) {
				NumericStats::SetMin<T>(group_stats, minimum);
				NumericStats::SetMax<T>(group_stats, maximum);
				group_verdict = checker.Check(group_stats, nullptr);
			}
			if (first) {
				verdict = group_verdict;
				first = false;
			} else if (group_verdict != verdict) {
				verdict = FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			if (verdict == FilterPropagateResult::NO_PRUNING_POSSIBLE) {
				break;
			}
			remaining -= MinValue<idx_t>(remaining, vector_rows - offset_in_vector);
			vector_idx++;
			offset_in_vector = 0;
		}
	}
	if (verdict == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
	    verdict == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
		// nothing in the window can pass: advance the cursor without decoding (whole skipped
		// vectors only step the metadata pointer)
		scan_state.Skip(segment, vector_count);
		sel_count = 0;
		return;
	}
	AlpScanPartial<T>(segment, state, vector_count, result, 0);
	FlatVector::SetSize(result, count_t(vector_count));
	if (verdict == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
		return;
	}
	ColumnSegment::FilterSelection(sel, result, filter_state, vector_count, sel_count);
}

template <class T>
CompressionFunction GetAlpFunction(PhysicalType data_type) {
	throw NotImplementedException("GetAlpFunction not implemented for the given datatype");
}

template <>
CompressionFunction GetAlpFunction<float>(PhysicalType data_type) {
	CompressionFunction alpfun(CompressionType::COMPRESSION_ALP, data_type, AlpInitAnalyze<float>, AlpAnalyze<float>,
	                           AlpFinalAnalyze<float>, AlpInitCompression<float>, AlpCompress<float>,
	                           AlpFinalizeCompress<float>, AlpInitScan<float>, AlpScan<float>, AlpScanPartial<float>,
	                           AlpFetchRow<float>, AlpSkip<float>);
	alpfun.filter = AlpFilter<float>;
	return alpfun;
}

template <>
CompressionFunction GetAlpFunction<double>(PhysicalType data_type) {
	CompressionFunction alpfun(CompressionType::COMPRESSION_ALP, data_type, AlpInitAnalyze<double>, AlpAnalyze<double>,
	                           AlpFinalAnalyze<double>, AlpInitCompression<double>, AlpCompress<double>,
	                           AlpFinalizeCompress<double>, AlpInitScan<double>, AlpScan<double>,
	                           AlpScanPartial<double>, AlpFetchRow<double>, AlpSkip<double>);
	alpfun.filter = AlpFilter<double>;
	return alpfun;
}

CompressionFunction AlpCompressionFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::FLOAT:
		return GetAlpFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetAlpFunction<double>(type);
	default:
		throw InternalException("Unsupported type for Alp");
	}
}

bool AlpCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
