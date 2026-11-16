//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/metadata/metadata_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/common/serializer/write_stream.hpp"

namespace duckdb {

class MetadataWriter : public WriteStream {
public:
	explicit MetadataWriter(MetadataManager &manager,
	                        optional_ptr<vector<MetaBlockPointer>> written_pointers = nullptr);
	MetadataWriter(const MetadataWriter &) = delete;
	MetadataWriter &operator=(const MetadataWriter &) = delete;
	~MetadataWriter() override;

public:
	void Flush();

	BlockPointer GetBlockPointer();
	MetaBlockPointer GetMetaBlockPointer();
	MetadataManager &GetManager() {
		return manager;
	}
	void SetWrittenPointers(optional_ptr<vector<MetaBlockPointer>> written_pointers);

protected:
	virtual MetadataHandle NextHandle();
	void Overflow(const_data_ptr_t buffer, idx_t write_size) override;

private:
	void NextBlock();

private:
	MetadataManager &manager;
	MetadataHandle block;
	MetadataPointer current_pointer;
	optional_ptr<vector<MetaBlockPointer>> written_pointers;
};

} // namespace duckdb
