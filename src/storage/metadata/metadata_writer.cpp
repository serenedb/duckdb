#include "duckdb/storage/metadata/metadata_writer.hpp"

namespace duckdb {

MetadataWriter::MetadataWriter(MetadataManager &manager, optional_ptr<vector<MetaBlockPointer>> written_pointers_p)
    : manager(manager), written_pointers(written_pointers_p) {
	D_ASSERT(!written_pointers || written_pointers->empty());
}

MetadataWriter::~MetadataWriter() {
	// If there's an exception during checkpoint, this can get destroyed without
	// flushing the data...which is fine, because none of the unwritten data
	// will be referenced.
	//
	// Otherwise, we should have explicitly flushed (and thereby nulled the block).
	D_ASSERT(!block.handle.IsValid() || Exception::UncaughtException());
}

BlockPointer MetadataWriter::GetBlockPointer() {
	return MetadataManager::ToBlockPointer(GetMetaBlockPointer(), GetManager().GetMetadataBlockSize());
}

MetaBlockPointer MetadataWriter::GetMetaBlockPointer() {
	if (write_pos == write_end) {
		// at the end of the block - fetch the next block
		NextBlock();
		D_ASSERT(write_begin != write_end);
	}
	return manager.GetDiskPointer(block.pointer, UnsafeNumericCast<uint32_t>(WindowSize()));
}

void MetadataWriter::SetWrittenPointers(optional_ptr<vector<MetaBlockPointer>> written_pointers_p) {
	written_pointers = written_pointers_p;
	if (written_pointers && write_pos != write_end) {
		written_pointers->push_back(manager.GetDiskPointer(current_pointer));
	}
}

MetadataHandle MetadataWriter::NextHandle() {
	return manager.AllocateHandle();
}

void MetadataWriter::NextBlock() {
	// now we need to get a new block id
	auto new_handle = NextHandle();

	// write the block id of the new block to the start of the current block
	if (write_begin != write_end) {
		auto disk_block = manager.GetDiskPointer(new_handle.pointer);
		Store<idx_t>(disk_block.block_pointer, write_begin);
	}
	// now update the block id of the block
	write_offset += WindowSize();
	block = std::move(new_handle);
	current_pointer = block.pointer;
	const auto block_size = GetManager().GetMetadataBlockSize();
	SetWindow(block.handle.GetDataMutable() + current_pointer.index * block_size, block_size);
	Write<idx_t>(static_cast<idx_t>(-1));
	if (written_pointers) {
		written_pointers->push_back(manager.GetDiskPointer(current_pointer));
	}
}

void MetadataWriter::Overflow(const_data_ptr_t buffer, idx_t write_size) {
	while (write_size > WindowRoom()) {
		// we need to make a new block
		// first copy what we can
		const auto copy_amount = WindowRoom();
		if (copy_amount > 0) {
			memcpy(write_pos, buffer, copy_amount);
			buffer += copy_amount;
			write_pos += copy_amount;
			write_size -= copy_amount;
		}
		// move forward to the next block
		NextBlock();
	}
	memcpy(write_pos, buffer, write_size);
	write_pos += write_size;
}

void MetadataWriter::Flush() {
	if (write_pos != write_end) {
		// clear remaining bytes of block (if any)
		memset(write_pos, 0, WindowRoom());
	}
	block.handle.Destroy();
}

} // namespace duckdb
