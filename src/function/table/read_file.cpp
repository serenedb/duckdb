#include "duckdb/function/table/read_file.hpp"
#include "duckdb/function/table/direct_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table/range.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"

namespace duckdb {

namespace {

//------------------------------------------------------------------------------
// DirectMultiFileInfo
//------------------------------------------------------------------------------

template <class OP>
struct DirectMultiFileInfo : MultiFileReaderInterface {
	static unique_ptr<MultiFileReaderInterface> CreateInterface(ClientContext &context);
	unique_ptr<BaseFileReaderOptions> InitializeOptions(ClientContext &context,
	                                                    optional_ptr<TableFunctionInfo> info) override;
	bool ParseCopyOption(ClientContext &context, const string &key, const vector<Value> &values,
	                     BaseFileReaderOptions &options, vector<string> &expected_names,
	                     vector<LogicalType> &expected_types) override;
	bool ParseOption(ClientContext &context, const string &key, const Value &val, MultiFileOptions &file_options,
	                 BaseFileReaderOptions &options) override;
	unique_ptr<TableFunctionData> InitializeBindData(MultiFileBindData &multi_file_data,
	                                                 unique_ptr<BaseFileReaderOptions> options) override;
	void BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<string> &names,
	                MultiFileBindData &bind_data) override;
	optional_idx MaxThreads(const MultiFileBindData &bind_data_p, const MultiFileGlobalState &global_state,
	                        FileExpandResult expand_result) override;
	unique_ptr<GlobalTableFunctionState> InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data,
	                                                           MultiFileGlobalState &global_state) override;
	unique_ptr<LocalTableFunctionState> InitializeLocalState(ExecutionContext &, GlobalTableFunctionState &) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        BaseUnionData &union_data, const MultiFileBindData &bind_data_p) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        const OpenFileInfo &file, idx_t file_idx,
	                                        const MultiFileBindData &bind_data) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, const OpenFileInfo &file,
	                                        BaseFileReaderOptions &options,
	                                        const MultiFileOptions &file_options) override;
	unique_ptr<NodeStatistics> GetCardinality(const MultiFileBindData &bind_data, idx_t file_count) override;
	FileGlobInput GetGlobInput() override;
	void GetVirtualColumns(ClientContext &, MultiFileBindData &, virtual_column_map_t &result) override;
};

template <class OP>
unique_ptr<MultiFileReaderInterface> DirectMultiFileInfo<OP>::CreateInterface(ClientContext &context) {
	return make_uniq<DirectMultiFileInfo>();
}

template <class OP>
unique_ptr<BaseFileReaderOptions> DirectMultiFileInfo<OP>::InitializeOptions(ClientContext &context,
                                                                             optional_ptr<TableFunctionInfo> info) {
	return make_uniq<BaseFileReaderOptions>();
}

template <class OP>
bool DirectMultiFileInfo<OP>::ParseCopyOption(ClientContext &context, const string &key, const vector<Value> &values,
                                              BaseFileReaderOptions &options, vector<string> &expected_names,
                                              vector<LogicalType> &expected_types) {
	return true;
}

template <class OP>
bool DirectMultiFileInfo<OP>::ParseOption(ClientContext &context, const string &key, const Value &val,
                                          MultiFileOptions &file_options, BaseFileReaderOptions &options) {
	return true;
}

template <class OP>
unique_ptr<TableFunctionData> DirectMultiFileInfo<OP>::InitializeBindData(MultiFileBindData &multi_file_data,
                                                                          unique_ptr<BaseFileReaderOptions> options) {
	auto result = make_uniq<ReadFileBindData>();
	result->options = std::move(options);
	return std::move(result);
}

template <class OP>
void DirectMultiFileInfo<OP>::BindReader(ClientContext &context, vector<LogicalType> &return_types,
                                         vector<string> &names, MultiFileBindData &bind_data) {
	auto &read_bind = bind_data.bind_data->Cast<ReadFileBindData>();
	bind_data.reader_bind = bind_data.multi_file_reader->BindReader(
	    context, return_types, names, *bind_data.file_list, bind_data, *read_bind.options, bind_data.file_options);
}

template <class OP>
optional_idx DirectMultiFileInfo<OP>::MaxThreads(const MultiFileBindData &bind_data_p,
                                                 const MultiFileGlobalState &global_state,
                                                 FileExpandResult expand_result) {
	return 1;
}

template <class OP>
unique_ptr<GlobalTableFunctionState>
DirectMultiFileInfo<OP>::InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data,
                                               MultiFileGlobalState &global_state) {
	auto result = make_uniq<ReadFileGlobalState>();

	result->file_list = bind_data.file_list;
	// state.column_ids must align with reader.column_ids slot-for-slot. The framework
	// column mapper handles "constant" virtuals (filename, file_index) via constant_map
	// and OMITS them from reader.column_ids. Mirror that here: keep file-relative cols
	// plus the file_row_number virtual which the reader populates itself.
	vector<idx_t> column_ids;
	column_ids.reserve(global_state.column_indexes.size());
	for (idx_t i = 0; i < global_state.column_indexes.size(); i++) {
		const auto col_id = global_state.column_indexes[i].GetPrimaryIndex();
		if (col_id == MultiFileReader::COLUMN_IDENTIFIER_FILENAME || col_id == COLUMN_IDENTIFIER_EMPTY ||
		    col_id == MultiFileReader::COLUMN_IDENTIFIER_FILE_INDEX) {
			continue;
		}
		column_ids.push_back(col_id);
	}

	for (const auto &column_id : column_ids) {
		if (column_id == ReadFileBindData::FILE_CONTENT_COLUMN || column_id == ReadFileBindData::FILE_SIZE_COLUMN ||
		    column_id == ReadFileBindData::FILE_LAST_MODIFIED_COLUMN) {
			result->requires_file_open = true;
			break;
		}
	}

	result->column_ids = std::move(column_ids);
	return std::move(result);
}

template <class OP>
unique_ptr<LocalTableFunctionState> DirectMultiFileInfo<OP>::InitializeLocalState(ExecutionContext &,
                                                                                  GlobalTableFunctionState &) {
	return make_uniq<LocalTableFunctionState>();
}

template <class OP>
shared_ptr<BaseFileReader>
DirectMultiFileInfo<OP>::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                      BaseUnionData &union_data, const MultiFileBindData &bind_data_p) {
	return make_shared_ptr<DirectFileReader>(union_data.file, OP::TYPE());
}

template <class OP>
shared_ptr<BaseFileReader>
DirectMultiFileInfo<OP>::CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
                                      const OpenFileInfo &file, idx_t file_idx, const MultiFileBindData &bind_data) {
	return make_shared_ptr<DirectFileReader>(file, OP::TYPE());
}

template <class OP>
shared_ptr<BaseFileReader> DirectMultiFileInfo<OP>::CreateReader(ClientContext &context, const OpenFileInfo &file,
                                                                 BaseFileReaderOptions &options_p,
                                                                 const MultiFileOptions &) {
	return make_shared_ptr<DirectFileReader>(file, OP::TYPE());
}

template <class OP>
unique_ptr<NodeStatistics> DirectMultiFileInfo<OP>::GetCardinality(const MultiFileBindData &bind_data,
                                                                   idx_t file_count) {
	auto result = make_uniq<NodeStatistics>();
	result->has_max_cardinality = true;
	result->max_cardinality = bind_data.file_list->GetTotalFileCount();
	result->has_estimated_cardinality = true;
	result->estimated_cardinality = bind_data.file_list->GetTotalFileCount();
	return result;
}

template <class OP>
FileGlobInput DirectMultiFileInfo<OP>::GetGlobInput() {
	return FileGlobOptions::ALLOW_EMPTY;
}

template <class OP>
void DirectMultiFileInfo<OP>::GetVirtualColumns(ClientContext &, MultiFileBindData &, virtual_column_map_t &result) {
	// One row per file -- file_row_number is always 0. Exposing it lets the inverted-index
	// fast path build a stable (file_index, 0) PK, matching `iceberg_scan`'s glob shape.
	result.emplace(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER,
	               TableColumn("file_row_number", LogicalType::BIGINT));
}

//------------------------------------------------------------------------------
// Operations
//------------------------------------------------------------------------------

struct ReadBlobOperation {
	static constexpr const char *NAME = "read_blob";

	static inline LogicalType TYPE() {
		return LogicalType::BLOB;
	}
};

struct ReadTextOperation {
	static constexpr const char *NAME = "read_text";

	static inline LogicalType TYPE() {
		return LogicalType::VARCHAR;
	}
};

template <class OP>
static TableFunction GetFunction() {
	MultiFileFunction<DirectMultiFileInfo<OP>> table_function(OP::NAME);
	// Erase extra multi file reader options
	table_function.named_parameters.erase("filename");
	table_function.named_parameters.erase("hive_partitioning");
	table_function.named_parameters.erase("union_by_name");
	table_function.named_parameters.erase("hive_types");
	table_function.named_parameters.erase("hive_types_autocast");
	return table_function;
}

template <class OP>
struct DirectLookupGlobalState : public GlobalTableFunctionState {
	OpenFileInfo file;
	vector<idx_t> output_to_file_col;
	idx_t file_row_number_idx = DConstants::INVALID_INDEX;
	bool requires_file_open = false;

	// Opened once in init_global, reused across pk_lookups (CSV pattern).
	unique_ptr<FileHandle> file_handle;
	idx_t file_size = 0;

	// Lazily populated on first FILE_CONTENT_COLUMN read, then reused for
	// subsequent pk_lookups within the same call.
	unique_ptr<MemoryStream> content_stream;
};

template <class OP>
unique_ptr<GlobalTableFunctionState> DirectLookupInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->CastNoConst<MultiFileBindData>();
	auto state = make_uniq<DirectLookupGlobalState<OP>>();
	state->file = bind_data.file_list->GetFirstFile();

	state->output_to_file_col.reserve(input.column_indexes.size());
	for (idx_t i = 0; i < input.column_indexes.size(); i++) {
		auto &col = input.column_indexes[i];
		if (col.IsVirtualColumn()) {
			if (col.GetPrimaryIndex() == MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER) {
				state->file_row_number_idx = i;
			}
			state->output_to_file_col.push_back(DConstants::INVALID_INDEX);
			continue;
		}
		const auto col_id = col.GetPrimaryIndex();
		if (col_id == COLUMN_IDENTIFIER_ROW_ID) {
			state->output_to_file_col.push_back(DConstants::INVALID_INDEX);
			continue;
		}
		state->output_to_file_col.push_back(col_id);
		if (col_id == ReadFileBindData::FILE_CONTENT_COLUMN || col_id == ReadFileBindData::FILE_SIZE_COLUMN ||
		    col_id == ReadFileBindData::FILE_LAST_MODIFIED_COLUMN) {
			state->requires_file_open = true;
		}
	}

	if (state->requires_file_open) {
		auto &fs = FileSystem::GetFileSystem(context);
		auto flags = FileFlags::FILE_FLAGS_READ;
		if (FileSystem::IsRemoteFile(state->file.path)) {
			flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
		}
		flags.SetCachingMode(CachingMode::CACHE_REMOTE_ONLY);
		state->file_handle = fs.OpenFile(state->file, flags);
		state->file_size = state->file_handle->GetFileSize();
		// AssertMaxFileSize: max file size enforced by AssertMaxFileSize in the
		// scan path is also relevant here, but file already passed it at index
		// build time; reading it again will hit the same byte count.
	}
	return std::move(state);
}

template <class OP>
void DirectLookupScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<DirectLookupGlobalState<OP>>();
	if (data.pk_lookups.empty()) {
		return;
	}
	D_ASSERT(data.pk_output_positions.size() == data.pk_lookups.size());

	auto &fs = FileSystem::GetFileSystem(context);
	auto &file = gstate.file;

	int64_t *frn_data = nullptr;
	if (gstate.file_row_number_idx != DConstants::INVALID_INDEX) {
		frn_data = FlatVector::GetDataMutable<int64_t>(output.data[gstate.file_row_number_idx]);
	}

	const idx_t count = data.pk_lookups.size();
	for (idx_t pk_idx = 0; pk_idx < count; pk_idx++) {
		// read_text/read_blob emit one row per file -> file_row_number is always 0.
		D_ASSERT(data.pk_lookups[pk_idx] == 0);
		const idx_t out_idx = data.pk_output_positions[pk_idx];
		if (frn_data) {
			frn_data[out_idx] = 0;
		}

		for (idx_t col_idx = 0; col_idx < gstate.output_to_file_col.size(); col_idx++) {
			const auto file_col = gstate.output_to_file_col[col_idx];
			if (file_col == DConstants::INVALID_INDEX) {
				continue;
			}
			try {
				switch (file_col) {
				case ReadFileBindData::FILE_NAME_COLUMN: {
					auto &vec = output.data[col_idx];
					auto name_string = StringVector::AddString(vec, file.path);
					FlatVector::GetDataMutable<string_t>(vec)[out_idx] = name_string;
				} break;
				case ReadFileBindData::FILE_CONTENT_COLUMN: {
					if (!gstate.content_stream) {
						// Lookup TF only sees real seekable files (no pipes; FIFO indexing
						// is not a meaningful workflow). file_size is known. Allocate once
						// (MemoryStream requires power-of-two capacity) and read with a
						// short-read loop.
						const idx_t cap = MaxValue<idx_t>(NextPowerOfTwo(gstate.file_size), 1);
						gstate.content_stream = make_uniq<MemoryStream>(BufferAllocator::Get(context), cap);
						idx_t total = 0;
						while (total < gstate.file_size) {
							const idx_t got = NumericCast<idx_t>(gstate.file_handle->Read(
							    gstate.content_stream->GetData() + total, gstate.file_size - total));
							if (got == 0) {
								throw IOException("Failed to read file '%s' at offset %lu, unexpected EOF", file.path,
								                  total);
							}
							total += got;
						}
						gstate.content_stream->SetPosition(total);
					}
					auto &vec = output.data[col_idx];
					auto &dest = FlatVector::GetDataMutable<string_t>(vec)[out_idx];
					const string_t raw(char_ptr_cast(gstate.content_stream->GetData()),
					                   NumericCast<uint32_t>(gstate.content_stream->GetPosition()));
					if (OP::TYPE() == LogicalType::VARCHAR) {
						if (Utf8Proc::Analyze(raw.GetData(), raw.GetSize()) == UnicodeType::INVALID) {
							throw InvalidInputException(
							    "read_text: could not read content of file '%s' as valid UTF-8 encoded text. "
							    "You may want to use read_blob instead.",
							    file.path);
						}
						dest = StringVector::AddString(vec, raw);
					} else {
						dest = StringVector::AddStringOrBlob(vec, raw);
					}
				} break;
				case ReadFileBindData::FILE_SIZE_COLUMN: {
					FlatVector::GetDataMutable<int64_t>(output.data[col_idx])[out_idx] =
					    NumericCast<int64_t>(gstate.file_size);
				} break;
				case ReadFileBindData::FILE_LAST_MODIFIED_COLUMN: {
					auto &vec = output.data[col_idx];
					try {
						const auto ts = fs.GetLastModifiedTime(*gstate.file_handle);
						FlatVector::GetDataMutable<timestamp_tz_t>(vec)[out_idx] = timestamp_tz_t(ts);
					} catch (std::exception &ex) {
						ErrorData error(ex);
						if (error.Type() == ExceptionType::CONVERSION) {
							FlatVector::SetNull(vec, out_idx, true);
						} else {
							throw;
						}
					}
				} break;
				default:
					break;
				}
			} catch (std::exception &ex) {
				ErrorData error(ex);
				if (error.Type() == ExceptionType::NOT_IMPLEMENTED) {
					FlatVector::SetNull(output.data[col_idx], out_idx, true);
				} else {
					throw;
				}
			}
		}
	}
}

} // namespace

TableFunction MakeTextLookupTableFunction() {
	TableFunction fn;
	fn.init_global = DirectLookupInitGlobal<ReadTextOperation>;
	fn.function = DirectLookupScan<ReadTextOperation>;
	return fn;
}

TableFunction MakeBlobLookupTableFunction() {
	TableFunction fn;
	fn.init_global = DirectLookupInitGlobal<ReadBlobOperation>;
	fn.function = DirectLookupScan<ReadBlobOperation>;
	return fn;
}

//------------------------------------------------------------------------------
// Register
//------------------------------------------------------------------------------

void ReadBlobFunction::RegisterFunction(BuiltinFunctions &set) {
	auto scan_fun = GetFunction<ReadBlobOperation>();
	set.AddFunction(MultiFileReader::CreateFunctionSet(scan_fun));
}

void ReadTextFunction::RegisterFunction(BuiltinFunctions &set) {
	auto scan_fun = GetFunction<ReadTextOperation>();
	set.AddFunction(MultiFileReader::CreateFunctionSet(scan_fun));
}

} // namespace duckdb
