#include "duckdb.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/checkpoint/string_checkpoint_state.hpp"
#include "duckdb/storage/string_uncompressed.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

#include "duckdb/storage/compression/compression_options.hpp"
#include "duckdb/storage/compression/lz4/lz4.hpp"

#include "lz4.h"
#include "lz4hc.h"

/*
Data layout per segment:
+--------------------------------------------+
|            Segment Header                  |
|   +------------------------------------+   |
|   |   int64_t  dict_block_id           |   |
|   |   uint32_t dict_size               |   |
|   |   uint32_t reserved                |   |
|   +------------------------------------+   |
|            Vector Metadata                 |
|   +------------------------------------+   |
|   |   int64_t  page_id[]               |   |
|   |   uint32_t page_offset[]           |   |
|   |   uint64_t uncompressed_size[]     |   |
|   |   uint64_t compressed_size[]       |   |
|   +------------------------------------+   |
+--------------------------------------------+
|              [Vector Data]+                |
|   +------------------------------------+   |
|   |   uint32_t lengths[]               |   |
|   |   void    *compressed_data (lz4)   |   |
|   +------------------------------------+   |
+--------------------------------------------+
Each ~2048-row vector is an independent LZ4 block, so any vector can be
decompressed on its own (random access + fetch_row).
*/

namespace duckdb {

namespace {

constexpr idx_t LZ4_VECTOR_SIZE = STANDARD_VECTOR_SIZE > 2048 ? STANDARD_VECTOR_SIZE : 2048;

idx_t Lz4GetWritableSpace(const CompressionInfo &info) {
	return info.GetBlockSize() - sizeof(block_id_t);
}

idx_t Lz4GetVectorCount(idx_t count) {
	idx_t vector_count = count / LZ4_VECTOR_SIZE;
	vector_count += (count % LZ4_VECTOR_SIZE) != 0;
	return vector_count;
}

idx_t Lz4GetVectorMetadataSize(idx_t vector_count) {
	idx_t vector_metadata_size = LZ4_SEGMENT_HEADER_SIZE;
	vector_metadata_size += sizeof(lz4_page_id_t) * vector_count;

	vector_metadata_size = AlignValue<idx_t, sizeof(lz4_page_offset_t)>(vector_metadata_size);
	vector_metadata_size += sizeof(lz4_page_offset_t) * vector_count;

	vector_metadata_size = AlignValue<idx_t, sizeof(lz4_uncompressed_size_t)>(vector_metadata_size);
	vector_metadata_size += sizeof(lz4_uncompressed_size_t) * vector_count;

	vector_metadata_size = AlignValue<idx_t, sizeof(lz4_compressed_size_t)>(vector_metadata_size);
	vector_metadata_size += sizeof(lz4_compressed_size_t) * vector_count;
	return vector_metadata_size;
}

DictScope ParseDictScope(const string &scope) {
	auto lower = StringUtil::Lower(scope);
	if (lower == "row_group") {
		return DictScope::ROW_GROUP;
	}
	if (lower == "column") {
		return DictScope::COLUMN;
	}
	return DictScope::NONE;
}

CompressionOptions GetLz4Options(DBConfig &config) {
	CompressionOptions options;
	options.lz4_level = UnsafeNumericCast<int32_t>(Settings::Get<Lz4CompressionLevelSetting>(config));
	options.use_dictionary = Settings::Get<CompressionDictionarySetting>(config);
	options.dict_scope = ParseDictScope(Settings::Get<CompressionDictionaryScopeSetting>(config));
	options.effort = CompressEffort::FLUSH;
	return options;
}

bool DictionaryEnabled(const CompressionOptions &options) {
	return options.use_dictionary && options.dict_scope != DictScope::NONE;
}

//! Resolve the effective level: 0/negative => fast codec, >0 => HC at that level.
//! MERGE effort promotes the default (0) to the HC default level.
int32_t EffectiveLevel(const CompressionOptions &options) {
	int32_t level = options.lz4_level;
	if (options.effort == CompressEffort::MERGE && level <= 0) {
		level = LZ4HC_CLEVEL_DEFAULT;
	}
	return level;
}

} // namespace

struct LZ4Storage {
	static unique_ptr<AnalyzeState> StringInitAnalyze(CompressionAnalyzeContext &ctx, PhysicalType type);
	static bool StringAnalyze(AnalyzeState &state_p, const Vector &input);
	static idx_t StringFinalAnalyze(AnalyzeState &state_p);

	static unique_ptr<CompressionState> InitCompression(ColumnDataCheckpointData &checkpoint_data,
	                                                    unique_ptr<AnalyzeState> analyze_state_p);
	static void Compress(CompressionState &state_p, const Vector &scan_vector);
	static void FinalizeCompress(CompressionState &state_p);

	static unique_ptr<SegmentScanState> StringInitScan(const QueryContext &context, ColumnSegment &segment);
	static void StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
	                              idx_t result_offset);
	static void StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result);
	static void StringFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result,
	                           idx_t result_idx);
	static void StringSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count);

	static unique_ptr<CompressedSegmentState> StringInitSegment(ColumnSegment &segment, block_id_t block_id,
	                                                            optional_ptr<ColumnSegmentState> segment_state);
	static unique_ptr<ColumnSegmentState> SerializeState(ColumnSegment &segment);
	static unique_ptr<ColumnSegmentState> DeserializeState(Deserializer &deserializer);
	static void VisitBlockIds(const ColumnSegment &segment, BlockIdVisitor &visitor);
};

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//

struct LZ4AnalyzeState : public AnalyzeState {
public:
	LZ4AnalyzeState(BlockManager &block_manager, DBConfig &config)
	    : AnalyzeState(block_manager), config(config), options(GetLz4Options(config)) {
		dict_capacity = MinValue<idx_t>(options.max_dict_size, Lz4GetWritableSpace(info));
	}

public:
	inline void SampleDictionary(const string_t &str) {
		if (!DictionaryEnabled(options)) {
			return;
		}
		if (dictionary.size() >= dict_capacity) {
			return;
		}
		auto remaining = dict_capacity - dictionary.size();
		auto to_add = MinValue<idx_t>(remaining, str.GetSize());
		dictionary.append(str.GetData(), to_add);
	}

public:
	DBConfig &config;
	CompressionOptions options;
	idx_t dict_capacity = 0;
	//! Sampled dictionary bytes (only populated when the dictionary knob is on)
	string dictionary;

	//! The combined string lengths for all values in the segment
	idx_t total_size = 0;
	//! The total amount of values in the segment
	idx_t count = 0;

	//! The amount of vectors per filled segment
	idx_t vectors_per_segment = 0;
	//! The total amount of segments we will create
	idx_t segment_count = 0;
	//! Current vector in the segment
	idx_t vectors_in_segment = 0;
	//! Current amount of values in the vector
	idx_t values_in_vector = 0;
};

unique_ptr<AnalyzeState> LZ4Storage::StringInitAnalyze(CompressionAnalyzeContext &ctx, PhysicalType type) {
	if (ctx.block_manager.InMemory()) {
		//! Can't use the extra-page mechanism in an in-memory environment
		return nullptr;
	}
	if (StorageManager::IsPriorToVersion(StorageVersion::V1_2_0, ctx.storage_version)) {
		// compatibility mode with old versions - disable lz4
		return nullptr;
	}
	auto &config = DBConfig::GetConfig(ctx.db);
	return make_uniq<LZ4AnalyzeState>(ctx.block_manager, config);
}

bool LZ4Storage::StringAnalyze(AnalyzeState &state_p, const Vector &input) {
	auto &state = state_p.Cast<LZ4AnalyzeState>();
	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);

	const auto count = input.size();
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		auto &str = data[idx];
		state.total_size += str.GetSize();
		state.SampleDictionary(str);
	}
	state.values_in_vector += count;
	while (state.values_in_vector >= LZ4_VECTOR_SIZE) {
		if (Lz4GetVectorMetadataSize(state.vectors_in_segment + 1) > Lz4GetWritableSpace(state.info)) {
			state.vectors_per_segment = state.vectors_in_segment;
			state.segment_count++;
			state.vectors_in_segment = 0;
		} else {
			state.vectors_in_segment++;
		}
		state.values_in_vector -= LZ4_VECTOR_SIZE;
	}
	state.count += count;
	return true;
}

idx_t LZ4Storage::StringFinalAnalyze(AnalyzeState &state_p) {
	auto &state = state_p.Cast<LZ4AnalyzeState>();

	if (!state.count) {
		return NumericLimits<idx_t>::Maximum();
	}

	if (state.values_in_vector) {
		D_ASSERT(state.values_in_vector < LZ4_VECTOR_SIZE);
		state.vectors_in_segment++;
	}
	if (state.vectors_in_segment) {
		state.segment_count++;
	}

	// Rough estimate: LZ4 typically compresses text to ~half. We deliberately apply a large
	// penalty so this codec is (almost) never chosen automatically - it stays reachable via
	// force_compression / a per-column setting without regressing the auto-selection defaults.
	auto expected_compressed_size = (double)state.total_size / 2.0;

	idx_t estimated_size = 0;
	estimated_size += LossyNumericCast<idx_t>(expected_compressed_size);
	estimated_size += state.count * sizeof(lz4_string_length_t);
	estimated_size += Lz4GetVectorMetadataSize(Lz4GetVectorCount(state.count));

	constexpr double AUTO_SELECT_PENALTY = 1000.0;
	return LossyNumericCast<idx_t>((double)estimated_size * AUTO_SELECT_PENALTY);
}

//===--------------------------------------------------------------------===//
// Compress
//===--------------------------------------------------------------------===//

class LZ4CompressionState : public CompressionState {
public:
	explicit LZ4CompressionState(ColumnDataCheckpointData &checkpoint_data,
	                             unique_ptr<LZ4AnalyzeState> &&analyze_state_p)
	    : CompressionState(checkpoint_data, CompressionType::COMPRESSION_LZ4),
	      analyze_state(std::move(analyze_state_p)), stats_writer(checkpoint_data.GetType()),
	      options(analyze_state->options), total_tuple_count(analyze_state->count),
	      total_vector_count(Lz4GetVectorCount(total_tuple_count)), total_segment_count(analyze_state->segment_count),
	      vectors_per_segment(analyze_state->vectors_per_segment) {
		dictionary = std::move(analyze_state->dictionary);
		segment_count = 0;
		vector_count = 0;
		vector_state.tuple_count = 0;

		NewSegment();
		if (!(buffer_collection.GetCurrentOffset() <= Lz4GetWritableSpace(info))) {
			throw InternalException("(LZ4CompressionState) Offset (%d) exceeds writable space! (%d)",
			                        buffer_collection.GetCurrentOffset(), Lz4GetWritableSpace(info));
		}
	}

public:
	void WriteBlockIdPointer(lz4_page_id_t block_id) {
		auto ptr = buffer_collection.GetCurrentBufferPtr();
		Store<block_id_t>(block_id, ptr);
		buffer_collection.GetCurrentOffset() += sizeof(block_id_t);
	}

	void GetExtraPageBuffer(block_id_t current_block_id) {
		auto &buffer_manager = block_manager.buffer_manager;

		auto &current_buffer_state = buffer_collection.GetCurrentBufferState();
		current_buffer_state.full = true;

		if (buffer_collection.CanFlush()) {
			auto &buffer_state = buffer_collection.GetCurrentBufferState();
			FlushPage(buffer_collection.BufferHandleMutable(), current_block_id);
			buffer_state.flags.Clear();
			buffer_state.full = false;
			buffer_state.offset = 0;
			return;
		}

		auto buffer_data = buffer_collection.GetBufferData(/*include_segment = */ false);
		for (auto &buffer : buffer_data) {
			auto &buffer_state = buffer.state;
			auto &flags = buffer_state.flags;
			if (flags.HasStringMetadata() || buffer_state.full) {
				continue;
			}
			buffer_collection.SetCurrentBuffer(buffer.slot);
			auto &buffer_handle = buffer_collection.BufferHandleMutable();
			if (!buffer_handle.IsValid()) {
				buffer_handle = buffer_manager.Allocate(MemoryTag::OVERFLOW_STRINGS, &block_manager);
			}
			return;
		}
		throw InternalException(
		    "(LZ4CompressionState::GetExtraPageBuffer) Wasn't able to find a buffer to write overflow data to!");
	}

	void NewSegment() {
		if (buffer_collection.IsOnSegmentBuffer()) {
			throw InternalException("(LZ4CompressionState::NewSegment) We are asking for a new segment, but somehow "
			                        "we're still writing vector data onto the initial (segment) page");
		}
		FlushSegment();
		CreateEmptySegment();

		idx_t vectors_in_segment;
		if (segment_count + 1 >= total_segment_count) {
			vectors_in_segment = total_vector_count - (segment_count * vectors_per_segment);
		} else {
			vectors_in_segment = vectors_per_segment;
		}

		buffer_collection.SetCurrentBuffer(LZ4CompressionBufferCollection::Slot::SEGMENT);
		buffer_collection.buffer_states[0].flags.SetVectorMetadata();
		segment_state.InitializeSegment(buffer_collection, vectors_in_segment);
		WriteDictionaryBlock();
		if (!(buffer_collection.GetCurrentOffset() <= Lz4GetWritableSpace(info))) {
			throw InternalException("(LZ4CompressionState::NewSegment) Offset (%d) exceeds writable space! (%d)",
			                        buffer_collection.GetCurrentOffset(), Lz4GetWritableSpace(info));
		}
	}

	//! Persist the (optional) shared dictionary on its own block, once per segment.
	void WriteDictionaryBlock() {
		if (dictionary.empty()) {
			return;
		}
		auto dict_size = UnsafeNumericCast<uint32_t>(dictionary.size());
		auto new_id = checkpoint_data.GetFreeBlockId();
		auto &state = buffer_collection.segment->GetSegmentState()->Cast<UncompressedStringSegmentState>();
		state.RegisterBlock(block_manager, new_id);

		auto &buffer_manager = block_manager.buffer_manager;
		auto handle = buffer_manager.Allocate(MemoryTag::OVERFLOW_STRINGS, &block_manager);
		memcpy(handle.GetDataMutable(), dictionary.data(), dictionary.size());
		block_manager.Write(QueryContext(), handle.GetFileBuffer(), new_id);

		segment_state.SetDictionary(buffer_collection, new_id, dict_size);
	}

	void InitializeVector() {
		D_ASSERT(!vector_state.in_vector);
		idx_t expected_tuple_count;
		if (vector_count + 1 >= total_vector_count) {
			expected_tuple_count = analyze_state->count - (LZ4_VECTOR_SIZE * vector_count);
		} else {
			expected_tuple_count = LZ4_VECTOR_SIZE;
		}
		buffer_collection.AlignCurrentOffset();
		if (!(buffer_collection.GetCurrentOffset() <= Lz4GetWritableSpace(info))) {
			throw InternalException("(LZ4CompressionState::InitializeVector) Offset (%d) exceeds writable space! (%d)",
			                        buffer_collection.GetCurrentOffset(), Lz4GetWritableSpace(info));
		}
		vector_state.compressed_size = 0;
		vector_state.uncompressed_size = 0;
		vector_state.string_lengths = nullptr;
		vector_state.tuple_count = 0;
		vector_state.vector_size = 0;
		vector_state.starting_page = 0XDEADBEEF;
		vector_state.starting_offset = 0XDEADBEEF;
		input_buffer.clear();

		if (segment_state.vector_in_segment_count + 1 > segment_state.total_vectors_in_segment) {
			NewSegment();
		}

		if (buffer_collection.GetCurrentOffset() + (expected_tuple_count * sizeof(lz4_string_length_t)) >=
		    Lz4GetWritableSpace(info)) {
			NewPage();
		}

		buffer_collection.AlignCurrentOffset();
		vector_state.Initialize(expected_tuple_count, buffer_collection, info);
		vector_state.in_vector = true;
	}

	//! Compress the accumulated vector data (one LZ4 block) and stream it onto the page(s).
	void FinalizeVectorCompression() {
		auto uncompressed_size = UnsafeNumericCast<int>(input_buffer.size());
		vector_state.uncompressed_size = input_buffer.size();
		if (uncompressed_size == 0) {
			vector_state.compressed_size = 0;
			return;
		}
		if (input_buffer.size() > UnsafeNumericCast<idx_t>(LZ4_MAX_INPUT_SIZE)) {
			throw InternalException("(LZ4CompressionState) Vector too large to compress with LZ4 (%d bytes)",
			                        input_buffer.size());
		}

		auto bound = UnsafeNumericCast<idx_t>(LZ4_compressBound(uncompressed_size));
		if (compress_buffer.GetSize() < bound) {
			compress_buffer = Allocator::DefaultAllocator().Allocate(bound);
		}
		auto dst = char_ptr_cast(compress_buffer.get());
		auto src = input_buffer.data();
		int level = EffectiveLevel(options);

		int compressed;
		if (!dictionary.empty()) {
			auto dict = dictionary.data();
			auto dict_size = UnsafeNumericCast<int>(dictionary.size());
			if (level <= 0) {
				auto stream = LZ4_createStream();
				LZ4_loadDict(stream, dict, dict_size);
				compressed = LZ4_compress_fast_continue(stream, src, dst, uncompressed_size,
				                                        UnsafeNumericCast<int>(bound), 1);
				LZ4_freeStream(stream);
			} else {
				auto stream = LZ4_createStreamHC();
				LZ4_resetStreamHC_fast(stream, level);
				LZ4_loadDictHC(stream, dict, dict_size);
				compressed = LZ4_compress_HC_continue(stream, src, dst, uncompressed_size,
				                                      UnsafeNumericCast<int>(bound));
				LZ4_freeStreamHC(stream);
			}
		} else {
			if (level <= 0) {
				compressed = LZ4_compress_default(src, dst, uncompressed_size, UnsafeNumericCast<int>(bound));
			} else {
				compressed = LZ4_compress_HC(src, dst, uncompressed_size, UnsafeNumericCast<int>(bound), level);
			}
		}
		if (compressed <= 0) {
			throw InvalidInputException("LZ4 Compression failed");
		}
		WriteCompressedData(compress_buffer.get(), UnsafeNumericCast<idx_t>(compressed));
	}

	void WriteCompressedData(const_data_ptr_t src, idx_t size) {
		idx_t src_offset = 0;
		idx_t remaining = size;
		while (remaining) {
			auto current_offset = buffer_collection.GetCurrentOffset();
			if (current_offset >= Lz4GetWritableSpace(info)) {
				NewPage();
				continue;
			}
			idx_t page_room = Lz4GetWritableSpace(info) - current_offset;
			idx_t to_copy = MinValue<idx_t>(remaining, page_room);
			memcpy(buffer_collection.GetCurrentBufferPtr(), src + src_offset, to_copy);
			buffer_collection.GetCurrentOffset() += UnsafeNumericCast<lz4_page_offset_t>(to_copy);
			vector_state.compressed_size += to_copy;
			src_offset += to_copy;
			remaining -= to_copy;
			if (remaining) {
				NewPage();
			}
		}
	}

	void AddStringInternal(const string_t &string) {
		if (!vector_state.tuple_count) {
			InitializeVector();
		}

		auto is_final_string = vector_state.AddStringLength(string);
		input_buffer.append(string.GetData(), string.GetSize());
		if (is_final_string) {
			FinalizeVectorCompression();
			FlushVector();
		}
	}

	void AddString(const string_t &string) {
		AddStringInternal(string);
		stats_writer.Update(string);
	}

	void NewPage() {
		block_id_t new_id = FinalizePage();
		block_id_t current_block_id = buffer_collection.GetCurrentId();
		GetExtraPageBuffer(current_block_id);
		buffer_collection.block_id = new_id;
	}

	block_id_t FinalizePage() {
		auto new_id = checkpoint_data.GetFreeBlockId();

		auto &state = buffer_collection.segment->GetSegmentState()->Cast<UncompressedStringSegmentState>();
		state.RegisterBlock(block_manager, new_id);

		auto &buffer_state = buffer_collection.GetCurrentBufferState();
		buffer_state.full = true;

		WriteBlockIdPointer(new_id);
		D_ASSERT(buffer_state.offset <= info.GetBlockSize());
		return new_id;
	}

	void FlushPage(BufferHandle &buffer, block_id_t block_id) {
		if (block_id == INVALID_BLOCK) {
			return;
		}
		block_manager.Write(QueryContext(), buffer.GetFileBuffer(), block_id);
	}

	void FlushVector() {
		segment_state.page_ids[segment_state.vector_in_segment_count] = vector_state.starting_page;
		segment_state.page_offsets[segment_state.vector_in_segment_count] = vector_state.starting_offset;
		segment_state.compressed_sizes[segment_state.vector_in_segment_count] = vector_state.compressed_size;
		segment_state.uncompressed_sizes[segment_state.vector_in_segment_count] = vector_state.uncompressed_size;
		if (segment_state.vector_in_segment_count >= segment_state.total_vectors_in_segment) {
			throw InternalException(
			    "(LZ4CompressionState::FlushVector) Written too many vectors (%d) to this segment! (expected: %d)",
			    segment_state.vector_in_segment_count, segment_state.total_vectors_in_segment);
		}
		vector_count++;
		segment_state.vector_in_segment_count++;
		vector_state.in_vector = false;
		buffer_collection.segment->count += vector_state.tuple_count;

		vector_state.tuple_count = 0;

		auto buffer_data = buffer_collection.GetBufferData(/*include_segment=*/true);
		LZ4CompressionBufferCollection::Slot slot = LZ4CompressionBufferCollection::Slot::SEGMENT;
		optional_ptr<BufferHandle> buffer_handle_ptr;
		optional_ptr<LZ4CompressionBufferState> buffer_state_ptr;
		for (auto &buffer : buffer_data) {
			auto &buffer_state = buffer.state;
			if (buffer_state.flags.HasStringMetadata()) {
				if (buffer_handle_ptr) {
					throw InternalException("(LZ4CompressionState::FlushVector) Multiple buffers (%d and %d) have "
					                        "string metadata on them, this is impossible and indicates a bug!",
					                        static_cast<uint8_t>(slot), static_cast<uint8_t>(buffer.slot));
				}
				slot = buffer.slot;
				buffer_state_ptr = buffer.state;
				buffer_handle_ptr = buffer.handle;
			}
			buffer_state.flags.UnsetStringMetadata();
			buffer_state.flags.UnsetData();
		}

		if (!buffer_handle_ptr) {
			throw InternalException("(LZ4CompressionState::FlushVector) None of the buffers have string metadata on "
			                        "them, this is impossible and indicates a bug!");
		}
		if (slot == LZ4CompressionBufferCollection::Slot::SEGMENT) {
			return;
		}
		auto &buffer_state = *buffer_state_ptr;
		if (!buffer_state.full) {
			return;
		}
		auto &buffer_handle = *buffer_handle_ptr;
		FlushPage(buffer_handle, vector_state.starting_page);
		buffer_state.offset = 0;
		buffer_state.full = false;
	}

	void CreateEmptySegment() {
		auto compressed_segment = CreateNewSegment();
		buffer_collection.segment = std::move(compressed_segment);
		stats_writer.Clear();

		auto &buffer_manager = BufferManager::GetBufferManager(checkpoint_data.GetDatabase());
		buffer_collection.segment_handle = buffer_manager.Pin(buffer_collection.segment->GetBlockHandle());
	}

	void FlushSegment() {
		if (!buffer_collection.segment) {
			return;
		}
		if (segment_state.vector_in_segment_count != segment_state.total_vectors_in_segment) {
			throw InternalException("(LZ4CompressionState::FlushSegment) We haven't written all vectors that we were "
			                        "expecting to write (%d instead of %d)!",
			                        segment_state.vector_in_segment_count, segment_state.total_vectors_in_segment);
		}

		auto &segment_buffer_state = buffer_collection.buffer_states[0];
		auto segment_block_size = segment_buffer_state.offset;
		if (segment_block_size < Lz4GetVectorMetadataSize(segment_state.total_vectors_in_segment)) {
			throw InternalException(
			    "(LZ4CompressionState::FlushSegment) Expected offset to be at least %d, but found %d instead",
			    Lz4GetVectorMetadataSize(segment_state.total_vectors_in_segment), segment_block_size);
		}

		bool seen_dirty_buffer = false;
		auto buffer_data = buffer_collection.GetBufferData(/*include_segment=*/false);
		for (auto &buffer : buffer_data) {
			auto &buffer_state = buffer.state;
			auto &buffer_handle = buffer.handle;
			if (buffer_state.offset != 0) {
				if (seen_dirty_buffer) {
					throw InternalException("(LZ4CompressionState::FlushSegment) Both extra pages were dirty (needed "
					                        "to be flushed), this should be impossible");
				}
				FlushPage(buffer_handle, buffer_collection.block_id);
				buffer_state.full = false;
				buffer_state.offset = 0;
				buffer_state.flags.Clear();
				seen_dirty_buffer = true;
			}
		}

		checkpoint_data.FlushSegment(std::move(buffer_collection.segment), std::move(buffer_collection.segment_handle),
		                             segment_block_size);
		segment_buffer_state.flags.Clear();
		segment_buffer_state.full = true;
		segment_buffer_state.offset = 0;
		segment_count++;
	}

	void Finalize() {
		D_ASSERT(!vector_state.tuple_count);
		FlushSegment();
		buffer_collection.segment.reset();
	}

	void AddNull() {
		stats_writer.SetHasNull();
		string_t empty(static_cast<uint32_t>(0));
		AddStringInternal(empty);
	}

public:
	unique_ptr<LZ4AnalyzeState> analyze_state;
	StatsWriter<string_t> stats_writer;
	CompressionOptions options;
	//! Shared dictionary bytes (empty when the dictionary knob is off)
	string dictionary;

	const idx_t total_tuple_count;
	const idx_t total_vector_count;
	const idx_t total_segment_count;
	const idx_t vectors_per_segment;

	idx_t vector_count = 0;
	idx_t segment_count = 0;

	LZ4CompressionBufferCollection buffer_collection;

	//! Raw bytes of the current vector, awaiting a single block compression
	string input_buffer;
	//! Reusable destination for the compressed block
	AllocatedData compress_buffer;

	LZ4CompressionVectorState vector_state;
	LZ4CompressionSegmentState segment_state;
};

unique_ptr<CompressionState> LZ4Storage::InitCompression(ColumnDataCheckpointData &checkpoint_data,
                                                         unique_ptr<AnalyzeState> analyze_state_p) {
	return make_uniq<LZ4CompressionState>(checkpoint_data,
	                                      unique_ptr_cast<AnalyzeState, LZ4AnalyzeState>(std::move(analyze_state_p)));
}

void LZ4Storage::Compress(CompressionState &state_p, const Vector &input) {
	auto &state = state_p.Cast<LZ4CompressionState>();

	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);

	for (idx_t i = 0; i < input.size(); i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			state.AddNull();
			continue;
		}
		state.AddString(data[idx]);
	}
}

void LZ4Storage::FinalizeCompress(CompressionState &state_p) {
	auto &state = state_p.Cast<LZ4CompressionState>();
	state.Finalize();
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
struct LZ4VectorScanMetadata {
	idx_t vector_idx;
	block_id_t block_id;
	lz4_page_offset_t block_offset;
	lz4_uncompressed_size_t uncompressed_size;
	lz4_compressed_size_t compressed_size;
	idx_t count;
};

struct LZ4VectorScanState {
public:
	LZ4VectorScanState() {
	}
	LZ4VectorScanState(LZ4VectorScanState &&other) = default;
	LZ4VectorScanState(const LZ4VectorScanState &other) = delete;

public:
	LZ4VectorScanMetadata metadata;
	//! The (uncompressed) string lengths for this vector (copied, owns memory)
	vector<lz4_string_length_t> string_lengths;
	//! The fully decompressed vector data
	AllocatedData uncompressed_data;
	//! The amount of values already consumed from the state
	idx_t scanned_count = 0;
	//! Byte offset within uncompressed_data corresponding to scanned_count
	idx_t byte_offset = 0;
};

struct LZ4ScanState : public SegmentScanState {
public:
	explicit LZ4ScanState(ColumnSegment &segment)
	    : state(segment.GetSegmentState()->Cast<UncompressedStringSegmentState>()),
	      block_manager(segment.GetBlockHandle()->GetBlockManager()),
	      buffer_manager(BufferManager::GetBufferManager(segment.GetDatabase())),
	      segment_block_offset(segment.GetBlockOffset()), segment(segment) {
		segment_handle = buffer_manager.Pin(segment.GetBlockHandle());

		auto data = segment_handle.GetDataMutable() + segment.GetBlockOffset();

		// Read the segment header (dictionary metadata)
		dict_block_id = Load<int64_t>(data);
		dict_size = Load<uint32_t>(data + sizeof(int64_t));
		if (dict_block_id != INVALID_BLOCK && dict_size > 0) {
			auto dict_block = state.GetHandle(block_manager, dict_block_id);
			dict_handle = buffer_manager.Pin(dict_block);
			dictionary = char_ptr_cast(dict_handle.GetDataMutable());
		}

		idx_t offset = LZ4_SEGMENT_HEADER_SIZE;

		segment_count = segment.count.load();
		idx_t amount_of_vectors = (segment_count / LZ4_VECTOR_SIZE) + ((segment_count % LZ4_VECTOR_SIZE) != 0);

		offset = AlignValue<idx_t, sizeof(lz4_page_id_t)>(offset);
		page_ids = reinterpret_cast<lz4_page_id_t *>(data + offset);
		offset += (sizeof(lz4_page_id_t) * amount_of_vectors);

		offset = AlignValue<idx_t, sizeof(lz4_page_offset_t)>(offset);
		page_offsets = reinterpret_cast<lz4_page_offset_t *>(data + offset);
		offset += (sizeof(lz4_page_offset_t) * amount_of_vectors);

		offset = AlignValue<idx_t, sizeof(lz4_uncompressed_size_t)>(offset);
		uncompressed_sizes = reinterpret_cast<lz4_uncompressed_size_t *>(data + offset);
		offset += (sizeof(lz4_uncompressed_size_t) * amount_of_vectors);

		offset = AlignValue<idx_t, sizeof(lz4_compressed_size_t)>(offset);
		compressed_sizes = reinterpret_cast<lz4_compressed_size_t *>(data + offset);
		offset += (sizeof(lz4_compressed_size_t) * amount_of_vectors);

		scanned_count = 0;
	}
	~LZ4ScanState() override {
	}

public:
	idx_t GetVectorIndex(idx_t start_index, idx_t &offset) {
		idx_t vector_idx = start_index / LZ4_VECTOR_SIZE;
		offset = start_index % LZ4_VECTOR_SIZE;
		return vector_idx;
	}

	LZ4VectorScanMetadata GetVectorMetadata(idx_t vector_idx) {
		idx_t previous_value_count = vector_idx * LZ4_VECTOR_SIZE;
		idx_t value_count = MinValue<idx_t>(segment_count - previous_value_count, LZ4_VECTOR_SIZE);

		return LZ4VectorScanMetadata {/* vector_idx = */ vector_idx,
		                              /* block_id = */ page_ids[vector_idx],
		                              /* block_offset = */ page_offsets[vector_idx],
		                              /* uncompressed_size = */ uncompressed_sizes[vector_idx],
		                              /* compressed_size = */ compressed_sizes[vector_idx],
		                              /* count = */ value_count};
	}

	shared_ptr<BlockHandle> LoadPage(block_id_t block_id) {
		return state.GetHandle(block_manager, block_id);
	}

	bool UseVectorStateCache(idx_t vector_idx, idx_t internal_offset) {
		if (!current_vector) {
			return false;
		}
		if (current_vector->metadata.vector_idx != vector_idx) {
			return false;
		}
		if (current_vector->scanned_count != internal_offset) {
			return false;
		}
		return true;
	}

	//! Gather the (possibly page-spanning) compressed frame into a contiguous buffer.
	void GatherCompressedData(data_ptr_t handle_start, idx_t current_offset, const LZ4VectorScanMetadata &metadata,
	                          idx_t string_lengths_size, data_ptr_t destination) {
		idx_t copied = 0;
		idx_t remaining = metadata.compressed_size;

		auto src = handle_start + current_offset;
		idx_t page_bytes;
		if (metadata.block_offset + string_lengths_size + metadata.compressed_size >
		    (segment.SegmentSize() - sizeof(block_id_t))) {
			page_bytes = MinValue<idx_t>(remaining, block_manager.GetBlockSize() - sizeof(block_id_t) - current_offset);
		} else {
			page_bytes = MinValue<idx_t>(remaining, block_manager.GetBlockSize() - current_offset);
		}

		memcpy(destination + copied, src, page_bytes);
		copied += page_bytes;
		remaining -= page_bytes;

		while (remaining) {
			// Next block id is stored right after the consumed bytes on this page
			block_id_t next_id = Load<block_id_t>(src + page_bytes);
			auto block = LoadPage(next_id);
			auto handle = buffer_manager.Pin(block);
			auto ptr = handle.GetDataMutable();
			extra_handles.push_back(std::move(handle));

			idx_t page_size = segment.SegmentSize() - sizeof(block_id_t);
			page_bytes = MinValue<idx_t>(page_size, remaining);
			memcpy(destination + copied, ptr, page_bytes);
			copied += page_bytes;
			remaining -= page_bytes;
			src = ptr;
		}
	}

	LZ4VectorScanState &LoadVector(idx_t vector_idx, idx_t internal_offset) {
		if (UseVectorStateCache(vector_idx, internal_offset)) {
			return *current_vector;
		}
		current_vector = make_uniq<LZ4VectorScanState>();
		current_vector->metadata = GetVectorMetadata(vector_idx);
		auto &metadata = current_vector->metadata;
		auto &scan_state = *current_vector;
		extra_handles.clear();

		data_ptr_t handle_start;
		idx_t ptr_offset = 0;
		BufferHandle local_handle;
		if (metadata.block_id == INVALID_BLOCK) {
			handle_start = segment_handle.GetDataMutable();
			ptr_offset += segment_block_offset;
		} else {
			auto block = LoadPage(metadata.block_id);
			local_handle = buffer_manager.Pin(block);
			handle_start = local_handle.GetDataMutable();
		}

		ptr_offset += metadata.block_offset;
		ptr_offset = AlignValue<idx_t, sizeof(lz4_string_length_t)>(ptr_offset);

		auto vector_size = metadata.count;
		auto string_lengths_size = (sizeof(lz4_string_length_t) * vector_size);
		auto lengths_ptr = reinterpret_cast<lz4_string_length_t *>(handle_start + ptr_offset);
		scan_state.string_lengths.assign(lengths_ptr, lengths_ptr + vector_size);

		idx_t current_offset = ptr_offset + string_lengths_size;

		if (metadata.compressed_size > 0) {
			AllocatedData compressed = Allocator::DefaultAllocator().Allocate(metadata.compressed_size);
			GatherCompressedData(handle_start, current_offset, metadata, string_lengths_size, compressed.get());

			scan_state.uncompressed_data = Allocator::DefaultAllocator().Allocate(metadata.uncompressed_size);
			int result = LZ4_decompress_safe_usingDict(
			    char_ptr_cast(compressed.get()), char_ptr_cast(scan_state.uncompressed_data.get()),
			    UnsafeNumericCast<int>(metadata.compressed_size),
			    UnsafeNumericCast<int>(metadata.uncompressed_size), dictionary,
			    UnsafeNumericCast<int>(dict_size));
			if (result < 0 || UnsafeNumericCast<idx_t>(result) != metadata.uncompressed_size) {
				throw InvalidInputException("LZ4 Decompression failed (got %d, expected %d)", result,
				                            metadata.uncompressed_size);
			}
		}

		scan_state.scanned_count = 0;
		scan_state.byte_offset = 0;

		if (internal_offset) {
			Skip(scan_state, internal_offset);
		}
		return scan_state;
	}

	void Skip(LZ4VectorScanState &scan_state, idx_t count) {
		D_ASSERT(scan_state.scanned_count + count <= scan_state.metadata.count);
		for (idx_t i = 0; i < count; i++) {
			scan_state.byte_offset += scan_state.string_lengths[scan_state.scanned_count + i];
		}
		scan_state.scanned_count += count;
		scanned_count += count;
	}

	void ScanInternal(LZ4VectorScanState &scan_state, idx_t count, Vector &result, idx_t result_offset) {
		D_ASSERT(scan_state.scanned_count + count <= scan_state.metadata.count);
		D_ASSERT(result.GetType().InternalType() == PhysicalType::VARCHAR);

		auto string_data = FlatVector::GetDataMutable<string_t>(result);
		auto base = scan_state.uncompressed_data.get();
		idx_t offset = scan_state.byte_offset;
		for (idx_t i = 0; i < count; i++) {
			auto len = scan_state.string_lengths[scan_state.scanned_count + i];
			if (len == 0) {
				string_data[result_offset + i] = string_t(static_cast<uint32_t>(0));
			} else {
				string_data[result_offset + i] = StringVector::AddString(result, char_ptr_cast(base + offset), len);
			}
			offset += len;
		}
		scan_state.byte_offset = offset;
		scan_state.scanned_count += count;
		scanned_count += count;
	}

	void ScanPartial(idx_t start_idx, Vector &result, idx_t offset, idx_t count) {
		idx_t remaining = count;
		idx_t scanned = 0;
		while (remaining) {
			idx_t internal_offset;
			idx_t vector_idx = GetVectorIndex(start_idx + scanned, internal_offset);
			auto &scan_state = LoadVector(vector_idx, internal_offset);
			idx_t remaining_in_vector = scan_state.metadata.count - scan_state.scanned_count;
			idx_t to_scan = MinValue<idx_t>(remaining, remaining_in_vector);
			ScanInternal(scan_state, to_scan, result, offset + scanned);
			remaining -= to_scan;
			scanned += to_scan;
		}
		D_ASSERT(scanned == count);
	}

public:
	UncompressedStringSegmentState &state;
	BlockManager &block_manager;
	BufferManager &buffer_manager;

	idx_t segment_block_offset;
	BufferHandle segment_handle;

	//! Dictionary (if present) pinned for the segment lifetime
	BufferHandle dict_handle;
	const char *dictionary = nullptr;
	block_id_t dict_block_id = INVALID_BLOCK;
	uint32_t dict_size = 0;

	//! Extra pages pinned while gathering a spanning frame
	vector<BufferHandle> extra_handles;

	lz4_page_id_t *page_ids;
	lz4_page_offset_t *page_offsets;
	lz4_uncompressed_size_t *uncompressed_sizes;
	lz4_compressed_size_t *compressed_sizes;

	unique_ptr<LZ4VectorScanState> current_vector;

	idx_t segment_count;
	idx_t scanned_count = 0;
	ColumnSegment &segment;
};

unique_ptr<SegmentScanState> LZ4Storage::StringInitScan(const QueryContext &context, ColumnSegment &segment) {
	auto result = make_uniq<LZ4ScanState>(segment);
	return std::move(result);
}

void LZ4Storage::StringScanPartial(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result,
                                   idx_t result_offset) {
	auto &scan_state = state.scan_state->template Cast<LZ4ScanState>();
	auto start = state.GetPositionInSegment();
	scan_state.ScanPartial(start, result, result_offset, scan_count);
}

void LZ4Storage::StringScan(ColumnSegment &segment, ColumnScanState &state, idx_t scan_count, Vector &result) {
	StringScanPartial(segment, state, scan_count, result, 0);
}

void LZ4Storage::StringSkip(ColumnSegment &segment, ColumnScanState &state, idx_t skip_count) {
	// Random access is supported: skipping is handled lazily inside ScanPartial via the internal offset.
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void LZ4Storage::StringFetchRow(ColumnSegment &segment, ColumnFetchState &state, row_t row_id, Vector &result,
                                idx_t result_idx) {
	LZ4ScanState scan_state(segment);
	scan_state.ScanPartial(UnsafeNumericCast<idx_t>(row_id), result, result_idx, 1);
}

//===--------------------------------------------------------------------===//
// Serialization & Cleanup
//===--------------------------------------------------------------------===//
unique_ptr<CompressedSegmentState> LZ4Storage::StringInitSegment(ColumnSegment &segment, block_id_t block_id,
                                                                 optional_ptr<ColumnSegmentState> segment_state) {
	auto result = make_uniq<UncompressedStringSegmentState>();
	if (segment_state) {
		auto &serialized_state = segment_state->Cast<SerializedStringSegmentState>();
		result->on_disk_blocks = std::move(serialized_state.blocks);
	}
	return std::move(result);
}

unique_ptr<ColumnSegmentState> LZ4Storage::SerializeState(ColumnSegment &segment) {
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	if (state.on_disk_blocks.empty()) {
		return nullptr;
	}
	return make_uniq<SerializedStringSegmentState>(state.on_disk_blocks);
}

unique_ptr<ColumnSegmentState> LZ4Storage::DeserializeState(Deserializer &deserializer) {
	auto result = make_uniq<SerializedStringSegmentState>();
	deserializer.ReadProperty(1, "overflow_blocks", result->blocks);
	return std::move(result);
}

void LZ4Storage::VisitBlockIds(const ColumnSegment &segment, BlockIdVisitor &visitor) {
	auto &state = segment.GetSegmentState()->Cast<UncompressedStringSegmentState>();
	for (auto &block_id : state.on_disk_blocks) {
		visitor.Visit(block_id);
	}
}

//===--------------------------------------------------------------------===//
// Get Function
//===--------------------------------------------------------------------===//
CompressionFunction LZ4Fun::GetFunction(PhysicalType data_type) {
	D_ASSERT(data_type == PhysicalType::VARCHAR);
	auto lz4 = CompressionFunction(
	    CompressionType::COMPRESSION_LZ4, data_type, LZ4Storage::StringInitAnalyze, LZ4Storage::StringAnalyze,
	    LZ4Storage::StringFinalAnalyze, LZ4Storage::InitCompression, LZ4Storage::Compress,
	    LZ4Storage::FinalizeCompress, LZ4Storage::StringInitScan, LZ4Storage::StringScan,
	    LZ4Storage::StringScanPartial, LZ4Storage::StringFetchRow, LZ4Storage::StringSkip);
	lz4.init_segment = LZ4Storage::StringInitSegment;
	lz4.serialize_state = LZ4Storage::SerializeState;
	lz4.deserialize_state = LZ4Storage::DeserializeState;
	lz4.visit_block_ids = LZ4Storage::VisitBlockIds;
	return lz4;
}

bool LZ4Fun::TypeIsSupported(PhysicalType type) {
	return type == PhysicalType::VARCHAR;
}

} // namespace duckdb
