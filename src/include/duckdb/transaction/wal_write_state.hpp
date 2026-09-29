//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/wal_write_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/undo_buffer.hpp"
#include "duckdb/common/vector_size.hpp"

namespace duckdb {
class CatalogEntry;
class DataChunk;
class DuckTableEntry;
class DuckTransaction;
class WriteAheadLog;
class ClientContext;

struct DeleteInfo;
struct UpdateInfo;

class WALWriteState {
public:
	WALWriteState(DuckTransaction &transaction, optional_ptr<WriteAheadLog> log,
	              optional_ptr<StorageCommitState> commit_state, optional_ptr<WriteAheadLog> catalog_log);

public:
	void CommitEntry(UndoFlags type, data_ptr_t data);

private:
	void SwitchTable(DuckTableEntry &table_entry, UndoFlags new_op);

	void WriteCatalogEntry(CatalogEntry &entry, data_ptr_t extra_data);
	void WriteCatalogEntry(WriteAheadLog &target, CatalogEntry &entry, const AlterInfo *alter_info);
	void WriteDelete(DeleteInfo &info);
	void WriteUpdate(UpdateInfo &info);
	WriteAheadLog &Log();

private:
	DuckTransaction &transaction;
	optional_ptr<WriteAheadLog> log;
	optional_ptr<StorageCommitState> commit_state;
	optional_ptr<WriteAheadLog> catalog_log;
	bool catalog_selected = false;

	optional_ptr<DuckTableEntry> current_table_entry;

	unique_ptr<DataChunk> delete_chunk;
	unique_ptr<DataChunk> update_chunk;
};

} // namespace duckdb
