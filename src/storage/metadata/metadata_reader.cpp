#include "duckdb/storage/metadata/metadata_reader.hpp"

namespace duckdb {

MetadataReader::MetadataReader(MetadataManager &manager, MetaBlockPointer pointer,
                               optional_ptr<vector<MetaBlockPointer>> read_pointers_p, BlockReaderType type)
    : manager(manager), type(type), next_pointer(pointer), has_next_block(true), read_pointers(read_pointers_p),
      index(0), next_offset(pointer.offset) {
}

MetadataReader::MetadataReader(MetadataManager &manager, BlockPointer pointer)
    : MetadataReader(manager, MetadataManager::FromBlockPointer(pointer, manager.GetMetadataBlockSize())) {
}

MetadataPointer MetadataReader::FromDiskPointer(MetaBlockPointer pointer) {
	if (type == BlockReaderType::EXISTING_BLOCKS) {
		return manager.FromDiskPointer(pointer);
	} else {
		return manager.RegisterDiskPointer(pointer);
	}
}

MetadataReader::~MetadataReader() {
}

void MetadataReader::Underflow(QueryContext context, data_ptr_t buffer, idx_t read_size) {
	while (read_size > ReadWindowRoom()) {
		// cannot read entire entry from block
		// first read what we can from this block
		idx_t to_read = ReadWindowRoom();
		if (to_read > 0) {
			memcpy(buffer, read_pos, to_read);
			read_size -= to_read;
			buffer += to_read;
			read_pos += to_read;
		}
		// then move to the next block
		ReadNextBlock(context);
	}
	// we have enough left in this block to read from the buffer
	memcpy(buffer, read_pos, read_size);
	read_pos += read_size;
}

MetaBlockPointer MetadataReader::GetMetaBlockPointer() {
	if (!block.handle.IsValid()) {
		throw InternalException("GetMetaBlockPointer called but there is no active pointer");
	}
	return manager.GetDiskPointer(block.pointer, UnsafeNumericCast<uint32_t>(read_pos - BasePtr()));
}

void MetadataReader::ReadNextBlock() {
	ReadNextBlock(QueryContext());
}

void MetadataReader::ReadNextBlock(QueryContext context) {
	if (!has_next_block) {
		throw IOException("No more data remaining in MetadataReader");
	}
	if (read_pointers) {
		read_pointers->push_back(next_pointer);
	}
	auto next_disk_pointer = FromDiskPointer(next_pointer);
	block = manager.Pin(context, next_disk_pointer);
	index = next_disk_pointer.index;

	idx_t next_block = Load<idx_t>(BasePtr());
	if (next_block == idx_t(-1)) {
		has_next_block = false;
	} else {
		next_pointer = MetaBlockPointer(next_block, 0);
	}
	if (next_offset < sizeof(block_id_t)) {
		next_offset = sizeof(block_id_t);
	}
	if (next_offset > GetMetadataManager().GetMetadataBlockSize()) {
		throw InternalException("next_offset cannot be bigger than block size");
	}
	SetReadWindow(BasePtr() + next_offset, GetMetadataManager().GetMetadataBlockSize() - next_offset);
	next_offset = sizeof(block_id_t);
}

data_ptr_t MetadataReader::BasePtr() {
	return block.handle.GetDataMutable() + index * GetMetadataManager().GetMetadataBlockSize();
}

} // namespace duckdb
