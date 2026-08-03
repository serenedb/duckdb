#include "duckdb/storage/checkpoint_manager.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_trigger_info.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/enums/checkpoint_abort.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/checkpoint/table_data_reader.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/host_table_definition.hpp"

namespace duckdb {

ActiveCheckpointWrapper::ActiveCheckpointWrapper(optional_ptr<ClientContext> context, AttachedDatabase &db_p,
                                                 DuckTransactionManager &transaction_manager_p)
    : db(db_p), transaction_manager(transaction_manager_p) {
	if (!context) {
		return;
	}
	checkpoint_connection = make_uniq<Connection>(db.GetDatabase());
	checkpoint_context = checkpoint_connection->context.get();
}

ActiveCheckpointWrapper::~ActiveCheckpointWrapper() {
	// This happens on failure before we commit the transaction.
	if (checkpoint_transaction) {
		transaction_manager.RollbackTransaction(*checkpoint_transaction);
		checkpoint_transaction = nullptr;
	}
	if (checkpoint_context) {
		checkpoint_context->transaction.ClearTransaction();
	}
}

void ActiveCheckpointWrapper::GetCheckpointTransaction(CheckpointOptions &options) {
	checkpoint_context->transaction.BeginTransaction();
	checkpoint_context->transaction.SetReadOnly();
	auto &transaction = DuckTransaction::Get(*checkpoint_context, db);
	transaction.SetIsCheckpointTransaction();
	transaction_manager.RefreshCheckpointSnapshot(transaction);
	checkpoint_transaction = &transaction;
	options.transaction_id = transaction.start_time;
	transaction_manager.SetActiveCheckpoint(transaction.start_time);
}

void ActiveCheckpointWrapper::Commit() {
	transaction_manager.ResetActiveCheckpoint();
	if (!checkpoint_transaction) {
		return;
	}
	checkpoint_context->transaction.Commit();
	checkpoint_transaction = nullptr;
}

bool ActiveCheckpointWrapper::HasCheckpointContext() const {
	return checkpoint_context;
}

void ReorderTableEntries(catalog_entry_vector_t &tables);

SingleFileCheckpointWriter::SingleFileCheckpointWriter(QueryContext context, AttachedDatabase &db,
                                                       BlockManager &block_manager, CheckpointOptions options_p)
    : CheckpointWriter(db), context(context.GetClientContext()),
      partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT), options(options_p) {
}

BlockManager &SingleFileCheckpointWriter::GetBlockManager() {
	auto &storage_manager = db.GetStorageManager().Cast<SingleFileStorageManager>();
	return *storage_manager.block_manager;
}

MetadataWriter &SingleFileCheckpointWriter::GetMetadataWriter() {
	return *metadata_writer;
}

MetadataManager &SingleFileCheckpointWriter::GetMetadataManager() {
	return GetBlockManager().GetMetadataManager();
}

unique_ptr<TableDataWriter> SingleFileCheckpointWriter::GetTableDataWriter(TableCatalogEntry &table) {
	return make_uniq<SingleFileTableDataWriter>(*this, table, *table_metadata_writer);
}

static catalog_entry_vector_t GetCatalogEntries(vector<reference<SchemaCatalogEntry>> &schemas) {
	catalog_entry_vector_t entries;
	for (auto &schema_p : schemas) {
		auto &schema = schema_p.get();
		entries.push_back(schema);
		schema.Scan(CatalogType::TYPE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			entries.push_back(entry);
		});

		schema.Scan(CatalogType::SEQUENCE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			entries.push_back(entry);
		});

		catalog_entry_vector_t tables;
		vector<reference<ViewCatalogEntry>> views;
		schema.Scan(CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type == CatalogType::TABLE_ENTRY) {
				tables.push_back(entry.Cast<TableCatalogEntry>());
			} else if (entry.type == CatalogType::VIEW_ENTRY) {
				views.push_back(entry.Cast<ViewCatalogEntry>());
			} else {
				throw NotImplementedException("Catalog type for entries");
			}
		});
		// Reorder tables because of foreign key constraint
		ReorderTableEntries(tables);
		for (auto &table : tables) {
			entries.push_back(table.get());
		}
		for (auto &view : views) {
			entries.push_back(view.get());
		}

		// Scan triggers from each table directly (triggers are nested under their table)
		for (auto &table_entry : tables) {
			auto &table = table_entry.get().Cast<TableCatalogEntry>();
			if (!table.IsDuckTable()) {
				continue;
			}
			auto &duck_table = table.Cast<DuckTableEntry>();
			duck_table.ScanTriggersNonTransactional([&](CatalogEntry &entry) {
				if (!entry.internal) {
					entries.push_back(entry);
				}
			});
		}

		schema.Scan(CatalogType::SCALAR_FUNCTION_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type == CatalogType::MACRO_ENTRY) {
				entries.push_back(entry);
			}
		});

		schema.Scan(CatalogType::TABLE_FUNCTION_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type == CatalogType::TABLE_MACRO_ENTRY) {
				entries.push_back(entry);
			}
		});

		schema.Scan(CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
			D_ASSERT(!entry.internal);
			entries.push_back(entry);
		});
	}
	return entries;
}

void SingleFileCheckpointWriter::CreateCheckpoint() {
	auto &storage_manager = db.GetStorageManager().Cast<SingleFileStorageManager>();
	if (storage_manager.InMemory()) {
		return;
	}
	if (ValidChecker::IsInvalidated(db.GetDatabase())) {
		// don't checkpoint invalidated databases
		return;
	}
	// assert that the checkpoint manager hasn't been used before
	D_ASSERT(!metadata_writer);

	auto &block_manager = GetBlockManager();
	auto &metadata_manager = GetMetadataManager();

	//! Set up the writers for the checkpoints
	metadata_writer = make_uniq<MetadataWriter>(metadata_manager);
	table_metadata_writer = make_uniq<MetadataWriter>(metadata_manager);

	// get the id of the first meta block
	auto meta_block = metadata_writer->GetMetaBlockPointer();

	// write a checkpoint flag to the WAL
	// in case a crash happens during the checkpoint, we know a checkpoint was instantiated
	// we write the root meta block of the planned checkpoint to the WAL
	// during recovery we use this:
	// * if the root meta block matches the checkpoint entry, we know the checkpoint was completed
	// * if the root meta block does not match the checkpoint entry, we know the checkpoint was not completed
	// if the checkpoint was completed we don't need to replay the WAL - otherwise we need to replay the WAL
	// we also know if a checkpoint was running that we need to check for the checkpoint WAL (`.checkpoint.wal`)
	// to replay any concurrent commits that have succeeded and ensure these are not lost
	auto &transaction_manager = db.GetTransactionManager().Cast<DuckTransactionManager>();

	// If there is a context (non shutdown path): this will create a new connection for the checkpoint, then in
	// WALStartCheckpoint we will create a transaction for the checkpoint.
	ActiveCheckpointWrapper active_checkpoint(context, db, transaction_manager);
	auto has_wal = storage_manager.WALStartCheckpoint(meta_block, options, active_checkpoint);

	catalog_entry_vector_t catalog_entries;

	auto checkpoint_sleep_ms = Settings::Get<DebugCheckpointSleepMsSetting>(db.GetDatabase());
	if (checkpoint_sleep_ms > 0) {
		ThreadUtil::SleepMs(checkpoint_sleep_ms);
	}

	vector<reference<SchemaCatalogEntry>> schemas;
	// The schemas whose definitions a catalog of its own persists. Their tables still keep their rows here, so
	// the checkpoint writes those as a manifest: an identifier and the data, no CreateInfo.
	vector<reference<SchemaCatalogEntry>> foreign_schemas;
	// we scan the set of committed schemas
	auto &catalog = Catalog::GetCatalog(db).Cast<DuckCatalog>();
	catalog.ScanSchemas(
	    [&](SchemaCatalogEntry &entry) { (entry.duck_managed ? schemas : foreign_schemas).push_back(entry); });

	D_ASSERT(catalog.IsDuckCatalog());

	catalog_entry_vector_t manifest_entries;
	unordered_map<idx_t, reference<CatalogEntry>> manifest_by_id;
	for (auto &schema : foreign_schemas) {
		schema.get().Scan(CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.type != CatalogType::TABLE_ENTRY) {
				return;
			}
			auto storage = entry.Cast<TableCatalogEntry>().TryGetStorage();
			if (!storage) {
				return;
			}
			auto catalog_id = storage->GetDataTableInfo()->GetCatalogId();
			if (catalog_id == 0) {
				return;
			}
			manifest_by_id.emplace(catalog_id, entry);
		});
	}

	catalog_entries = GetCatalogEntries(schemas);
	// A catalog that tracks dependencies elsewhere reports no manager; reordering is
	// what the manager contributes here, and entries with no tracked edges need none.
	auto dependency_manager = catalog.GetDependencyManager();
	if (dependency_manager) {
		dependency_manager->ReorderEntries(catalog_entries);
	}

	// The manifest records create their tables, so they inherit the foreign-key ordering the reorder above
	// established rather than the order the owning schemas happened to hand them out in. The definitions of those
	// tables, and of the indexes on them, are the owning catalog's and are rebuilt from there at load, so this
	// file writes no record for them -- they stay in `catalog_entries` because the delta merge below still has to
	// see them.
	auto qualified_name_of = [](const StandardEntry &entry, const Identifier &table) {
		return entry.schema.name.GetIdentifierName() + "." + table.GetIdentifierName();
	};
	unordered_set<string> manifest_tables;
	unordered_set<idx_t> definitions_elsewhere;
	manifest_entries.reserve(manifest_by_id.size());
	if (!manifest_by_id.empty()) {
		for (idx_t i = 0; i < catalog_entries.size(); i++) {
			auto &entry = catalog_entries[i].get();
			if (entry.type != CatalogType::TABLE_ENTRY) {
				continue;
			}
			auto storage = entry.Cast<TableCatalogEntry>().TryGetStorage();
			if (!storage) {
				continue;
			}
			auto owner = manifest_by_id.find(storage->GetDataTableInfo()->GetCatalogId());
			if (owner == manifest_by_id.end()) {
				continue;
			}
			manifest_entries.push_back(owner->second);
			manifest_tables.insert(qualified_name_of(entry.Cast<StandardEntry>(), entry.name));
			definitions_elsewhere.insert(i);
		}
		for (idx_t i = 0; i < catalog_entries.size(); i++) {
			auto &entry = catalog_entries[i].get();
			if (entry.type != CatalogType::INDEX_ENTRY) {
				continue;
			}
			auto &index = entry.Cast<IndexCatalogEntry>();
			if (manifest_tables.count(qualified_name_of(index, index.GetTableName())) != 0) {
				definitions_elsewhere.insert(i);
			}
		}
	}

	catalog_entry_vector_t written_entries;
	written_entries.reserve(catalog_entries.size() - definitions_elsewhere.size() + manifest_entries.size());
	for (idx_t i = 0; i < catalog_entries.size(); i++) {
		if (definitions_elsewhere.count(i) == 0) {
			written_entries.push_back(catalog_entries[i]);
		}
	}
	// The manifests come last: each one creates a table the entries before it may reference.
	for (auto &entry : manifest_entries) {
		written_entries.push_back(entry);
	}

	// write the actual data into the database

	// Create a serializer to write the checkpoint data
	// The serialized format is roughly:
	/*
	    {
	        schemas: [
	            {
	                schema: <schema_info>,
	                custom_types: [ { type: <type_info> }, ... ],
	                sequences: [ { sequence: <sequence_info> }, ... ],
	                tables: [ { table: <table_info> }, ... ],
	                views: [ { view: <view_info> }, ... ],
	                macros: [ { macro: <macro_info> }, ... ],
	                table_macros: [ { table_macro: <table_macro_info> }, ... ],
	                indexes: [ { index: <index_info>, root_offset <block_ptr> }, ... ]
	            }
	        ]
	    }
	 */
	BinarySerializer serializer(*metadata_writer, SerializationOptions(db));
	serializer.Begin();
	serializer.WriteList(100, "catalog_entries", written_entries.size(), [&](Serializer::List &list, idx_t i) {
		list.WriteObject([&](Serializer &obj) { WriteEntry(written_entries[i].get(), obj); });
	});
	serializer.End();

	metadata_writer->Flush();
	table_metadata_writer->Flush();

	auto debug_checkpoint_abort = Settings::Get<DebugCheckpointAbortSetting>(db.GetDatabase());
	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_HEADER) {
		throw FatalException("Checkpoint aborted before header write because of PRAGMA checkpoint_abort flag");
	}
	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_HEADER_NON_FATAL) {
		throw IOException("Checkpoint aborted before header write (non-fatal) because of PRAGMA checkpoint_abort flag");
	}

	// finally write the updated header
	DatabaseHeader header;
	header.meta_block = meta_block.block_pointer;
	header.block_alloc_size = block_manager.GetBlockAllocSize();
	header.vector_size = STANDARD_VECTOR_SIZE;
	// The WAL is truncated once this header lands, so the position its CATALOG_POSITION records carried has to be
	// folded in here or it is lost.
	header.catalog_position = storage_manager.GetCatalogPosition();
	block_manager.WriteHeader(context, header);

	auto debug_verify_blocks = Settings::Get<DebugVerifyBlocksSetting>(db.GetDatabase());
	if (debug_verify_blocks) {
		// extend verify_block_usage_count
		auto metadata_info = storage_manager.GetMetadataInfo();
		for (auto &info : metadata_info) {
			verify_block_usage_count[info.block_id]++;
		}
		for (auto &entry_ref : catalog_entries) {
			auto &entry = entry_ref.get();
			if (entry.type != CatalogType::TABLE_ENTRY) {
				continue;
			}
			auto &table = entry.Cast<DuckTableEntry>();
			auto &storage = table.GetStorage();
			auto segment_info = storage.GetColumnSegmentInfo(context);
			for (auto &segment : segment_info) {
				verify_block_usage_count[segment.block_id]++;
				if (StringUtil::Contains(segment.segment_info, "Overflow String Block Ids: ")) {
					auto overflow_blocks = StringUtil::Replace(segment.segment_info, "Overflow String Block Ids: ", "");
					auto splits = StringUtil::Split(overflow_blocks, ", ");
					for (auto &split : splits) {
						auto overflow_block_id = std::stoll(split);
						verify_block_usage_count[overflow_block_id]++;
					}
				}
			}
		}
		block_manager.VerifyBlocks(verify_block_usage_count);
	}

	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_TRUNCATE) {
		throw FatalException("Checkpoint aborted before truncate because of PRAGMA checkpoint_abort flag");
	}

	// truncate the file
	block_manager.Truncate();

	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_BEFORE_WAL_FINISH) {
		throw FatalException("Checkpoint aborted before truncate because of PRAGMA checkpoint_abort flag");
	}

	// truncate the WAL
	if (has_wal) {
		unique_lock<mutex> owned_wal_lock;
		optional_ptr<unique_lock<mutex>> wal_lock;
		if (!options.wal_lock) {
			// not holding the WAL lock yet - grab it
			owned_wal_lock = storage_manager.GetWALLock();
			wal_lock = owned_wal_lock;
		} else {
			// we already have the WAL lock - just refer to it
			wal_lock = options.wal_lock;
		}
		storage_manager.WALFinishCheckpoint(*wal_lock);
	}

	// for any indexes that were appended to while checkpointing, merge the delta back into the main index
	// FIXME: we only clean up appends made to tables that are part of this checkpoint
	// Currently, that is correct, since we don't allow creating tables DURING a checkpoint
	// In the future, we will allow this
	// When that happens, we should ensure the delta indexes are NOT used for tables created DURING a checkpoint
	// this is also not necessary - if we are not checkpointing a table, we are not checkpointing its indexes
	// ergo we don't need the delta indexes
	for (auto &entry_ref : catalog_entries) {
		auto &entry = entry_ref.get();
		if (entry.type != CatalogType::TABLE_ENTRY) {
			continue;
		}
		auto &table = entry.Cast<DuckTableEntry>();
		auto &storage = table.GetStorage();
		auto &table_info = storage.GetDataTableInfo();
		auto &index_list = table_info->GetIndexes();
		index_list.MergeCheckpointDeltas(options.transaction_id);
	}
	active_checkpoint.Commit();
}

void CheckpointReader::LoadCheckpoint(CatalogTransaction transaction, MetadataReader &reader) {
	BinaryDeserializer deserializer(reader);
	deserializer.Set<Catalog &>(catalog);
	deserializer.Begin();
	deserializer.ReadList(100, "catalog_entries", [&](Deserializer::List &list, idx_t i) {
		return list.ReadObject([&](Deserializer &obj) { ReadEntry(transaction, obj); });
	});
	deserializer.End();
	deserializer.Unset<Catalog>();
}

MetadataManager &SingleFileCheckpointReader::GetMetadataManager() {
	return storage.block_manager->GetMetadataManager();
}

void SingleFileCheckpointReader::LoadFromStorage() {
	auto &block_manager = *storage.block_manager;
	auto &metadata_manager = GetMetadataManager();
	MetaBlockPointer meta_block(block_manager.GetMetaBlock(), 0);
	if (!meta_block.IsValid()) {
		// storage is empty
		return;
	}

	if (block_manager.Prefetch()) {
		auto metadata_blocks = metadata_manager.GetBlocks();
		auto &buffer_manager = BufferManager::GetBufferManager(storage.GetDatabase());
		// Database load happens before any query is running, so there is no query context to attribute to.
		buffer_manager.Prefetch(QueryContext(), metadata_blocks);
	}

	// create the MetadataReader to read from the storage
	MetadataReader reader(metadata_manager, meta_block);
	auto transaction = CatalogTransaction::GetSystemTransaction(catalog.GetDatabase());
	LoadCheckpoint(transaction, reader);
}

void CheckpointWriter::WriteDataManifest(TableCatalogEntry &table, Serializer &serializer) {
	throw InternalException("Unsupported method WriteDataManifest for this checkpoint writer");
}

void CheckpointWriter::WriteEntry(CatalogEntry &entry, Serializer &serializer) {
	if (!entry.duck_managed) {
		// The definition is the owning catalog's to persist; what is this file's is the rows, filed under the
		// identifier that catalog knows the table by.
		auto &table = entry.Cast<TableCatalogEntry>();
		serializer.WriteProperty(98, "manifest_catalog_id", table.GetStorage().GetDataTableInfo()->GetCatalogId());
		serializer.WriteProperty(99, "catalog_type", entry.type);
		WriteDataManifest(table, serializer);
		return;
	}
	serializer.WriteProperty(99, "catalog_type", entry.type);

	switch (entry.type) {
	case CatalogType::SCHEMA_ENTRY: {
		auto &schema = entry.Cast<SchemaCatalogEntry>();
		WriteSchema(schema, serializer);
		break;
	}
	case CatalogType::TYPE_ENTRY: {
		auto &custom_type = entry.Cast<TypeCatalogEntry>();
		WriteType(custom_type, serializer);
		break;
	}
	case CatalogType::SEQUENCE_ENTRY: {
		auto &seq = entry.Cast<SequenceCatalogEntry>();
		WriteSequence(seq, serializer);
		break;
	}
	case CatalogType::TABLE_ENTRY: {
		auto &table = entry.Cast<TableCatalogEntry>();
		WriteTable(table, serializer);
		break;
	}
	case CatalogType::VIEW_ENTRY: {
		auto &view = entry.Cast<ViewCatalogEntry>();
		WriteView(view, serializer);
		break;
	}
	case CatalogType::MACRO_ENTRY: {
		auto &macro = entry.Cast<ScalarMacroCatalogEntry>();
		WriteMacro(macro, serializer);
		break;
	}
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro = entry.Cast<TableMacroCatalogEntry>();
		WriteTableMacro(macro, serializer);
		break;
	}
	case CatalogType::INDEX_ENTRY: {
		auto &index = entry.Cast<IndexCatalogEntry>();
		WriteIndex(index, serializer);
		break;
	}
	case CatalogType::TRIGGER_ENTRY: {
		auto &trigger = entry.Cast<TriggerCatalogEntry>();
		WriteTrigger(trigger, serializer);
		break;
	}
	default:
		throw InternalException("Unrecognized catalog type in CheckpointWriter::WriteEntry");
	}
}

//===--------------------------------------------------------------------===//
// Schema
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteSchema(SchemaCatalogEntry &schema, Serializer &serializer) {
	// write the schema data
	serializer.WriteProperty(100, "schema", &schema);
}

void CheckpointReader::ReadEntry(CatalogTransaction transaction, Deserializer &deserializer) {
	auto manifest_catalog_id = deserializer.ReadPropertyWithExplicitDefault<idx_t>(98, "manifest_catalog_id", 0);
	auto type = deserializer.ReadProperty<CatalogType>(99, "type");
	if (manifest_catalog_id != 0) {
		ReadDataManifest(transaction, deserializer, manifest_catalog_id);
		return;
	}

	switch (type) {
	case CatalogType::SCHEMA_ENTRY: {
		ReadSchema(transaction, deserializer);
		break;
	}
	case CatalogType::TYPE_ENTRY: {
		ReadType(transaction, deserializer);
		break;
	}
	case CatalogType::SEQUENCE_ENTRY: {
		ReadSequence(transaction, deserializer);
		break;
	}
	case CatalogType::TABLE_ENTRY: {
		ReadTable(transaction, deserializer);
		break;
	}
	case CatalogType::VIEW_ENTRY: {
		ReadView(transaction, deserializer);
		break;
	}
	case CatalogType::MACRO_ENTRY: {
		ReadMacro(transaction, deserializer);
		break;
	}
	case CatalogType::TABLE_MACRO_ENTRY: {
		ReadTableMacro(transaction, deserializer);
		break;
	}
	case CatalogType::INDEX_ENTRY: {
		ReadIndex(transaction, deserializer);
		break;
	}
	case CatalogType::TRIGGER_ENTRY: {
		ReadTrigger(transaction, deserializer);
		break;
	}
	default:
		throw InternalException("Unrecognized catalog type in CheckpointWriter::WriteEntry");
	}
}

void CheckpointReader::ReadSchema(CatalogTransaction transaction, Deserializer &deserializer) {
	// Read the schema and create it in the catalog
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "schema");
	auto &schema_info = info->Cast<CreateSchemaInfo>();

	// we set create conflict to IGNORE_ON_CONFLICT, so that we can ignore a failure when recreating the main schema
	schema_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	catalog.CreateSchema(transaction, schema_info);
}

//===--------------------------------------------------------------------===//
// Views
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteView(ViewCatalogEntry &view, Serializer &serializer) {
	serializer.WriteProperty(100, "view", &view);
}

void CheckpointReader::ReadView(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "view");
	auto &view_info = info->Cast<CreateViewInfo>();
	catalog.CreateView(transaction, view_info);
}

//===--------------------------------------------------------------------===//
// Triggers
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteTrigger(TriggerCatalogEntry &trigger, Serializer &serializer) {
	serializer.WriteProperty(100, "trigger", &trigger);
}

void CheckpointReader::ReadTrigger(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "trigger");
	auto &trigger_info = info->Cast<CreateTriggerInfo>();
	trigger_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	auto &schema = catalog.GetSchema(transaction, trigger_info.GetQualifiedName().Schema());
	auto table_entry = schema.GetEntry(transaction, CatalogType::TABLE_ENTRY, trigger_info.base_table->Table());
	if (!table_entry) {
		throw IOException("corrupt database file - trigger entry without table entry");
	}
	auto &duck_table = table_entry->Cast<DuckTableEntry>();
	duck_table.CreateTrigger(transaction, trigger_info);
}

//===--------------------------------------------------------------------===//
// Sequences
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteSequence(SequenceCatalogEntry &seq, Serializer &serializer) {
	auto info = seq.GetInfo();
	serializer.WriteProperty(100, "sequence", info.get());
}

void CheckpointReader::ReadSequence(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "sequence");
	auto &sequence_info = info->Cast<CreateSequenceInfo>();
	catalog.CreateSequence(transaction, sequence_info);
}

//===--------------------------------------------------------------------===//
// Indexes
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteIndex(IndexCatalogEntry &index_catalog_entry, Serializer &serializer) {
	// The index data is written as part of WriteTableData
	// Here, we serialize the index catalog entry

	// we need to keep the tag "index", even though it is slightly misleading
	serializer.WriteProperty(100, "index", &index_catalog_entry);
}

void CheckpointReader::ReadIndex(CatalogTransaction transaction, Deserializer &deserializer) {
	// we need to keep the tag "index", even though it is slightly misleading.
	auto create_info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "index");

	// also, we have to read the root_block_pointer, which will not be valid for newer storage versions.
	// This leads to different code paths in this function.
	auto root_block_pointer =
	    deserializer.ReadPropertyWithExplicitDefault<BlockPointer>(101, "root_block_pointer", BlockPointer());

	CreateIndexEntry(transaction, std::move(create_info), root_block_pointer);
}

void CheckpointReader::CreateIndexEntry(CatalogTransaction transaction, unique_ptr<CreateInfo> create_info,
                                        BlockPointer root_block_pointer) {
	auto &info = create_info->Cast<CreateIndexInfo>();

	// create the index in the catalog

	// look for the table in the catalog
	auto &schema = catalog.GetSchema(transaction, create_info->GetQualifiedName().Schema());
	auto catalog_table = schema.GetEntry(transaction, CatalogType::TABLE_ENTRY, info.table);
	if (!catalog_table) {
		// See internal issue 3663.
		throw IOException("corrupt database file - index entry without table entry");
	}
	auto &table = catalog_table->Cast<DuckTableEntry>();

	// we also need to make sure the index type is loaded
	// backwards compatibility:
	// if the index type is not specified, we default to ART
	if (info.index_type.empty()) {
		info.index_type = ART::TYPE_NAME;
	}

	// now we can look for the index in the catalog and assign the table info
	auto &index = schema.CreateIndex(transaction, info, table)->Cast<DuckIndexEntry>();
	auto &data_table = table.GetStorage();
	auto &table_info = data_table.GetDataTableInfo();

	IndexStorageInfo index_storage_info;
	if (root_block_pointer.IsValid()) {
		// Read older duckdb files.
		index_storage_info.name = index.name;
		index_storage_info.root_block_ptr = root_block_pointer;
	} else {
		// Extract the matching index storage info (moves it out of the stored collection).
		index_storage_info = table_info->ExtractIndexStorageInfo(index.name);
	}

	D_ASSERT(index_storage_info.IsValid());
	D_ASSERT(!index_storage_info.name.empty());

	// Create an unbound index and add it to the table.
	auto unbound_index = make_uniq<UnboundIndex>(std::move(create_info), std::move(index_storage_info),
	                                             TableIOManager::Get(data_table), data_table.db);
	table_info->GetIndexes().AddIndex(std::move(unbound_index));
}

//===--------------------------------------------------------------------===//
// Custom Types
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteType(TypeCatalogEntry &type, Serializer &serializer) {
	serializer.WriteProperty(100, "type", &type);
}

void CheckpointReader::ReadType(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "type");
	auto &type_info = info->Cast<CreateTypeInfo>();
	catalog.CreateType(transaction, type_info);
}

//===--------------------------------------------------------------------===//
// Macro's
//===--------------------------------------------------------------------===//
void CheckpointWriter::WriteMacro(ScalarMacroCatalogEntry &macro, Serializer &serializer) {
	serializer.WriteProperty(100, "macro", &macro);
}

void CheckpointReader::ReadMacro(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "macro");
	auto &macro_info = info->Cast<CreateMacroInfo>();
	catalog.CreateFunction(transaction, macro_info);
}

void CheckpointWriter::WriteTableMacro(TableMacroCatalogEntry &macro, Serializer &serializer) {
	serializer.WriteProperty(100, "table_macro", &macro);
}

void CheckpointReader::ReadTableMacro(CatalogTransaction transaction, Deserializer &deserializer) {
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "table_macro");
	auto &macro_info = info->Cast<CreateMacroInfo>();
	catalog.CreateFunction(transaction, macro_info);
}

//===--------------------------------------------------------------------===//
// Table Metadata
//===--------------------------------------------------------------------===//
void SingleFileCheckpointWriter::WriteTable(TableCatalogEntry &table, Serializer &serializer) {
	// Write the table metadata
	serializer.WriteProperty(100, "table", &table);

	// If there is a context available, bind indexes before serialization.
	// This is necessary so that buffered index operations are replayed before we checkpoint, otherwise
	// we would lose them if there was a restart after this.
	if (context && context->transaction.HasActiveTransaction()) {
		auto &info = table.GetStorage().GetDataTableInfo();
		info->BindIndexes(*context);
	}
	// FIXME: If we do not have a context, however, the unbound indexes have to be serialized to disk.

	// Write the table data
	auto table_lock = table.GetStorage().GetCheckpointLock();
	auto writer = GetTableDataWriter(table);
	if (writer) {
		writer->WriteTableData(serializer);
	}
}

void SingleFileCheckpointWriter::WriteDataManifest(TableCatalogEntry &table, Serializer &serializer) {
	auto &storage = table.GetStorage();
	if (context && context->transaction.HasActiveTransaction()) {
		storage.GetDataTableInfo()->BindIndexes(*context);
	}
	auto table_lock = storage.GetCheckpointLock();
	auto writer = GetTableDataWriter(table);
	if (writer) {
		writer->WriteTableData(serializer);
	}
}

void CheckpointReader::ReadTable(CatalogTransaction transaction, Deserializer &deserializer) {
	// deserialize the table meta data
	auto info = deserializer.ReadProperty<unique_ptr<CreateInfo>>(100, "table");
	auto &schema = catalog.GetSchema(transaction, info->GetQualifiedName().Schema());
	auto bound_info = Binder::BindCreateTableCheckpoint(std::move(info), schema);

	for (auto &dep : bound_info->Base().dependencies.Set()) {
		bound_info->dependencies.AddDependency(dep);
	}

	// now read the actual table data and place it into the CreateTableInfo
	ReadTableData(transaction, deserializer, *bound_info);

	// finally create the table in the catalog
	catalog.CreateTable(transaction, *bound_info);
}

void CheckpointReader::ReadDataManifest(CatalogTransaction transaction, Deserializer &deserializer, idx_t catalog_id) {
	auto &config = DBConfig::GetConfig(catalog.GetDatabase());
	if (!config.table_definition_provider) {
		throw IOException("corrupt database file - data manifest for table %llu, whose definition no catalog holds",
		                  catalog_id);
	}
	auto definition = config.table_definition_provider(catalog.GetAttached(), catalog_id, /*with_checks=*/true);
	if (!definition) {
		// The table was dropped after this checkpoint was taken. Its rows are still here and the drop is in the WAL
		// about to replay, so there is nothing to attach them to and nothing to keep: read past them and let the
		// next checkpoint stop referencing the blocks.
		SkipTableData(deserializer);
		return;
	}
	auto &schema = catalog.GetSchema(transaction, definition->table->GetQualifiedName().Schema());

	unique_ptr<BoundCreateTableInfo> bound_info;
	try {
		bound_info = Binder::BindCreateTableCheckpoint(std::move(definition->table), schema);
	} catch (const std::exception &) {
		// The CHECK constraints of a host definition do not always bind here -- one naming a function of the host's
		// own catalog does not, which is why the host creates such a table without them and enforces them itself.
		// Retry the way it did rather than refuse to open the database.
		definition = config.table_definition_provider(catalog.GetAttached(), catalog_id, /*with_checks=*/false);
		if (!definition) {
			throw;
		}
		bound_info = Binder::BindCreateTableCheckpoint(std::move(definition->table), schema);
	}

	for (auto &dep : bound_info->Base().dependencies.Set()) {
		bound_info->dependencies.AddDependency(dep);
	}

	ReadTableData(transaction, deserializer, *bound_info);

	// A named constraint this file has no index for was added to the host catalog after the checkpoint. Leave it
	// out of the table: the ALTER that added it is in the WAL about to replay, and replaying it is what builds the
	// index over the rows -- attaching an empty one here would present it as complete.
	auto &table_base = bound_info->Base().Cast<CreateTableInfo>();
	auto has_index_data = [&](const string &index_name) {
		for (auto &index : bound_info->indexes) {
			if (index.name == index_name) {
				return true;
			}
		}
		return false;
	};
	for (idx_t i = table_base.constraints.size(); i > 0; i--) {
		auto &constraint = table_base.constraints[i - 1];
		const bool indexed =
		    constraint->type == ConstraintType::UNIQUE || constraint->type == ConstraintType::FOREIGN_KEY;
		if (!indexed || constraint->constraint_name.empty() || has_index_data(constraint->constraint_name)) {
			continue;
		}
		table_base.constraints.erase_at(i - 1);
	}

	auto entry = catalog.CreateTable(transaction, *bound_info);
	if (!entry) {
		return;
	}
	auto &table_info = entry->Cast<DuckTableEntry>().GetStorage().GetDataTableInfo();
	for (auto &index : definition->indexes) {
		// An index the catalog holds but this file does not: it was created after the checkpoint, so its build is
		// in the WAL that replays next. Attaching it here with no data would present it as complete and empty.
		if (!table_info->HasIndexStorageInfo(index->GetQualifiedName().Name())) {
			continue;
		}
		CreateIndexEntry(transaction, std::move(index), BlockPointer());
	}
}

void CheckpointReader::SkipTableData(Deserializer &deserializer) {
	deserializer.ReadProperty<MetaBlockPointer>(101, "table_pointer");
	auto total_rows = deserializer.ReadProperty<idx_t>(102, "total_rows");
	deserializer.ReadPropertyWithExplicitDefault<vector<BlockPointer>>(103, "index_pointers", {});
	deserializer.ReadPropertyWithExplicitDefault<vector<IndexStorageInfo>>(104, "index_storage_infos", {});
	deserializer.ReadPropertyWithExplicitDefault<idx_t>(105, "next_row_id", total_rows);
}

void CheckpointReader::ReadTableData(CatalogTransaction transaction, Deserializer &deserializer,
                                     BoundCreateTableInfo &bound_info) {
	// written in "SingleFileTableDataWriter::FinalizeTable"
	auto table_pointer = deserializer.ReadProperty<MetaBlockPointer>(101, "table_pointer");
	auto total_rows = deserializer.ReadProperty<idx_t>(102, "total_rows");

	// Cover reading old storage files.
	auto index_pointers = deserializer.ReadPropertyWithExplicitDefault<vector<BlockPointer>>(103, "index_pointers", {});
	// Cover reading new storage files.
	auto index_storage_infos =
	    deserializer.ReadPropertyWithExplicitDefault<vector<IndexStorageInfo>>(104, "index_storage_infos", {});
	// Read next_row_id as total_rows for backwards compatibility. Older storage versions do not allow for gaps in
	// row_id numbering, in which case next_row_id = total_rows.
	auto next_row_id = deserializer.ReadPropertyWithExplicitDefault<idx_t>(105, "next_row_id", total_rows);
	D_ASSERT(next_row_id >= total_rows);

	if (!index_storage_infos.empty()) {
		bound_info.indexes = std::move(index_storage_infos);

	} else {
		// This is an old duckdb file containing index pointers and deprecated storage.
		for (idx_t i = 0; i < index_pointers.size(); i++) {
			// Deprecated storage is always true for old duckdb files.
			IndexStorageInfo index_storage_info;
			index_storage_info.root_block_ptr = index_pointers[i];
			bound_info.indexes.push_back(std::move(index_storage_info));
		}
	}

	// FIXME: icky downcast to get the underlying MetadataReader
	auto &binary_deserializer = dynamic_cast<BinaryDeserializer &>(deserializer);
	auto &reader = dynamic_cast<MetadataReader &>(binary_deserializer.GetStream());

	vector<MetaBlockPointer> read_pointers;
	MetadataReader table_data_reader(reader.GetMetadataManager(), table_pointer, read_pointers);
	TableDataReader data_reader(table_data_reader, bound_info, table_pointer);
	data_reader.ReadTableData();

	bound_info.data->total_rows = total_rows;
	bound_info.data->next_row_id = next_row_id;
	bound_info.data->read_metadata_pointers = read_pointers;
}

} // namespace duckdb
