#include "duckdb/execution/index/unbound_index.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/index_storage_info.hpp"
#include "duckdb/storage/table_io_manager.hpp"

namespace duckdb {

UnboundIndex::UnboundIndex(unique_ptr<CreateInfo> create_info, IndexStorageInfo storage_info_p,
                           TableIOManager &table_io_manager, AttachedDatabase &db)
    : Index(create_info->Cast<CreateIndexInfo>().column_ids, table_io_manager, db), create_info(std::move(create_info)),
      storage_info(std::move(storage_info_p)) {
	// Memory safety check.
	for (idx_t info_idx = 0; info_idx < storage_info.allocator_infos.size(); info_idx++) {
		auto &info = storage_info.allocator_infos[info_idx];
		for (idx_t buffer_idx = 0; buffer_idx < info.buffer_ids.size(); buffer_idx++) {
			if (info.buffer_ids[buffer_idx] > idx_t(MAX_ROW_ID)) {
				throw InternalException("found invalid buffer ID in UnboundIndex constructor");
			}
		}
	}
}

void UnboundIndex::RemapColumnIds(const vector<column_t> &new_column_ids) {
	D_ASSERT(new_column_ids.size() == column_ids.size());
	for (auto &mapped : mapped_column_ids) {
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (column_ids[i] == mapped.GetPrimaryIndex()) {
				mapped.SetIndex(new_column_ids[i]);
				break;
			}
		}
	}
	create_info->Cast<CreateIndexInfo>().column_ids = new_column_ids;
	Index::RemapColumnIds(new_column_ids);
}

void UnboundIndex::ResetStorage() {
	auto &block_manager = table_io_manager.GetIndexBlockManager();
	for (auto &info : storage_info.allocator_infos) {
		for (auto &block : info.block_pointers) {
			if (block.IsValid()) {
				block_manager.MarkBlockAsModified(block.block_id);
			}
		}
	}
}

void UnboundIndex::BufferChunk(DataChunk &index_column_chunk, Vector &row_ids,
                               const vector<StorageIndex> &mapped_column_ids_p, const BufferedIndexReplay replay_type) {
	D_ASSERT(!column_ids.empty());
	if (mapped_column_ids.empty()) {
		vector<column_t> own_columns(column_id_set.begin(), column_id_set.end());
		std::sort(own_columns.begin(), own_columns.end());
		for (auto column : own_columns) {
			mapped_column_ids.emplace_back(column);
		}
	}

	vector<idx_t> sources;
	vector<LogicalType> types;
	for (auto &mapped : mapped_column_ids) {
		auto source =
		    std::find_if(mapped_column_ids_p.begin(), mapped_column_ids_p.end(), [&](const StorageIndex &column) {
			    return column.GetPrimaryIndex() == mapped.GetPrimaryIndex();
		    });
		if (source == mapped_column_ids_p.end()) {
			throw InternalException("Buffered index chunk does not carry indexed column " +
			                        std::to_string(mapped.GetPrimaryIndex()));
		}
		sources.push_back(NumericCast<idx_t>(source - mapped_column_ids_p.begin()));
		types.push_back(index_column_chunk.data[sources.back()].GetType());
	}
	types.push_back(LogicalType::ROW_TYPE);

	auto &allocator = Allocator::Get(db);

	DataChunk combined_chunk;
	combined_chunk.InitializeEmpty(types);
	for (idx_t i = 0; i < sources.size(); i++) {
		combined_chunk.data[i].Reference(index_column_chunk.data[sources[i]]);
	}
	combined_chunk.data.back().Reference(row_ids);

	auto &buffer = buffered_replays.GetBuffer(replay_type);
	if (buffer == nullptr) {
		buffer = make_uniq<ColumnDataCollection>(allocator, types);
	}
	// The starting index of the buffer range is the size of the buffer.
	const idx_t start = buffer->Count();
	const idx_t end = start + combined_chunk.size();
	auto &ranges = buffered_replays.ranges;

	if (ranges.empty() || ranges.back().type != replay_type) {
		// If there are no buffered ranges, or the replay types don't match, append a new range.
		ranges.emplace_back(replay_type, start, end);
		buffer->Append(combined_chunk);
		return;
	}
	// Otherwise merge the range with the previous one.
	ranges.back().end = end;
	buffer->Append(combined_chunk);
}

} // namespace duckdb
