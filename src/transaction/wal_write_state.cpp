#include "duckdb/transaction/wal_write_state.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/trigger_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/chunk_info.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/append_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/transaction/update_info.hpp"

namespace duckdb {

WALWriteState::WALWriteState(DuckTransaction &transaction_p, optional_ptr<WriteAheadLog> log,
                             optional_ptr<StorageCommitState> commit_state, optional_ptr<WriteAheadLog> catalog_log)
    : transaction(transaction_p), log(log), commit_state(commit_state), catalog_log(catalog_log),
      current_table_entry(nullptr) {
}

WriteAheadLog &WALWriteState::Log() {
	if (!log) {
		throw InternalException("WALWriteState - database \"%s\" has no WAL to write data to",
		                        transaction.manager.GetDB().GetName());
	}
	return *log;
}

void WALWriteState::SwitchTable(DuckTableEntry &table_entry, UndoFlags new_op) {
	if (current_table_entry.get() != &table_entry) {
		// write the current table to the log
		Log().WriteSetTable(table_entry);
		current_table_entry = table_entry;
	}
}

static bool IsAlterableLoggedEntry(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::TOKENIZER_ENTRY:
	case CatalogType::ROLE_ENTRY:
	case CatalogType::DATABASE_ENTRY:
	case CatalogType::FOREIGN_SERVER_ENTRY:
	case CatalogType::SCHEMA_ENTRY:
		return true;
	default:
		return false;
	}
}

static optional_ptr<DataTableInfo> IndexTableInfo(CatalogEntry &entry, const AlterInfo *alter_info) {
	if (alter_info) {
		return nullptr;
	}
	optional_ptr<CatalogEntry> index;
	if (entry.Parent().type == CatalogType::INDEX_ENTRY) {
		index = entry.Parent();
	} else if (entry.Parent().type == CatalogType::DELETED_ENTRY && entry.type == CatalogType::INDEX_ENTRY) {
		index = entry;
	}
	if (!index) {
		return nullptr;
	}
	auto &duck_index = index->Cast<DuckIndexEntry>();
	if (!duck_index.info || !duck_index.info->info) {
		return nullptr;
	}
	return duck_index.info->info.get();
}

static bool ChangesTableStorage(CatalogEntry &entry, const AlterInfo *alter_info) {
	auto &parent = entry.Parent();
	if (parent.type == CatalogType::DELETED_ENTRY) {
		return entry.type == CatalogType::TABLE_ENTRY && entry.Cast<TableCatalogEntry>().IsDuckTable();
	}
	if (parent.type != CatalogType::TABLE_ENTRY || !parent.Cast<TableCatalogEntry>().IsDuckTable()) {
		return false;
	}
	if (!alter_info) {
		return true;
	}
	if (entry.type != CatalogType::TABLE_ENTRY || alter_info->type != AlterType::ALTER_TABLE) {
		return false;
	}
	switch (alter_info->Cast<AlterTableInfo>().alter_table_type) {
	case AlterTableType::RENAME_COLUMN:
	case AlterTableType::ADD_COLUMN:
	case AlterTableType::REMOVE_COLUMN:
	case AlterTableType::ALTER_COLUMN_TYPE:
	case AlterTableType::ADD_FIELD:
	case AlterTableType::REMOVE_FIELD:
	case AlterTableType::RENAME_FIELD:
		return true;
	default:
		return false;
	}
}

void WALWriteState::WriteCatalogEntry(CatalogEntry &entry, data_ptr_t dataptr) {
	if (entry.temporary || entry.Parent().temporary) {
		return;
	}
	auto &parent = entry.Parent();
	unique_ptr<ParseInfo> parse_info;
	if (IsAlterableLoggedEntry(parent.type) &&
	    (entry.type == CatalogType::RENAMED_ENTRY || entry.type == parent.type)) {
		// ALTER statement, read the extra data after the entry
		auto extra_data_size = Load<idx_t>(dataptr);
		auto extra_data = data_ptr_cast(dataptr + sizeof(idx_t));

		MemoryStream source(extra_data, extra_data_size);
		BinaryDeserializer deserializer(source);
		deserializer.Begin();
		auto column_name = deserializer.ReadProperty<string>(100, "column_name");
		parse_info = deserializer.ReadProperty<unique_ptr<ParseInfo>>(101, "alter_info");
		deserializer.End();
	}
	auto alter_info = parse_info ? &parse_info->Cast<AlterInfo>() : nullptr;
	if (!catalog_log) {
		WriteCatalogEntry(Log(), entry, alter_info);
		return;
	}
	if (!catalog_selected) {
		catalog_log->WriteUseCatalog(transaction.manager.GetDB().oid);
		catalog_selected = true;
	}
	WriteCatalogEntry(*catalog_log, entry, alter_info);
	if (!log) {
		return;
	}
	if (ChangesTableStorage(entry, alter_info)) {
		if (entry.type == CatalogType::TABLE_ENTRY) {
			SwitchTable(entry.Cast<DuckTableEntry>(), UndoFlags::CATALOG_ENTRY);
		}
		WriteCatalogEntry(*log, entry, alter_info);
		return;
	}
	auto index_table = IndexTableInfo(entry, alter_info);
	if (index_table) {
		log->WriteSetTable(QualifiedName(index_table->GetSchemaPath(), index_table->GetTableName()),
		                   index_table->GetTableOid());
		current_table_entry = nullptr;
		WriteCatalogEntry(*log, entry, alter_info);
	}
}

void WALWriteState::WriteCatalogEntry(WriteAheadLog &target, CatalogEntry &entry, const AlterInfo *alter_info) {
	auto &parent = entry.Parent();
	const bool with_index_storage = &target != catalog_log.get();

	switch (parent.type) {
	case CatalogType::TRIGGER_ENTRY:
		D_ASSERT(entry.type != CatalogType::RENAMED_ENTRY);
		if (entry.type == parent.type) {
			// Column-rename propagation from the owning ALTER TABLE — the ALTER_INFO record
			// already covers this on replay; writing a second CREATE_TRIGGER would be redundant.
			return;
		}
		target.WriteCreateTrigger(parent.Cast<TriggerCatalogEntry>());
		break;
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
	case CatalogType::TOKENIZER_ENTRY:
	case CatalogType::ROLE_ENTRY:
	case CatalogType::DATABASE_ENTRY:
	case CatalogType::FOREIGN_SERVER_ENTRY:
	case CatalogType::SCHEMA_ENTRY:
		if (alter_info) {
			target.WriteAlter(entry, *alter_info, with_index_storage);
		} else {
			switch (parent.type) {
			case CatalogType::TABLE_ENTRY:
				// CREATE TABLE statement
				target.WriteCreateTable(parent.Cast<TableCatalogEntry>());
				break;
			case CatalogType::VIEW_ENTRY:
				// CREATE VIEW statement
				target.WriteCreateView(parent.Cast<ViewCatalogEntry>());
				break;
			case CatalogType::INDEX_ENTRY:
				// CREATE INDEX statement
				target.WriteCreateIndex(parent.Cast<IndexCatalogEntry>(), with_index_storage);
				break;
			case CatalogType::SEQUENCE_ENTRY:
				// CREATE SEQUENCE statement
				target.WriteCreateSequence(parent.Cast<SequenceCatalogEntry>());
				break;
			case CatalogType::TYPE_ENTRY:
				// CREATE TYPE statement
				target.WriteCreateType(parent.Cast<TypeCatalogEntry>());
				break;
			case CatalogType::MACRO_ENTRY:
				target.WriteCreateMacro(parent.Cast<ScalarMacroCatalogEntry>());
				break;
			case CatalogType::TABLE_MACRO_ENTRY:
				target.WriteCreateTableMacro(parent.Cast<TableMacroCatalogEntry>());
				break;
			case CatalogType::TOKENIZER_ENTRY:
				target.WriteCreateTokenizer(parent.Cast<StandardEntry>());
				break;
			case CatalogType::ROLE_ENTRY:
				target.WriteCreateRole(parent.Cast<InCatalogEntry>());
				break;
			case CatalogType::DATABASE_ENTRY:
				target.WriteCreateDatabase(parent.Cast<InCatalogEntry>());
				break;
			case CatalogType::FOREIGN_SERVER_ENTRY:
				target.WriteCreateForeignServer(parent.Cast<InCatalogEntry>());
				break;
			case CatalogType::SCHEMA_ENTRY:
				target.WriteCreateSchema(parent.Cast<SchemaCatalogEntry>());
				break;
			default:
				throw InternalException("Don't know how to create this type!");
			}
		}
		break;
	case CatalogType::RENAMED_ENTRY:
		// This is a rename, nothing needs to be done for this
		break;
	case CatalogType::DELETED_ENTRY:
		switch (entry.type) {
		case CatalogType::TABLE_ENTRY:
			target.WriteDropTable(entry.Cast<TableCatalogEntry>());
			break;
		case CatalogType::SCHEMA_ENTRY:
			target.WriteDropSchema(entry.Cast<SchemaCatalogEntry>());
			break;
		case CatalogType::VIEW_ENTRY:
			target.WriteDropView(entry.Cast<ViewCatalogEntry>());
			break;
		case CatalogType::SEQUENCE_ENTRY:
			target.WriteDropSequence(entry.Cast<SequenceCatalogEntry>());
			break;
		case CatalogType::MACRO_ENTRY:
			target.WriteDropMacro(entry.Cast<ScalarMacroCatalogEntry>());
			break;
		case CatalogType::TABLE_MACRO_ENTRY:
			target.WriteDropTableMacro(entry.Cast<TableMacroCatalogEntry>());
			break;
		case CatalogType::TYPE_ENTRY:
			target.WriteDropType(entry.Cast<TypeCatalogEntry>());
			break;
		case CatalogType::INDEX_ENTRY: {
			target.WriteDropIndex(entry.Cast<IndexCatalogEntry>());
			break;
		}
		case CatalogType::TRIGGER_ENTRY:
			target.WriteDropTrigger(entry.Cast<TriggerCatalogEntry>());
			break;
		case CatalogType::TOKENIZER_ENTRY:
			target.WriteDropTokenizer(entry.Cast<StandardEntry>());
			break;
		case CatalogType::ROLE_ENTRY:
			target.WriteDropRole(entry.Cast<InCatalogEntry>());
			break;
		case CatalogType::DATABASE_ENTRY:
			target.WriteDropDatabase(entry.Cast<InCatalogEntry>());
			break;
		case CatalogType::FOREIGN_SERVER_ENTRY:
			target.WriteDropForeignServer(entry.Cast<InCatalogEntry>());
			break;
		case CatalogType::RENAMED_ENTRY:
		case CatalogType::PREPARED_STATEMENT:
		case CatalogType::SCALAR_FUNCTION_ENTRY:
		case CatalogType::DEPENDENCY_ENTRY:
		case CatalogType::SECRET_ENTRY:
		case CatalogType::SECRET_TYPE_ENTRY:
		case CatalogType::SECRET_FUNCTION_ENTRY:
			// do nothing, prepared statements and scalar functions aren't persisted to disk
			break;
		default:
			throw InternalException("Don't know how to drop this type!");
		}
		break;
	case CatalogType::PREPARED_STATEMENT:
	case CatalogType::AGGREGATE_FUNCTION_ENTRY:
	case CatalogType::SCALAR_FUNCTION_ENTRY:
	case CatalogType::TABLE_FUNCTION_ENTRY:
	case CatalogType::COPY_FUNCTION_ENTRY:
	case CatalogType::PRAGMA_FUNCTION_ENTRY:
	case CatalogType::COLLATION_ENTRY:
	case CatalogType::COORDINATE_SYSTEM_ENTRY:
	case CatalogType::DEPENDENCY_ENTRY:
	case CatalogType::SECRET_ENTRY:
	case CatalogType::SECRET_TYPE_ENTRY:
	case CatalogType::SECRET_FUNCTION_ENTRY:
		// do nothing, these entries are not persisted to disk
		break;
	default:
		throw InternalException("UndoBuffer - don't know how to write this entry to the WAL");
	}
}

void WALWriteState::WriteDelete(DeleteInfo &info) {
	// switch to the current table, if necessary
	SwitchTable(*info.table, UndoFlags::DELETE_TUPLE);

	if (!delete_chunk) {
		delete_chunk = make_uniq<DataChunk>();
		vector<LogicalType> delete_types = {LogicalType::ROW_TYPE};
		delete_chunk->Initialize(Allocator::DefaultAllocator(), delete_types);
	}
	auto rows = FlatVector::GetDataMutable<row_t>(delete_chunk->data[0]);
	if (info.is_consecutive) {
		for (idx_t i = 0; i < info.count; i++) {
			rows[i] = UnsafeNumericCast<int64_t>(info.base_row + i);
		}
	} else {
		auto delete_rows = info.GetRows();
		for (idx_t i = 0; i < info.count; i++) {
			rows[i] = UnsafeNumericCast<int64_t>(info.base_row) + delete_rows[i];
		}
	}
	delete_chunk->SetChildCardinality(info.count);
	Log().WriteDelete(*delete_chunk);
}

void WALWriteState::WriteUpdate(UpdateInfo &info) {
	// switch to the current table, if necessary
	auto &column_data = info.segment->column_data;

	SwitchTable(*info.table, UndoFlags::UPDATE_TUPLE);

	// initialize the update chunk
	vector<LogicalType> update_types;
	if (column_data.type.id() == LogicalTypeId::VALIDITY) {
		update_types.emplace_back(LogicalType::BOOLEAN);
	} else {
		update_types.push_back(column_data.type);
	}
	update_types.emplace_back(LogicalType::ROW_TYPE);

	update_chunk = make_uniq<DataChunk>();
	update_chunk->Initialize(Allocator::DefaultAllocator(), update_types);

	// fetch the updated values from the base segment
	info.segment->FetchCommitted(info.vector_index, update_chunk->data[0]);

	// write the row ids into the chunk
	auto row_ids = FlatVector::GetDataMutable<row_t>(update_chunk->data[1]);
	idx_t start = info.row_group_start + info.vector_index * STANDARD_VECTOR_SIZE;
	auto tuples = info.GetTuples();
	for (idx_t i = 0; i < info.N; i++) {
		row_ids[tuples[i]] = UnsafeNumericCast<int64_t>(start + tuples[i]);
	}
	if (column_data.type.id() == LogicalTypeId::VALIDITY) {
		// zero-initialize the booleans
		// FIXME: this is only required because of NullValue<T> in Vector::Serialize...
		auto booleans = FlatVector::GetDataMutable<bool>(update_chunk->data[0]);
		for (idx_t i = 0; i < info.N; i++) {
			auto idx = tuples[i];
			booleans[idx] = false;
		}
	}
	SelectionVector sel(tuples, info.N);
	update_chunk->Slice(sel, info.N);

	// construct the column index path
	vector<column_t> column_indexes;
	reference<const ColumnData> current_column_data = column_data;
	while (current_column_data.get().HasParent()) {
		column_indexes.push_back(current_column_data.get().column_index);
		current_column_data = current_column_data.get().Parent();
	}
	column_indexes.push_back(info.column_index);
	std::reverse(column_indexes.begin(), column_indexes.end());

	Log().WriteUpdate(*update_chunk, column_indexes);
}

void WALWriteState::CommitEntry(UndoFlags type, data_ptr_t data) {
	switch (type) {
	case UndoFlags::CATALOG_ENTRY: {
		// set the commit timestamp of the catalog entry to the given id
		auto catalog_entry = Load<CatalogEntry *>(data);
		D_ASSERT(catalog_entry->HasParent());
		// push the catalog update to the WAL
		WriteCatalogEntry(*catalog_entry, data + sizeof(CatalogEntry *));
		break;
	}
	case UndoFlags::INSERT_TUPLE: {
		// append:
		auto info = reinterpret_cast<AppendInfo *>(data);
		if (!info->table->GetStorage().IsTemporary()) {
			SwitchTable(*info->table, UndoFlags::INSERT_TUPLE);
			info->table->GetStorage().WriteToLog(transaction, Log(), info->start_row, info->count, commit_state.get());
		}
		break;
	}
	case UndoFlags::DELETE_TUPLE: {
		// deletion:
		auto info = reinterpret_cast<DeleteInfo *>(data);
		if (!info->table->GetStorage().IsTemporary()) {
			WriteDelete(*info);
		}
		break;
	}
	case UndoFlags::UPDATE_TUPLE: {
		// update:
		auto info = reinterpret_cast<UpdateInfo *>(data);
		if (!info->segment->column_data.GetTableInfo().IsTemporary()) {
			WriteUpdate(*info);
		}
		break;
	}
	case UndoFlags::ATTACHED_DATABASE:
		break;
	case UndoFlags::SEQUENCE_VALUE: {
		auto info = reinterpret_cast<SequenceValue *>(data);
		Log().WriteSequenceValue(*info);
		break;
	}
	default:
		throw InternalException("UndoBuffer - don't know how to commit this type!");
	}
}

} // namespace duckdb
