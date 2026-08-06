//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/checkpoint_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/partial_block_manager.hpp"

namespace duckdb {

// Forward declaration.
class AttachedDatabase;
class BlockManager;
class CatalogEntry;
struct CatalogTransaction;
class ClientContext;
class ColumnSegment;
class DatabaseInstance;
class Deserializer;
class Connection;
class DuckTransaction;
class DuckTransactionManager;
class IndexCatalogEntry;

class MetadataManager;
class MetadataReader;
class SchemaCatalogEntry;
class SequenceCatalogEntry;
class Serializer;
class ScalarMacroCatalogEntry;
class TableMacroCatalogEntry;
class TableCatalogEntry;
class TriggerCatalogEntry;
class ViewCatalogEntry;
class TableDataWriter;
class TypeCatalogEntry;
struct BoundCreateTableInfo;
struct CheckpointOptions;

//! Wrapper to manage the lifetime of a checkpoint connection and transaction.
class ActiveCheckpointWrapper {
public:
	//! Creates a connection if we have a context.
	//! If there is no context, we are on shutdown and a checkpoint connection/transaction is not created.
	ActiveCheckpointWrapper(optional_ptr<ClientContext> context, AttachedDatabase &db,
	                        DuckTransactionManager &transaction_manager);

	~ActiveCheckpointWrapper();

	//! Begin the transaction within the newly created connection.
	void GetCheckpointTransaction(CheckpointOptions &options);
	void Commit();
	bool HasCheckpointContext() const;

private:
	AttachedDatabase &db;
	DuckTransactionManager &transaction_manager;
	unique_ptr<Connection> checkpoint_connection;
	optional_ptr<ClientContext> checkpoint_context;
	optional_ptr<DuckTransaction> checkpoint_transaction;
};

class CheckpointWriter {
public:
	explicit CheckpointWriter(AttachedDatabase &db) : db(db) {
	}
	virtual ~CheckpointWriter() {
	}

	//! The database
	AttachedDatabase &db;

	virtual void CreateCheckpoint() = 0;
	virtual MetadataManager &GetMetadataManager() = 0;
	virtual MetadataWriter &GetMetadataWriter() = 0;
	virtual unique_ptr<TableDataWriter> GetTableDataWriter(TableCatalogEntry &table) = 0;

protected:
	//! Whether the definition of `entry` is another catalog's, so this file records only its rows. It is the
	//! schema that says so: the entries of a schema a catalog of its own persists are that catalog's.
	bool DefinitionElsewhere(const CatalogEntry &entry) const;

	//! The schemas whose definitions a catalog of its own persists.
	reference_set_t<SchemaCatalogEntry> foreign_schemas;

	virtual void WriteEntry(CatalogEntry &entry, Serializer &serializer);
	virtual void WriteSchema(SchemaCatalogEntry &schema, Serializer &serializer);
	virtual void WriteTable(TableCatalogEntry &table, Serializer &serializer) = 0;
	//! The rows of a table whose definition this file does not hold: the catalog that owns the entry persists
	//! that itself, so the record is the entry's identifier plus its data.
	virtual void WriteDataManifest(TableCatalogEntry &table, Serializer &serializer);
	virtual void WriteView(ViewCatalogEntry &table, Serializer &serializer);
	virtual void WriteSequence(SequenceCatalogEntry &table, Serializer &serializer);
	virtual void WriteMacro(ScalarMacroCatalogEntry &table, Serializer &serializer);
	virtual void WriteTableMacro(TableMacroCatalogEntry &table, Serializer &serializer);
	virtual void WriteIndex(IndexCatalogEntry &index_catalog_entry, Serializer &serializer);
	virtual void WriteType(TypeCatalogEntry &type, Serializer &serializer);
	virtual void WriteTrigger(TriggerCatalogEntry &trigger, Serializer &serializer);
};

class CheckpointReader {
public:
	explicit CheckpointReader(Catalog &catalog) : catalog(catalog) {
	}
	virtual ~CheckpointReader() {
	}

protected:
	Catalog &catalog;

protected:
	virtual void LoadCheckpoint(CatalogTransaction transaction, MetadataReader &reader);
	virtual void ReadEntry(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadSchema(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadTable(CatalogTransaction transaction, Deserializer &deserializer);
	//! Reads the rows of `catalog_id`'s table and creates it from the definition the host catalog holds.
	virtual void ReadDataManifest(CatalogTransaction transaction, Deserializer &deserializer, idx_t catalog_id);
	//! Reads past a table's rows without attaching them, for a table the host catalog no longer has.
	void SkipTableData(Deserializer &deserializer);
	void CreateIndexEntry(CatalogTransaction transaction, unique_ptr<CreateInfo> create_info,
	                      BlockPointer root_block_pointer);
	virtual void ReadView(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadSequence(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadMacro(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadTableMacro(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadIndex(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadType(CatalogTransaction transaction, Deserializer &deserializer);
	virtual void ReadTrigger(CatalogTransaction transaction, Deserializer &deserializer);

	virtual void ReadTableData(CatalogTransaction transaction, Deserializer &deserializer,
	                           BoundCreateTableInfo &bound_info);
};

class SingleFileCheckpointReader final : public CheckpointReader {
public:
	explicit SingleFileCheckpointReader(SingleFileStorageManager &storage)
	    : CheckpointReader(Catalog::GetCatalog(storage.GetAttached())), storage(storage) {
	}

	void LoadFromStorage();
	MetadataManager &GetMetadataManager();

	//! The database
	SingleFileStorageManager &storage;
};

class SingleFileCheckpointWriter final : public CheckpointWriter {
	friend class SingleFileRowGroupWriter;
	friend class SingleFileTableDataWriter;

public:
	SingleFileCheckpointWriter(QueryContext context, AttachedDatabase &db, BlockManager &block_manager,
	                           CheckpointOptions options);

	void CreateCheckpoint() override;

	MetadataWriter &GetMetadataWriter() override;
	MetadataManager &GetMetadataManager() override;
	unique_ptr<TableDataWriter> GetTableDataWriter(TableCatalogEntry &table) override;

	BlockManager &GetBlockManager();
	CheckpointOptions GetCheckpointOptions() const {
		return options;
	}
	optional_ptr<ClientContext> GetClientContext() const {
		return context;
	}

public:
	void WriteTable(TableCatalogEntry &table, Serializer &serializer) override;
	void WriteDataManifest(TableCatalogEntry &table, Serializer &serializer) override;

private:
	optional_ptr<ClientContext> context;
	//! The metadata writer is responsible for writing schema information
	unique_ptr<MetadataWriter> metadata_writer;
	//! The table data writer is responsible for writing the DataPointers used by the table chunks
	unique_ptr<MetadataWriter> table_metadata_writer;
	//! Because this is single-file storage, we can share partial blocks across
	//! an entire checkpoint.
	PartialBlockManager partial_block_manager;
	//! Checkpoint type
	CheckpointOptions options;
	//! Block usage count for verification purposes
	unordered_map<block_id_t, idx_t> verify_block_usage_count;
};

} // namespace duckdb
