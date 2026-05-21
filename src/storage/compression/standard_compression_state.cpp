#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"

#include <cstring>

namespace duckdb {

CompressionState::CompressionState(ColumnDataCheckpointData &checkpoint_data_p, CompressionType compression_type)
    : checkpoint_data(checkpoint_data_p), function(checkpoint_data.GetCompressionFunction(compression_type)),
      block_manager(checkpoint_data.GetBlockManager()), info(block_manager) {
}

unique_ptr<ColumnSegment> CompressionState::CreateNewSegment() {
	return ColumnSegment::CreateTransientSegment(checkpoint_data.GetDatabase(), function, checkpoint_data.GetType(),
	                                             info.GetBlockSize(), info.GetBlockManager());
}

const LogicalType &CompressionState::GetType() {
	return checkpoint_data.GetType();
}

StandardCompressionState::~StandardCompressionState() {
}

void StandardCompressionState::CreateAndPinNewSegment() {
	auto compressed_segment = CreateNewSegment();
	current_segment = std::move(compressed_segment);

	auto &buffer_manager = BufferManager::GetBufferManager(current_segment->GetDatabase());
	handle = buffer_manager.Pin(current_segment->GetBlockHandle());
	// TODO(serenedb): remove this memset and fix properly. Compressors write only
	// to their packed offsets and leave alignment padding untouched; duckdb itself
	// is fine because it only reads section data via header offsets. The iresearch
	// columnstore writer captures `segment_size` bytes verbatim into its own file
	// and so leaks uninitialized padding bytes (msan-confirmed). Real fix should
	// either zero only the padding regions inside each compressor's Finalize, or
	// have the iresearch capture path skip padding. This zero-fill is a
	// per-segment-alloc cost on the hot path and must be removed.
	std::memset(handle.GetDataMutable(), 0, info.GetBlockSize());
}

void StandardCompressionState::FlushCurrentSegment(idx_t segment_size) {
	checkpoint_data.FlushSegment(std::move(current_segment), std::move(handle), segment_size);
}

} // namespace duckdb
