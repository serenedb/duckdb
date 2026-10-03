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
#include "duckdb/parser/parsed_data/alter_info.hpp"

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
	              optional_ptr<StorageCommitState> commit_state, optional_ptr<vector<CatalogRunEntry>> catalog_run);

public:
	void CommitEntry(UndoFlags type, data_ptr_t data);
	static void WriteCatalogRun(WriteAheadLog &catalog_log, idx_t catalog_oid, const vector<CatalogRunEntry> &run);

private:
	void SwitchTable(DuckTableEntry &table_entry, UndoFlags new_op);

	void WriteCatalogEntry(CatalogEntry &entry, data_ptr_t extra_data);
	static void WriteCatalogEntry(WriteAheadLog &target, CatalogEntry &entry, const AlterInfo *alter_info,
	                              bool with_index_storage);
	void WriteDelete(DeleteInfo &info);
	void WriteUpdate(UpdateInfo &info);
	WriteAheadLog &Log();

private:
	DuckTransaction &transaction;
	optional_ptr<WriteAheadLog> log;
	optional_ptr<StorageCommitState> commit_state;
	optional_ptr<vector<CatalogRunEntry>> catalog_run;

	optional_ptr<DuckTableEntry> current_table_entry;

	unique_ptr<DataChunk> delete_chunk;
	unique_ptr<DataChunk> update_chunk;
};

} // namespace duckdb
