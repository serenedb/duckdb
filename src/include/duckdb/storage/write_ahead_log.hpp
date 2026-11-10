//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/write_ahead_log.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/common/enums/wal_type.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/table/table_log_storage.hpp"

#include <absl/functional/function_ref.h>

namespace roaring::api {
struct roaring_bitmap_s;
typedef struct roaring_bitmap_s roaring_bitmap_t;
} // namespace roaring::api

namespace duckdb {

struct AlterInfo;

class AttachedDatabase;
class Catalog;
class DatabaseInstance;
class SchemaCatalogEntry;
class SequenceCatalogEntry;
class ScalarMacroCatalogEntry;
class ViewCatalogEntry;
class TriggerCatalogEntry;
class TypeCatalogEntry;
class TableCatalogEntry;
class Transaction;
class TransactionManager;
class WriteAheadLogDeserializer;
struct PersistentCollectionData;

enum class WALInitState { NO_WAL, UNINITIALIZED, UNINITIALIZED_REQUIRES_TRUNCATE, INITIALIZED };

struct WALRowPositionGroup {
	uint64_t segment = 0;
	const ::roaring::api::roaring_bitmap_t *docs = nullptr;
};

struct WALRowPositionMap {
	uint64_t target = 0;
	uint32_t base = 0;
	uint32_t rank_begin = 0;
	uint32_t rank_end = 0;
	const ::roaring::api::roaring_bitmap_t *mask = nullptr;
};

class WALEntryBuffer {
public:
	WALEntryBuffer(AttachedDatabase &database, idx_t capacity);

	void WriteInsert(DataChunk &chunk);
	void WriteTruncateTable();
	void WriteAdoptSegments(vector<string> segments);
	void WriteDeleteRowPositions(idx_t count, absl::FunctionRef<WALRowPositionGroup(idx_t)> groups);
	void WriteRowPositionMaps(uint64_t source, idx_t count, absl::FunctionRef<WALRowPositionMap(idx_t)> maps);

private:
	friend class WriteAheadLog;

	AttachedDatabase &database;
	MemoryStream stream;
	idx_t entry_count = 0;
};

//! The WriteAheadLog (WAL) is a log that is used to provide durability. Prior
//! to committing a transaction it writes the changes the transaction made to
//! the database to the log, which can then be replayed upon startup in case the
//! server crashes or is shut down.
class WriteAheadLog {
public:
	//! Initialize the WAL in the specified directory
	explicit WriteAheadLog(StorageManager &storage_manager, const string &wal_path, idx_t wal_size = 0ULL,
	                       WALInitState state = WALInitState::NO_WAL,
	                       optional_idx checkpoint_iteration = optional_idx());
	virtual ~WriteAheadLog();

public:
	//! Replay and initialize the WAL, QueryContext is passed for metric collection purposes only!!
	static unique_ptr<WriteAheadLog> Replay(QueryContext context, StorageManager &storage_manager,
	                                        const string &wal_path);
	static void ReplayCatalog(QueryContext context, StorageManager &storage_manager, const string &log_path,
	                          idx_t catalog_oid);

	AttachedDatabase &GetDatabase();
	StorageManager &GetStorageManager();

	const string &GetPath() const {
		return wal_path;
	}
	idx_t GetCheckpointIteration();
	//! Gets the total bytes written to the WAL since startup
	idx_t GetTotalWritten() const;
	idx_t GetFlushedOffset() const;
	idx_t GetDurableOffset() const;

	//! A WAL is initialized, if a writer to a file exists.
	bool Initialized() const;
	//! Initializes the file of the WAL by creating the file writer.
	BufferedFileWriter &Initialize();

	//! Write the WAL header.
	void WriteHeader();

	virtual void WriteCreateTable(const TableCatalogEntry &entry);
	void WriteDropTable(const TableCatalogEntry &entry);

	void WriteCreateSchema(const SchemaCatalogEntry &entry);
	void WriteDropSchema(const SchemaCatalogEntry &entry);

	void WriteCreateView(const ViewCatalogEntry &entry);
	void WriteDropView(const ViewCatalogEntry &entry);

	void WriteCreateSequence(const SequenceCatalogEntry &entry);
	void WriteDropSequence(const SequenceCatalogEntry &entry);
	void WriteSequenceValue(SequenceValue val);

	void WriteCreateMacro(const ScalarMacroCatalogEntry &entry);
	void WriteDropMacro(const ScalarMacroCatalogEntry &entry);

	void WriteCreateTableMacro(const TableMacroCatalogEntry &entry);
	void WriteDropTableMacro(const TableMacroCatalogEntry &entry);

	void WriteCreateIndex(const IndexCatalogEntry &entry, bool with_index_storage = true);
	void WriteDropIndex(const IndexCatalogEntry &entry);

	void WriteCreateType(const TypeCatalogEntry &entry);
	void WriteDropType(const TypeCatalogEntry &entry);

	void WriteCreateTrigger(const TriggerCatalogEntry &entry);
	void WriteDropTrigger(const TriggerCatalogEntry &entry);

	void WriteCreateTokenizer(const StandardEntry &entry);
	void WriteDropTokenizer(const StandardEntry &entry);

	void WriteCreateJob(const StandardEntry &entry);
	void WriteDropJob(const StandardEntry &entry);

	void WriteCreateRole(const InCatalogEntry &entry);
	void WriteDropRole(const InCatalogEntry &entry);

	void WriteCreateDatabase(const InCatalogEntry &entry);
	void WriteDropDatabase(const InCatalogEntry &entry);

	void WriteCreateForeignServer(const InCatalogEntry &entry);
	void WriteDropForeignServer(const InCatalogEntry &entry);

	void WriteCreateTablespace(const InCatalogEntry &entry);
	void WriteDropTablespace(const InCatalogEntry &entry);

	void WriteCreateSubscription(const InCatalogEntry &entry);
	void WriteDropSubscription(const InCatalogEntry &entry);
	void WriteCreateReplicationOrigin(const InCatalogEntry &entry);
	void WriteDropReplicationOrigin(const InCatalogEntry &entry);
	void WriteReplicationLsn(const InCatalogEntry &entry, uint64_t remote_lsn, bool assign, idx_t relation = 0);
	//! Sets the table used for subsequent insert/delete/update commands. The qualified name holds the (possibly
	//! nested) schema path of the table followed by the table name.
	void WriteSetTable(const QualifiedName &table, idx_t table_oid);
	void WriteSetTable(const TableCatalogEntry &table);
	void WriteUseCatalog(idx_t catalog_oid);
	void WriteCommitPrepared(const hugeint_t &txid, const vector<pair<idx_t, idx_t>> &participants);

	void WriteAlter(CatalogEntry &entry, const AlterInfo &info, bool with_index_storage = true);

	void WriteInsert(DataChunk &chunk);
	void WriteRowGroupData(const PersistentCollectionData &data);
	void WriteDelete(DataChunk &chunk);
	void WriteEntries(const WALEntryBuffer &entries);
	//! Write a single (sub-) column update to the WAL. Chunk must be a pair of (COL, ROW_ID).
	//! The column_path vector is a *path* towards a column within the table
	//! i.e. if we have a table with a single column S STRUCT(A INT, B INT)
	//! and we update the validity mask of "S.B"
	//! the column path is:
	//! 0 (first column of table)
	//! -> 1 (second subcolumn of struct)
	//! -> 0 (first subcolumn of INT)
	void WriteUpdate(DataChunk &chunk, const vector<column_t> &column_path);

	//! Truncate the WAL to a previous size, and clear anything currently set in the writer.
	//! Used during RevertCommit.
	void Truncate(idx_t size);
	//! Write a WAL_FLUSH marker and sync the WAL up to it
	void Flush();
	//! Write a WAL_FLUSH marker and push the buffer to the OS without syncing it. Returns the
	//! offset covering the marker, to be passed to SyncUpTo. Caller must hold the WAL lock
	idx_t FlushMarker(optional_ptr<const hugeint_t> prepared_txid = nullptr);
	//! Block until the WAL is durable up to the given offset
	void SyncUpTo(idx_t offset);
	void SyncUpTo(idx_t offset, absl::FunctionRef<void(idx_t)> on_synced);
	//! Increment the WAL entry count, which is used for the auto-checkpoint threshold.
	void IncrementWALEntriesCount();
	void WriteCheckpoint(MetaBlockPointer meta_block);

protected:
	StorageManager &storage_manager;
	mutex wal_lock;
	unique_ptr<BufferedFileWriter> writer;
	string wal_path;
	atomic<WALInitState> init_state;
	optional_idx checkpoint_iteration;

	//! Sync offsets are logical (BufferedFileWriter::GetTotalWritten), not file positions: a
	//! truncation rewinds the file, so a file position can be reused but a logical one cannot
	//! The WAL is durable up to this logical offset
	atomic<idx_t> durable_offset {0};
	//! The highest logical offset for which a sync has been requested
	atomic<idx_t> requested_sync_offset {0};
	atomic<idx_t> syncs_in_flight {0};
	atomic<idx_t> sync_lanes {1};
	atomic<idx_t> syncing_offset {0};
	atomic<int32_t> lane_epoch {0};
	atomic<int32_t> durable_epoch {0};
	//! Set when a sync has failed; every further sync of this WAL fails
	atomic<bool> sync_failed {false};
	mutex sync_failure_lock;
	string sync_failure;
};

struct WALTailRead {
	//! Where the next read must resume: the start of the first batch this one could not confirm
	//! committed, which is also where a torn or still-deciding tail begins.
	idx_t boundary = 0;
	idx_t wal_version = 0;
	idx_t entries = 0;
	//! Batches whose entries were kept.
	idx_t committed_batches = 0;
	//! True when the read stopped on a batch whose transaction had not decided yet.
	bool stopped_undecided = false;
	string error;
};

//! Reads the committed part of [start_offset, end_offset) out of a WAL that is still being written,
//! handing each entry to the consumer `resolver` names for its table. Entries of a batch are
//! reported before the batch's fate is known, so the resolver is told at every terminator whether
//! to keep them.
//! Takes the file already open, because which file a WAL path names is only fixed while the commit
//! lock is held: a caller resolves and opens its files under that lock and reads them after
//! releasing it, the handle keeping the file it was opened on through any later rename or unlink.
WALTailRead ReadWALTail(AttachedDatabase &db, ClientContext &context, unique_ptr<FileHandle> handle, idx_t start_offset,
                        idx_t end_offset, TableLogStorageResolver &resolver);

} // namespace duckdb
