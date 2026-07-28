#include "duckdb/common/bitpacking.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include <functional>

namespace duckdb {

using rle_count_t = uint16_t;

static constexpr idx_t RLE_GROUP_SIZE = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;

//! Native layout: [uint64 counts_offset][T values[n]][pad][rle_count_t counts[n]] -- both
//! streams raw. SereneDB storage instead FOR+bitpacks them, which needs a wider header.
//! Both start with that same uint64, so the marker bit distinguishes them on read: it is
//! free natively because the field is a byte offset within one block.
static constexpr uint64_t RLE_PACKED_MARKER = 1ULL << 63;
static constexpr idx_t RLE_NATIVE_HEADER_SIZE = sizeof(uint64_t);

template <class T>
struct RLEHeader {
	uint64_t counts_offset;
	uint32_t entry_count;
	rle_count_t count_frame;
	bitpacking_width_t value_width;
	bitpacking_width_t count_width;
	T value_frame;
};

template <class T>
static constexpr idx_t RLEHeaderSize(bool packed) {
	return packed ? sizeof(RLEHeader<T>) : RLE_NATIVE_HEADER_SIZE;
}

template <class T>
static idx_t RLEMaxEntryCount(idx_t block_size, bool packed) {
	auto entry_size = sizeof(T) + sizeof(rle_count_t);
	auto staged = AlignValueFloor((block_size - RLEHeaderSize<T>(packed)) / entry_size);
	if (!packed) {
		return staged;
	}
	return staged - 2 * RLE_GROUP_SIZE;
}

template <class T, bool INTEGRAL = NumericLimits<T>::IsIntegral()>
struct RLEValueCodec;

template <class T>
struct RLEValueCodec<T, true> {
	using T_U = typename MakeUnsigned<T>::type;

	static bitpacking_width_t WidthForRange(T minimum, T maximum, T &frame) {
		T min_max_diff;
		if (!TrySubtractOperator::Operation(maximum, minimum, min_max_diff)) {
			frame = 0;
			return sizeof(T) * 8;
		}
		frame = minimum;
		return BitpackingPrimitives::MinimumBitWidth<T, false>(min_max_diff);
	}

	static bitpacking_width_t Analyze(const T *values, idx_t count, T &frame) {
		if (!count) {
			frame = 0;
			return 0;
		}
		T minimum = values[0];
		T maximum = values[0];
		for (idx_t i = 1; i < count; i++) {
			minimum = MinValue<T>(minimum, values[i]);
			maximum = MaxValue<T>(maximum, values[i]);
		}
		return WidthForRange(minimum, maximum, frame);
	}

	static idx_t PackedSize(idx_t count, bitpacking_width_t width) {
		return BitpackingPrimitives::GetRequiredSize(count, width);
	}

	static void Pack(data_ptr_t dst, T *values, idx_t count, T frame, bitpacking_width_t width) {
		if (frame) {
			for (idx_t i = 0; i < count; i++) {
				reinterpret_cast<T_U *>(values)[i] -= static_cast<T_U>(frame);
			}
		}
		BitpackingPrimitives::PackBuffer<T>(dst, values, count, width);
	}

	static void Unpack(T *dst, data_ptr_t src, idx_t count, T frame, bitpacking_width_t width) {
		BitpackingPrimitives::UnPackBuffer<T>(data_ptr_cast(dst), src, count, width, true);
		if (frame) {
			for (idx_t i = 0; i < count; i++) {
				reinterpret_cast<T_U *>(dst)[i] += static_cast<T_U>(frame);
			}
		}
	}
};

template <class T>
struct RLEValueCodec<T, false> {
	static bitpacking_width_t WidthForRange(T minimum, T maximum, T &frame) {
		frame = 0;
		return sizeof(T) * 8;
	}

	static bitpacking_width_t Analyze(const T *values, idx_t count, T &frame) {
		frame = 0;
		return sizeof(T) * 8;
	}

	static idx_t PackedSize(idx_t count, bitpacking_width_t width) {
		return BitpackingPrimitives::RoundUpToAlgorithmGroupSize(count) * sizeof(T);
	}

	static void Pack(data_ptr_t dst, T *values, idx_t count, T frame, bitpacking_width_t width) {
		memcpy(dst, values, count * sizeof(T));
		memset(dst + count * sizeof(T), 0, PackedSize(count, width) - count * sizeof(T));
	}

	static void Unpack(T *dst, data_ptr_t src, idx_t count, T frame, bitpacking_width_t width) {
		memcpy(dst, src, count * sizeof(T));
	}
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct EmptyRLEWriter {
	template <class VALUE_TYPE>
	static void Operation(VALUE_TYPE value, rle_count_t count, void *dataptr, bool is_null) {
	}
};

template <class T>
struct RLEState {
	RLEState() : seen_count(0), last_value(NullValue<T>()), last_seen_count(0), dataptr(nullptr) {
	}

	idx_t seen_count;
	T last_value;
	rle_count_t last_seen_count;
	void *dataptr;
	bool all_null = true;

public:
	template <class OP>
	void Flush() {
		OP::template Operation<T>(last_value, last_seen_count, dataptr, all_null);
	}

	template <class OP = EmptyRLEWriter>
	void UpdateFlatValid(const T *data, idx_t count) {
		static constexpr rle_count_t MAX_COUNT = NumericLimits<rle_count_t>::Maximum();
		if (!count) {
			return;
		}
		if (DUCKDB_UNLIKELY(all_null)) {
			last_value = data[0];
			seen_count++;
			all_null = false;
		}
		idx_t i = 0;
		while (i < count) {
			idx_t j = i;
			while (j < count && data[j] == last_value) {
				j++;
			}
			if (j == i) {
				if (last_seen_count > 0) {
					Flush<OP>();
					seen_count++;
				}
				//! Count this element as the first of its new run instead of re-scanning it: for a
				//! self-equal value the rescan would count it identically, and for NaN it never would
				//! (NaN != NaN), which left this loop spinning on the same element forever.
				last_value = data[i];
				last_seen_count = 1;
				i++;
				continue;
			}
			idx_t run = j - i;
			while (run > 0) {
				const idx_t take = MinValue<idx_t>(MAX_COUNT - last_seen_count, run);
				last_seen_count = UnsafeNumericCast<rle_count_t>(last_seen_count + take);
				run -= take;
				if (last_seen_count == MAX_COUNT) {
					Flush<OP>();
					last_seen_count = 0;
					seen_count++;
				}
			}
			i = j;
		}
	}

	template <class OP = EmptyRLEWriter>
	void Update(const T *data, ValidityMask &validity, idx_t idx) {
		if (validity.RowIsValid(idx)) {
			if (all_null) {
				// no value seen yet
				// assign the current value, and increment the seen_count
				// note that we increment last_seen_count rather than setting it to 1
				// this is intentional: this is the first VALID value we see
				// but it might not be the first value in case of nulls!
				last_value = data[idx];
				seen_count++;
				last_seen_count++;
				all_null = false;
			} else if (last_value == data[idx]) {
				// the last value is identical to this value: increment the last_seen_count
				last_seen_count++;
			} else {
				// the values are different
				// issue the callback on the last value
				// edge case: if a value has exactly 2^16 repeated values, we can end up here with last_seen_count = 0
				if (last_seen_count > 0) {
					Flush<OP>();
					seen_count++;
				}

				// increment the seen_count and put the new value into the RLE slot
				last_value = data[idx];
				last_seen_count = 1;
			}
		} else {
			// NULL value: we merely increment the last_seen_count
			last_seen_count++;
		}
		if (last_seen_count == NumericLimits<rle_count_t>::Maximum()) {
			// we have seen the same value so many times in a row we are at the limit of what fits in our count
			// write away the value and move to the next value
			Flush<OP>();
			last_seen_count = 0;
			seen_count++;
		}
	}
};

template <class T>
struct RLEAnalyzeState : public AnalyzeState {
	explicit RLEAnalyzeState(BlockManager &block_manager, bool packed_p)
	    : AnalyzeState(block_manager), packed(packed_p) {
		state.dataptr = this;
	}

	struct RLEAnalyzeWriter {
		template <class VALUE_TYPE>
		static void Operation(VALUE_TYPE value, rle_count_t count, void *dataptr, bool is_null) {
			reinterpret_cast<RLEAnalyzeState<T> *>(dataptr)->Observe(value, count);
		}
	};

	void Observe(T value, rle_count_t count) {
		if (!run_count) {
			min_value = max_value = value;
			min_count = max_count = count;
		} else {
			min_value = MinValue<T>(min_value, value);
			max_value = MaxValue<T>(max_value, value);
			min_count = MinValue<rle_count_t>(min_count, count);
			max_count = MaxValue<rle_count_t>(max_count, count);
		}
		run_count++;
	}

	RLEState<T> state;
	const bool packed;
	idx_t total_count = 0;
	idx_t run_count = 0;
	T min_value;
	T max_value;
	rle_count_t min_count;
	rle_count_t max_count;
};

template <class T>
unique_ptr<AnalyzeState> RLEInitAnalyze(CompressionAnalyzeContext &ctx, PhysicalType type) {
	const auto packed = IsSereneDBStorageVersion(ctx.storage_version);
	return make_uniq<RLEAnalyzeState<T>>(ctx.block_manager, packed);
}

template <class T>
bool RLEAnalyze(AnalyzeState &state, const Vector &input) {
	auto &rle_state = state.template Cast<RLEAnalyzeState<T>>();
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);

	auto data = UnifiedVectorFormat::GetData<T>(vdata);
	using Writer = typename RLEAnalyzeState<T>::RLEAnalyzeWriter;
	const idx_t count = input.size();
	if (!vdata.sel->IsSet() && vdata.validity.CannotHaveNull()) {
		rle_state.state.template UpdateFlatValid<Writer>(data, count);
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto idx = vdata.sel->get_index(i);
			rle_state.state.template Update<Writer>(data, vdata.validity, idx);
		}
	}
	rle_state.total_count += count;
	return true;
}

template <class T>
idx_t RLEFinalAnalyze(AnalyzeState &state) {
	auto &rle_state = state.template Cast<RLEAnalyzeState<T>>();
	if (rle_state.state.last_seen_count > 0) {
		rle_state.state.template Flush<typename RLEAnalyzeState<T>::RLEAnalyzeWriter>();
	}
	auto count = rle_state.run_count;
	if (!count) {
		return 0;
	}
	if (!rle_state.packed) {
		return (sizeof(rle_count_t) + sizeof(T)) * count;
	}
	if (count >= rle_state.total_count) {
		return DConstants::INVALID_INDEX;
	}

	T value_frame;
	rle_count_t count_frame;
	auto value_width = RLEValueCodec<T>::WidthForRange(rle_state.min_value, rle_state.max_value, value_frame);
	auto count_width = RLEValueCodec<rle_count_t>::WidthForRange(rle_state.min_count, rle_state.max_count, count_frame);

	auto max_entry_count = RLEMaxEntryCount<T>(rle_state.info.GetBlockSize(), true);
	auto full_size = RLEValueCodec<T>::PackedSize(max_entry_count, value_width) +
	                 RLEValueCodec<rle_count_t>::PackedSize(max_entry_count, count_width);
	auto rest = count % max_entry_count;
	return (count / max_entry_count) * full_size + RLEValueCodec<T>::PackedSize(rest, value_width) +
	       RLEValueCodec<rle_count_t>::PackedSize(rest, count_width);
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//
template <class T, bool WRITE_STATISTICS>
struct RLECompressState : public StandardCompressionState {
	using ValueCodec = RLEValueCodec<T>;
	using CountCodec = RLEValueCodec<rle_count_t>;

	explicit RLECompressState(ColumnDataCheckpointData &checkpoint_data_p)
	    : StandardCompressionState(checkpoint_data_p, CompressionType::COMPRESSION_RLE),
	      packed(
	          StorageManager::TargetAtLeastVersion(StorageVersion::SERENEDB_V1, checkpoint_data_p.GetStorageVersion())),
	      header_size(RLEHeaderSize<T>(packed)) {
		if (packed) {
			pack_buffer = make_unsafe_uniq_array<data_t>(checkpoint_data_p.GetBlockManager().GetBlockSize());
		}
		CreateEmptySegment();

		state.dataptr = (void *)this;
		max_rle_count = RLEMaxEntryCount<T>(info.GetBlockSize(), packed);
	}

	struct RLEWriter {
		template <class VALUE_TYPE>
		static void Operation(VALUE_TYPE value, rle_count_t count, void *dataptr, bool is_null) {
			auto state = reinterpret_cast<RLECompressState<T, WRITE_STATISTICS> *>(dataptr);
			state->WriteValue(value, count, is_null);
		}
	};

	void CreateEmptySegment() {
		CreateAndPinNewSegment();
	}

	void Append(UnifiedVectorFormat &vdata, idx_t count) {
		auto data = UnifiedVectorFormat::GetData<T>(vdata);
		using Writer = RLECompressState<T, WRITE_STATISTICS>::RLEWriter;
		if (!vdata.sel->IsSet() && vdata.validity.CannotHaveNull()) {
			state.template UpdateFlatValid<Writer>(data, count);
			return;
		}
		for (idx_t i = 0; i < count; i++) {
			auto idx = vdata.sel->get_index(i);
			if (WRITE_STATISTICS && !vdata.validity.RowIsValid(idx)) {
				stats_writer.SetHasNull();
			}
			state.template Update<Writer>(data, vdata.validity, idx);
		}
	}

	void WriteValue(T value, rle_count_t count, bool is_null) {
		StagedValues()[entry_count] = value;
		StagedCounts()[entry_count] = count;
		entry_count++;

		// update meta data
		if (WRITE_STATISTICS) {
			if (!is_null) {
				stats_writer.Update(value);
			} else {
				stats_writer.SetHasNull();
			}
		}
		current_segment->count += count;

		if (entry_count == max_rle_count) {
			// we have finished writing this segment: flush it and create a new segment
			FlushSegment();
			CreateEmptySegment();
			entry_count = 0;
		}
	}

	T *StagedValues() {
		return reinterpret_cast<T *>(handle.GetDataMutable() + header_size);
	}
	rle_count_t *StagedCounts() {
		return reinterpret_cast<rle_count_t *>(handle.GetDataMutable() + header_size + max_rle_count * sizeof(T));
	}

	void FlushSegment() {
		if (packed) {
			FlushPacked();
		} else {
			FlushNative();
		}
	}

	void FlushNative() {
		// flush the segment
		// we compact the segment by moving the counts so they are directly next to the values
		auto data_ptr = handle.GetDataMutable();
		idx_t counts_size = sizeof(rle_count_t) * entry_count;
		idx_t minimal_rle_offset = RLE_NATIVE_HEADER_SIZE + sizeof(T) * entry_count;
		idx_t aligned_rle_offset = AlignValue(minimal_rle_offset);
		if (aligned_rle_offset > minimal_rle_offset) {
			memset(data_ptr + minimal_rle_offset, 0, aligned_rle_offset - minimal_rle_offset);
		}
		memmove(data_ptr + aligned_rle_offset, StagedCounts(), counts_size);
		Store<uint64_t>(aligned_rle_offset, data_ptr);
		FlushCurrentSegment(stats_writer, aligned_rle_offset + counts_size);
	}

	void FlushPacked() {
		auto values = StagedValues();
		auto counts = StagedCounts();

		RLEHeader<T> header;
		header.entry_count = NumericCast<uint32_t>(entry_count);
		header.value_width = ValueCodec::Analyze(values, entry_count, header.value_frame);
		header.count_width = CountCodec::Analyze(counts, entry_count, header.count_frame);
		auto values_size = ValueCodec::PackedSize(entry_count, header.value_width);
		auto counts_size = CountCodec::PackedSize(entry_count, header.count_width);
		header.counts_offset = RLE_PACKED_MARKER | (header_size + values_size);

		ValueCodec::Pack(pack_buffer.get(), values, entry_count, header.value_frame, header.value_width);
		CountCodec::Pack(pack_buffer.get() + values_size, counts, entry_count, header.count_frame, header.count_width);

		auto data_ptr = handle.GetDataMutable();
		Store<RLEHeader<T>>(header, data_ptr);
		memcpy(data_ptr + header_size, pack_buffer.get(), values_size + counts_size);
		FlushCurrentSegment(stats_writer, header_size + values_size + counts_size);
	}

	void Finalize() {
		state.template Flush<RLECompressState<T, WRITE_STATISTICS>::RLEWriter>();

		FlushSegment();
		current_segment.reset();
	}

	RLEState<T> state;
	StatsWriter<T> stats_writer;
	const bool packed;
	const idx_t header_size;
	unsafe_unique_array<data_t> pack_buffer;
	idx_t entry_count = 0;
	idx_t max_rle_count;
};

template <class T, bool WRITE_STATISTICS>
unique_ptr<CompressionState> RLEInitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                unique_ptr<AnalyzeState> state) {
	return make_uniq<RLECompressState<T, WRITE_STATISTICS>>(checkpoint_data);
}

template <class T, bool WRITE_STATISTICS>
void RLECompress(CompressionState &state_p, const Vector &scan_vector) {
	auto &state = state_p.Cast<RLECompressState<T, WRITE_STATISTICS>>();
	UnifiedVectorFormat vdata;
	scan_vector.ToUnifiedFormat(vdata);

	state.Append(vdata, scan_vector.size());
}

template <class T, bool WRITE_STATISTICS>
void RLEFinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<RLECompressState<T, WRITE_STATISTICS>>();
	state.Finalize();
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
template <class T>
struct RLELayout {
	bool packed;
	idx_t entry_count;
	data_ptr_t values;
	data_ptr_t counts;
	T value_frame;
	rle_count_t count_frame;
	bitpacking_width_t value_width;
	bitpacking_width_t count_width;

	static RLELayout Parse(ColumnSegment &segment, data_ptr_t segment_data) {
		const auto marked = Load<uint64_t>(segment_data);
		const auto counts_offset = marked & ~RLE_PACKED_MARKER;
		RLELayout layout;
		layout.packed = (marked & RLE_PACKED_MARKER) != 0;
		const idx_t header_size = RLEHeaderSize<T>(layout.packed);
		if (counts_offset < header_size) {
			throw IOException("Corrupted RLE segment: counts_offset is corrupted");
		}
		if (segment.GetBlockOffset() + counts_offset > segment.GetBlockSize()) {
			throw IOException("Corrupted RLE segment: counts_offset is corrupted");
		}
		layout.values = segment_data + header_size;
		layout.counts = segment_data + counts_offset;
		const auto counts_room = segment.GetBlockSize() - segment.GetBlockOffset() - counts_offset;

		if (!layout.packed) {
			layout.entry_count = MinValue<idx_t>((counts_offset - header_size) / sizeof(T), segment.count.load());
			layout.value_frame = 0;
			layout.count_frame = 0;
			layout.value_width = sizeof(T) * 8;
			layout.count_width = sizeof(rle_count_t) * 8;
			if (layout.entry_count > counts_room / sizeof(rle_count_t)) {
				throw IOException("Corrupted RLE segment: counts_offset is corrupted");
			}
			return layout;
		}

		const auto header = Load<RLEHeader<T>>(segment_data);
		layout.entry_count = header.entry_count;
		layout.value_frame = header.value_frame;
		layout.count_frame = header.count_frame;
		layout.value_width = header.value_width;
		layout.count_width = header.count_width;
		if (layout.value_width > sizeof(T) * 8 || layout.count_width > sizeof(rle_count_t) * 8) {
			throw IOException("Corrupted RLE segment: bitpacking width exceeds the value width");
		}
		if (counts_offset < header_size + RLEValueCodec<T>::PackedSize(layout.entry_count, layout.value_width)) {
			throw IOException("Corrupted RLE segment: counts_offset is corrupted");
		}
		if (RLEValueCodec<rle_count_t>::PackedSize(layout.entry_count, layout.count_width) > counts_room) {
			throw IOException("Corrupted RLE segment: counts_offset is corrupted");
		}
		return layout;
	}
};

template <class T>
struct RLEScanState : public SegmentScanState {
	using ValueCodec = RLEValueCodec<T>;
	using CountCodec = RLEValueCodec<rle_count_t>;

	explicit RLEScanState(ColumnSegment &segment)
	    : handle(BufferManager::GetBufferManager(segment.GetDatabase()).Pin(segment.GetBlockHandle())), entry_pos(0),
	      position_in_entry(0),
	      layout(RLELayout<T>::Parse(segment, handle.GetDataMutable() + segment.GetBlockOffset())),
	      max_entry_pos(layout.entry_count) {
		if (layout.packed && max_entry_pos) {
			LoadGroup();
		}
	}

	void LoadGroup() {
		idx_t group_start = entry_pos & ~(RLE_GROUP_SIZE - 1);
		ValueCodec::Unpack(value_window, layout.values + group_start * layout.value_width / 8, RLE_GROUP_SIZE,
		                   layout.value_frame, layout.value_width);
		CountCodec::Unpack(count_window, layout.counts + group_start * layout.count_width / 8, RLE_GROUP_SIZE,
		                   layout.count_frame, layout.count_width);
	}

	inline T Value() const {
		if (!layout.packed) {
			return reinterpret_cast<const T *>(layout.values)[entry_pos];
		}
		return value_window[entry_pos & (RLE_GROUP_SIZE - 1)];
	}

	inline rle_count_t Count() const {
		if (!layout.packed) {
			return reinterpret_cast<const rle_count_t *>(layout.counts)[entry_pos];
		}
		return count_window[entry_pos & (RLE_GROUP_SIZE - 1)];
	}

	inline void SkipInternal(idx_t skip_count) {
		while (skip_count > 0) {
			rle_count_t run_end = Count();
			idx_t skip_amount = MinValue<idx_t>(skip_count, run_end - position_in_entry);

			skip_count -= skip_amount;
			position_in_entry += skip_amount;
			if (ExhaustedRun()) {
				ForwardToNextRun();
			}
		}
	}

	void Skip(ColumnSegment &segment, idx_t skip_count) {
		SkipInternal(skip_count);
	}

	inline void ForwardToNextRun() {
		// handled all entries in this RLE value
		// move to the next entry
		entry_pos++;
		if (entry_pos > max_entry_pos) {
			throw IOException("Corrupted RLE segment: entry_pos would reach outside of the blocks memory");
		}
		if (layout.packed && !(entry_pos & (RLE_GROUP_SIZE - 1)) && entry_pos < max_entry_pos) {
			LoadGroup();
		}
		position_in_entry = 0;
	}

	inline bool ExhaustedRun() {
		return position_in_entry >= Count();
	}

	//! Fill `result` with the runs starting at the cursor, with the layout resolved by the caller so
	//! nothing in the loop re-tests it. Returns how many values were written, which is `scan_count`
	//! unless the segment ran out of runs first.
	template <bool PACKED>
	idx_t FillRuns(T *result, idx_t scan_count) {
		idx_t written = 0;
		while (written < scan_count) {
			if (PACKED && position_in_entry == 0) {
				const idx_t lane = entry_pos & (RLE_GROUP_SIZE - 1);
				const idx_t runs = MinValue<idx_t>(RLE_GROUP_SIZE - lane, max_entry_pos - entry_pos);
				idx_t total = 0;
				for (idx_t r = 0; r < runs; r++) {
					total += count_window[lane + r];
				}
				if (total && total <= scan_count - written) {
					for (idx_t r = 0; r < runs; r++) {
						const T element = value_window[lane + r];
						const idx_t run_count = count_window[lane + r];
						for (idx_t i = 0; i < run_count; i++) {
							result[written + i] = element;
						}
						written += run_count;
					}
					entry_pos += runs;
					if (entry_pos < max_entry_pos) {
						LoadGroup();
					}
					continue;
				}
			}
			rle_count_t run_end;
			T element;
			if (PACKED) {
				const idx_t lane = entry_pos & (RLE_GROUP_SIZE - 1);
				run_end = count_window[lane];
				element = value_window[lane];
			} else {
				run_end = reinterpret_cast<const rle_count_t *>(layout.counts)[entry_pos];
				element = reinterpret_cast<const T *>(layout.values)[entry_pos];
			}
			const idx_t run_count = run_end - position_in_entry;
			const idx_t remaining = scan_count - written;
			if (DUCKDB_UNLIKELY(run_count > remaining)) {
				for (idx_t i = 0; i < remaining; i++) {
					result[written + i] = element;
				}
				position_in_entry += remaining;
				return scan_count;
			}
			for (idx_t i = 0; i < run_count; i++) {
				result[written + i] = element;
			}
			written += run_count;
			entry_pos++;
			if (DUCKDB_UNLIKELY(entry_pos > max_entry_pos)) {
				throw IOException("Corrupted RLE segment: entry_pos would reach outside of the blocks memory");
			}
			if (PACKED && !(entry_pos & (RLE_GROUP_SIZE - 1)) && entry_pos < max_entry_pos) {
				LoadGroup();
			}
			position_in_entry = 0;
		}
		return written;
	}

	idx_t Fill(T *result, idx_t scan_count) {
		return layout.packed ? FillRuns<true>(result, scan_count) : FillRuns<false>(result, scan_count);
	}

	BufferHandle handle;
	idx_t entry_pos;
	idx_t position_in_entry;
	const RLELayout<T> layout;
	//! If we are running a filter over the column - the runs that match the filter
	unsafe_unique_array<bool> matching_runs;
	idx_t matching_run_count = 0;

	const idx_t max_entry_pos;
	T value_window[RLE_GROUP_SIZE];
	rle_count_t count_window[RLE_GROUP_SIZE];
};

template <class T>
unique_ptr<SegmentScanState> RLEInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto result = make_uniq<RLEScanState<T>>(segment);
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Scan base data
//===--------------------------------------------------------------------===//
template <class T>
void RLESkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();
	scan_state.Skip(segment, skip_count);
}

template <bool ENTIRE_VECTOR>
static bool CanEmitConstantVector(idx_t position, idx_t run_length, idx_t scan_count) {
	if (!ENTIRE_VECTOR) {
		return false;
	}
	if (scan_count != STANDARD_VECTOR_SIZE) {
		// Only when we can fill an entire Vector can we emit a ConstantVector, because subsequent scans require the
		// input Vector to be flat
		return false;
	}
	D_ASSERT(position < run_length);
	auto remaining_in_run = run_length - position;
	// The amount of values left in this run are equal or greater than the amount of values we need to scan
	return remaining_in_run >= scan_count;
}

template <class T>
static void RLEScanConstant(RLEScanState<T> &scan_state, idx_t scan_count, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	FlatVector::SetSize(result, count_t(scan_count));
	auto result_data = ConstantVector::GetData<T>(result);
	result_data[0] = scan_state.Value();
	scan_state.position_in_entry += scan_count;
	if (scan_state.ExhaustedRun()) {
		scan_state.ForwardToNextRun();
	}
}

template <class T, bool ENTIRE_VECTOR>
void RLEScanPartialInternal(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                            idx_t result_offset) {
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();

	// If we are scanning an entire Vector and it contains only a single run
	if (CanEmitConstantVector<ENTIRE_VECTOR>(scan_state.position_in_entry, scan_state.Count(), scan_count)) {
		RLEScanConstant<T>(scan_state, scan_count, result);
		return;
	}

	auto result_data = FlatVector::GetDataMutable<T>(result);
	scan_state.Fill(result_data + result_offset, scan_count);
}

template <class T>
void RLEScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                    idx_t result_offset) {
	return RLEScanPartialInternal<T, false>(segment, state, scan_count, result, result_offset);
}

template <class T>
void RLEScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	RLEScanPartialInternal<T, true>(segment, state, scan_count, result, 0);
}

//===--------------------------------------------------------------------===//
// Select
//===--------------------------------------------------------------------===//
template <class T>
void RLESelect(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result,
               const SelectionVector &sel, idx_t sel_count) {
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();

	// If we are scanning an entire Vector and it contains only a single run we don't need to select at all
	if (CanEmitConstantVector<true>(scan_state.position_in_entry, scan_state.Count(), vector_count)) {
		RLEScanConstant<T>(scan_state, vector_count, result);
		return;
	}

	auto result_data = FlatVector::Writer<T>(result, sel_count);

	idx_t prev_idx = 0;
	for (idx_t i = 0; i < sel_count; i++) {
		auto next_idx = sel.get_index(i);
		if (next_idx < prev_idx) {
			throw InternalException("Error in RLESelect - selection vector indices are not ordered");
		}
		// skip forward to the next index
		scan_state.SkipInternal(next_idx - prev_idx);
		// read the element
		result_data.WriteValue(scan_state.Value());
		// move the next to the prev
		prev_idx = next_idx;
	}
	// skip the tail
	scan_state.SkipInternal(vector_count - prev_idx);
}

//===--------------------------------------------------------------------===//
// Filter
//===--------------------------------------------------------------------===//
template <class T>
void RLEFilter(ColumnSegment &segment, ColumnScanState &state, idx_t vector_count, Vector &result, SelectionVector &sel,
               idx_t &sel_count, const TableFilter &filter, TableFilterState &filter_state) {
	auto &scan_state = state.scan_state->Cast<RLEScanState<T>>();

	auto total_run_count = scan_state.max_entry_pos;
	if (!scan_state.matching_runs) {
		// we haven't applied the filter yet
		// apply the filter to all RLE values at once

		// initialize the filter set to all false (all runs are filtered out)
		scan_state.matching_runs = make_unsafe_uniq_array<bool>(total_run_count);
		memset(scan_state.matching_runs.get(), 0, sizeof(bool) * total_run_count);

		// this is the one path that needs every run value at once rather than the sequential window;
		// the native layout already has them contiguous in the block, so it filters in place
		unsafe_unique_array<T> run_values;
		auto &layout = scan_state.layout;
		auto values_ptr = layout.values;
		if (layout.packed) {
			auto padded_run_count = BitpackingPrimitives::RoundUpToAlgorithmGroupSize(total_run_count);
			run_values = make_unsafe_uniq_array<T>(padded_run_count);
			RLEScanState<T>::ValueCodec::Unpack(run_values.get(), values_ptr, padded_run_count, layout.value_frame,
			                                    layout.value_width);
			values_ptr = data_ptr_cast(run_values.get());
		}

		// execute the filter over all runs at once
		Vector run_vector(result.GetType(), values_ptr, total_run_count);

		SelectionVector run_matches;
		scan_state.matching_run_count = total_run_count;
		ColumnSegment::FilterSelection(run_matches, run_vector, filter_state, total_run_count,
		                               scan_state.matching_run_count);

		// for any runs that pass the filter - set the matches to true
		for (idx_t i = 0; i < scan_state.matching_run_count; i++) {
			auto idx = run_matches.get_index(i);
			scan_state.matching_runs[idx] = true;
		}
	}
	if (scan_state.matching_run_count == 0) {
		// early-out, no runs match the filter so the filter can never pass
		sel_count = 0;
		return;
	}
	// scan the matching runs like the plain scan would (one vectorized fill per run) and narrow
	// the selection to the rows they cover. The selection is only rebuilt from the first entry
	// that actually drops - a window whose candidate rows all land in matching runs costs exactly
	// a scan plus one flag check per run.
	auto result_data = FlatVector::GetDataMutable<T>(result);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	SelectionVector matching_sel;
	sel_t *out_sel = nullptr;
	idx_t matching_count = 0;
	idx_t pos = 0;
	idx_t sel_idx = 0;
	idx_t prev_row = 0;
	while (pos < vector_count) {
		rle_count_t run_end = scan_state.Count();
		idx_t run_count = run_end - scan_state.position_in_entry;
		idx_t take = MinValue<idx_t>(vector_count - pos, run_count);
		const bool match = scan_state.matching_runs[scan_state.entry_pos];
		if (match) {
			T element = scan_state.Value();
			std::fill(result_data + pos, result_data + pos + take, element);
		}
		// consume the selection entries that fall into this run slice
		idx_t run_begin_sel = sel_idx;
		while (sel_idx < sel_count && sel.get_index(sel_idx) < pos + take) {
			auto row = sel.get_index(sel_idx);
			if (row < prev_row) {
				throw InternalException("Error in RLEFilter - selection vector indices are not ordered");
			}
			prev_row = row;
			sel_idx++;
		}
		if (match) {
			if (out_sel) {
				for (idx_t i = run_begin_sel; i < sel_idx; i++) {
					out_sel[matching_count++] = UnsafeNumericCast<sel_t>(sel.get_index(i));
				}
			} else {
				// no entry dropped yet: the kept prefix is sel[0..sel_idx)
				matching_count = sel_idx;
			}
		} else if (sel_idx != run_begin_sel && !out_sel) {
			// first dropped entries: materialize the kept prefix and rebuild from here
			matching_sel.Initialize(sel_count);
			out_sel = matching_sel.data();
			for (idx_t i = 0; i < run_begin_sel; i++) {
				out_sel[i] = UnsafeNumericCast<sel_t>(sel.get_index(i));
			}
			matching_count = run_begin_sel;
		}
		scan_state.position_in_entry += take;
		if (scan_state.ExhaustedRun()) {
			scan_state.ForwardToNextRun();
		}
		pos += take;
	}

	// set up the filter result
	if (matching_count != sel_count) {
		sel.Initialize(matching_sel);
		sel_count = matching_count;
	}
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
template <class T>
void RLEFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result, idx_t result_idx) {
	RLEScanState<T> scan_state(segment);
	scan_state.Skip(segment, NumericCast<idx_t>(row_id));

	auto result_data = FlatVector::GetDataMutable<T>(result);
	result_data[result_idx] = scan_state.Value();
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
template <class T, bool WRITE_STATISTICS = true>
CompressionFunction GetRLEFunction(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_RLE, data_type, RLEInitAnalyze<T>, RLEAnalyze<T>,
	                           RLEFinalAnalyze<T>, RLEInitCompression<T, WRITE_STATISTICS>,
	                           RLECompress<T, WRITE_STATISTICS>, RLEFinalizeCompress<T, WRITE_STATISTICS>,
	                           RLEInitScan<T>, RLEScan<T>, RLEScanPartial<T>, RLEFetchRow<T>, RLESkip<T>, nullptr,
	                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, RLESelect<T>,
	                           RLEFilter<T>);
}

CompressionFunction RLEFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL: {
		auto function = GetRLEFunction<int8_t>(type);
		function.filter = nullptr;
		return function;
	}
	case PhysicalType::INT8:
		return GetRLEFunction<int8_t>(type);
	case PhysicalType::INT16:
		return GetRLEFunction<int16_t>(type);
	case PhysicalType::INT32:
		return GetRLEFunction<int32_t>(type);
	case PhysicalType::INT64:
		return GetRLEFunction<int64_t>(type);
	case PhysicalType::INT128:
		return GetRLEFunction<hugeint_t>(type);
	case PhysicalType::UINT128:
		return GetRLEFunction<uhugeint_t>(type);
	case PhysicalType::UINT8:
		return GetRLEFunction<uint8_t>(type);
	case PhysicalType::UINT16:
		return GetRLEFunction<uint16_t>(type);
	case PhysicalType::UINT32:
		return GetRLEFunction<uint32_t>(type);
	case PhysicalType::UINT64:
		return GetRLEFunction<uint64_t>(type);
	case PhysicalType::FLOAT:
		return GetRLEFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetRLEFunction<double>(type);
	case PhysicalType::LIST:
		return GetRLEFunction<uint64_t, false>(type);
	default:
		throw InternalException("Unsupported type for RLE");
	}
}

bool RLEFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
	case PhysicalType::LIST:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
