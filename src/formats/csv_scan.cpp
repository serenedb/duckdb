#include "duckdb/formats/csv_scan.hpp"

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/callback_async_task.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

namespace {

constexpr idx_t STITCH_BYTES = 1 << 16;
constexpr idx_t LOOKUP_BYTES = 1 << 13;

string CastMessage(const char *value, idx_t size, const string &type) {
	string message = "Could not convert string \"" + string(value, size) + "\" to '" + type + "'";
	CsvSanitize(message);
	return message;
}

template <class T>
bool CastInteger(const char *value, idx_t size, Vector &vector, idx_t row) {
	return TrySimpleIntegerCast<T>(value, size, FlatVector::GetDataMutable<T>(vector)[row], false);
}

template <class T>
bool CastUnsigned(const char *value, idx_t size, Vector &vector, idx_t row) {
	return TrySimpleIntegerCast<T, false>(value, size, FlatVector::GetDataMutable<T>(vector)[row], false);
}

template <class T>
bool CastDecimal(const char *value, idx_t size, Vector &vector, idx_t row, char separator, uint8_t width,
                 uint8_t scale) {
	auto &out = FlatVector::GetDataMutable<T>(vector)[row];
	if (separator == ',') {
		return TryDecimalStringCast<T, ','>(value, size, out, width, scale);
	}
	if (separator == '.') {
		return TryDecimalStringCast<T>(value, size, out, width, scale);
	}
	throw InvalidInputException("Decimals can only have ',' and '.' as decimal separators");
}

idx_t SkipBom(const char *data, idx_t size) {
	if (size >= 3 && data[0] == '\xEF' && data[1] == '\xBB' && data[2] == '\xBF') {
		return 3;
	}
	return 0;
}

} // namespace

bool CsvDecodesDirectly(const LogicalType &type, bool icu_loaded) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIME:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BOOLEAN:
		return true;
	case LogicalTypeId::TIMESTAMP_TZ:
		return !icu_loaded;
	case LogicalTypeId::VARCHAR:
		return !type.IsJSONType();
	default:
		return false;
	}
}

string_t CsvRemoveEscape(const char *value, idx_t size, char escape, char quote, bool strict_mode, Vector &vector) {
	auto keep = [&](idx_t pos, bool &escaped) {
		const char c = value[pos];
		if (c == escape && !escaped) {
			escaped = true;
			return false;
		}
		const bool kept = c != quote || escaped || !strict_mode;
		escaped = false;
		return kept;
	};
	idx_t length = 0;
	bool escaped = false;
	for (idx_t pos = 0; pos < size; pos++) {
		length += keep(pos, escaped);
	}
	auto result = StringVector::EmptyString(vector, length);
	auto out = result.GetDataWriteable();
	escaped = false;
	for (idx_t pos = 0, written = 0; pos < size; pos++) {
		if (keep(pos, escaped)) {
			out[written++] = value[pos];
		}
	}
	result.Finalize();
	return result;
}

void CsvSanitize(string &text) {
	text.push_back('\0');
	Utf8Proc::MakeValid(&text[0], text.size());
	text.pop_back();
}

idx_t CsvRangeBytes(const CSVReaderOptions &options) {
	constexpr idx_t ROWS_PER_RANGE = 4;
	const idx_t buffer_size = options.buffer_size_option.GetValue();
	const idx_t max_line_size = options.maximum_line_size.GetValue();
	const idx_t bytes = buffer_size / CSVBuffer::ROWS_PER_BUFFER * ROWS_PER_RANGE;
	if (bytes < max_line_size) {
		return max_line_size;
	}
	return MaxValue<idx_t>(bytes, 1);
}

static CsvScanSource FileSource(CSVFileScan &file) {
	CsvScanSource source;
	source.state_machine = file.state_machine;
	source.buffer_manager = file.buffer_manager;
	source.error_handler = file.error_handler;
	source.projection.assign(file.projection_ids.begin(), file.projection_ids.end());
	source.types = file.file_types;
	source.names = file.GetNames();
	source.path = file.GetFileName();
	source.file_row_number = file.file_row_number_idx;
	source.bytes_read = &file.bytes_read;
	return source;
}

static CsvScanSource FileSource(CSVFileScan &file, optional_ptr<TableFilterSet> filters) {
	auto source = FileSource(file);
	source.filters = filters;
	return source;
}

CsvScanGlobal::CsvScanGlobal(ClientContext &context_p, CSVFileScan &file, optional_ptr<TableFilterSet> filters_p)
    : CsvScanGlobal(context_p, file.options, FileSource(file, filters_p)) {
}

CsvScanGlobal::CsvScanGlobal(ClientContext &context_p, const CSVReaderOptions &options_p, CsvScanSource source_p)
    : context(context_p), options(options_p), source(std::move(source_p)) {
	auto &state_machine = *source.state_machine;
	grammar = CsvGrammar::For(state_machine);

	const bool icu_loaded = context.db->ExtensionIsLoaded("icu");
	for (idx_t i = 0; i < source.projection.size(); i++) {
		CsvScanColumn column;
		column.file_column = source.projection[i].first;
		column.output_column = source.projection[i].second;
		column.type = source.types[i];
		auto parse_type = column.type.IsJSONType() ? LogicalType::VARCHAR : column.type;
		if (CsvDecodesDirectly(parse_type, icu_loaded)) {
			column.parse_type = parse_type;
			column.validate_utf8 = true;
		} else {
			column.parse_type = LogicalType::VARCHAR;
			column.validate_utf8 = column.type.id() == LogicalTypeId::VARCHAR || column.type.IsNested();
		}
		column.force_not_null = i < options.force_not_null.size() && options.force_not_null[i];
		columns.push_back(std::move(column));
	}
	filtered.assign(columns.size(), false);
	if (source.filters) {
		for (auto &entry : *source.filters) {
			const idx_t output_column = entry.GetIndex();
			filters.push_back({output_column, entry.Filter()});
			for (idx_t i = 0; i < columns.size(); i++) {
				filtered[i] = filtered[i] || columns[i].output_column == output_column;
			}
		}
	}
	late_materialization = !filters.empty() && !options.store_rejects.GetValue();
	date_format = options.dialect_options.date_format.at(LogicalTypeId::DATE).GetValue();
	timestamp_format = options.dialect_options.date_format.at(LogicalTypeId::TIMESTAMP).GetValue();
	decimal_separator = options.decimal_separator[0];
	null_strings = options.null_str;
	for (auto &null_string : null_strings) {
		empty_is_null = empty_is_null || null_string.empty();
	}
	ignore_errors = options.IgnoreErrors();
	print_line = options.store_rejects.GetValue() || !options.ignore_errors.GetValue();
	if (source.sniffing) {
		skip_rows = state_machine.dialect_options.skip_rows.GetValue();
		skip_bom = skip_rows + state_machine.dialect_options.header.GetValue() == 0;
	} else {
		skip_rows = options.GetSkipRows() + options.dialect_options.header.GetValue();
		skip_bom = skip_rows == 0;
	}

	auto &manager = *source.buffer_manager;
	size = manager.file_handle->FileSize();
	buffer_size = manager.GetBufferSize();
	parallel = !source.sniffing && options.parallel && manager.HasKnownBufferRanges() && !grammar.only_crlf;
	if (!parallel) {
		return;
	}
	buffer_count = manager.KnownBufferCount();
	range_bytes = MaxValue<idx_t>(MinValue(buffer_size, CsvRangeBytes(options)), 1);
	parts = (buffer_size + range_bytes - 1) / range_bytes;
	range_count = buffer_count * parts;
	FindStart();
	first_range = MinValue((start / buffer_size) * parts + (start % buffer_size) / range_bytes, range_count);
	resolved = first_range;
	transfers.resize(range_count);
	published.assign(range_count, false);
	computing.assign(range_count, false);
	starts.assign(range_count + 1, CsvRangeStart());
	starts[first_range].line_end = start;
	starts[first_range].last_position = start;
	buffer_uses.resize(buffer_count);
}

void CsvScanGlobal::FindStart() {
	auto handle = Buffer(0);
	if (skip_rows == 0) {
		start = skip_bom ? SkipBom(handle->Ptr(), handle->actual_size) : 0;
		start_lines = 0;
		return;
	}
	CsvSkip skip(grammar, skip_rows);
	idx_t offset = 0;
	for (idx_t buffer = 1; handle; buffer++) {
		if (skip.Feed(handle->Ptr(), offset, offset + handle->actual_size) || handle->is_last_buffer) {
			break;
		}
		offset += handle->actual_size;
		handle = buffer < buffer_count ? Buffer(buffer) : nullptr;
	}
	start = MinValue(skip.Start(options), size);
	start_lines = skip.Lines();
}

idx_t CsvScanGlobal::Read(char *data, idx_t count, idx_t offset) const {
	auto &manager = *source.buffer_manager;
	if (manager.HasKnownBufferRanges()) {
		const idx_t available = offset < size ? MinValue(count, size - offset) : 0;
		if (available > 0) {
			manager.file_handle->ReadAt(data, available, offset);
		}
		return available;
	}
	const idx_t buffer_bytes = manager.GetBufferSize();
	idx_t read = 0;
	while (read < count) {
		const idx_t position = offset + read;
		auto handle = manager.GetBuffer(position / buffer_bytes);
		const idx_t within = position % buffer_bytes;
		if (!handle || within >= handle->actual_size) {
			break;
		}
		const idx_t piece = MinValue(count - read, handle->actual_size - within);
		memcpy(data + read, handle->Ptr() + within, piece);
		read += piece;
	}
	return read;
}

shared_ptr<CsvBufferUse> CsvScanGlobal::Use(idx_t buffer) {
	auto &use = buffer_uses[buffer];
	if (!use) {
		use = make_shared_ptr<CsvBufferUse>(*source.buffer_manager, buffer);
	}
	return use;
}

bool CsvScanGlobal::Claim(idx_t &range, vector<shared_ptr<CsvBufferUse>> &uses) {
	const absl::MutexLock guard(&lock);
	uses.clear();
	if (!parallel) {
		if (sequential_claimed) {
			return false;
		}
		sequential_claimed = true;
		range = 0;
		return true;
	}
	if (next_range >= range_count) {
		return false;
	}
	range = next_range++;
	const idx_t buffer = RangeBuffer(range);
	uses.push_back(Use(buffer));
	if (LastPart(range)) {
		if (buffer + 1 < buffer_count) {
			uses.push_back(Use(buffer + 1));
		}
		buffer_uses[buffer].reset();
	}
	return true;
}

vector<unique_ptr<AsyncTask>> CsvScanGlobal::LoadTasks(idx_t range) const {
	vector<unique_ptr<AsyncTask>> tasks;
	if (!parallel || RangeBegin(range) >= RangeEnd(range)) {
		return tasks;
	}
	auto manager = source.buffer_manager;
	auto load = [&](idx_t buffer) {
		shared_ptr<CSVBufferHandle> handle;
		if (manager->GetBufferResidency(buffer, handle) != CSVBufferResidency::NEEDS_LOAD) {
			return;
		}
		tasks.push_back(make_uniq<CallbackAsyncTask>([manager, buffer] { manager->GetBuffer(buffer); },
		                                             manager->KnownBufferSize(buffer)));
	};
	const idx_t buffer = RangeBuffer(range);
	load(buffer);
	if (LastPart(range) && buffer + 1 < buffer_count) {
		load(buffer + 1);
	}
	return tasks;
}

bool CsvScanGlobal::FinishRange(idx_t range, idx_t lines) {
	source.error_handler->Insert(range, lines);
	const absl::MutexLock guard(&lock);
	finished_ranges++;
	return finished_ranges == MaxValue<idx_t>(parallel ? range_count : 1, 1);
}

bool CsvScanGlobal::AllFinished() const {
	const absl::MutexLock guard(&lock);
	return finished_ranges == (parallel ? range_count : 1);
}

void CsvScanGlobal::Publish(idx_t range, const CsvRangeTransfer &transfer) {
	const absl::MutexLock guard(&lock);
	if (published[range]) {
		return;
	}
	transfers[range] = transfer;
	published[range] = true;
	while (resolved < range_count && published[resolved]) {
		auto &current = starts[resolved];
		auto &next = starts[resolved + 1];
		next = transfers[resolved][static_cast<uint8_t>(current.states[1])].end;
		next.Inherit(current);
		resolved++;
	}
}

void CsvScanGlobal::ComputeTransfer(idx_t range) {
	const idx_t begin = RangeBegin(range);
	const idx_t end = RangeEnd(range);
	CsvRangeTransfer transfer;
	if (begin < end) {
		const idx_t buffer = RangeBuffer(range);
		auto handle = Buffer(buffer);
		CsvTransfer(grammar, handle->Ptr(), buffer * buffer_size, begin, end, transfer);
	}
	Publish(range, transfer);
}

CsvRangeStart CsvScanGlobal::StartState(idx_t range) {
	while (true) {
		idx_t missing;
		{
			const absl::MutexLock guard(&lock);
			auto ready = [&]() {
				return resolved >= range || !computing[resolved];
			};
			lock.Await(absl::Condition(&ready));
			if (resolved >= range) {
				return starts[range];
			}
			missing = resolved;
			computing[missing] = true;
		}
		ComputeTransfer(missing);
	}
}

void CsvScanGlobal::StartComputing(idx_t range) {
	const absl::MutexLock guard(&lock);
	computing[range] = true;
}

CsvScan::CsvScan(CsvScanGlobal &global_p)
    : global(global_p), source(global_p.source), options(global_p.options), grammar(global_p.grammar) {
	splitter = make_uniq<CsvSplitter>(grammar, source.state_machine->dialect_options.num_cols, STANDARD_VECTOR_SIZE);
	value_errors.resize(STANDARD_VECTOR_SIZE);
	failed.resize(STANDARD_VECTOR_SIZE);
	dropped.resize(STANDARD_VECTOR_SIZE);
	selected.resize(STANDARD_VECTOR_SIZE);
	keep.Initialize(STANDARD_VECTOR_SIZE);
	for (auto &filter : global.filters) {
		filter_states.push_back(TableFilterState::Initialize(global.context, filter.filter.get()));
	}
	auto &allocator = Allocator::Get(global.context);
	vector<LogicalType> parse_types(global.columns.size(), LogicalType::VARCHAR);
	if (!parse_types.empty()) {
		parse_chunk.Initialize(allocator, parse_types);
	}
	scratch_chunk.Initialize(allocator, vector<LogicalType> {LogicalType::VARCHAR});
}

void CsvScan::Start(idx_t range_p, vector<shared_ptr<CsvBufferUse>> uses_p) {
	range = range_p;
	uses = std::move(uses_p);
	done = false;
	started = false;
	reported = false;
	no_rows = false;
	bytes.reset();
	base = 0;
	pending.reset();
	pending_start = 0;
	pending_used = 0;
	next_buffer = 0;
	next_buffer_start = 0;
	stitch_bytes = 0;
	last_buffer = false;
	range_begin = 0;
	range_end = NumericLimits<idx_t>::Maximum();
}

void CsvScan::View(shared_ptr<CSVBufferHandle> handle, idx_t position) {
	auto view = make_shared_ptr<CsvBytes>();
	view->data = handle->Ptr();
	view->size = handle->actual_size;
	view->buffer = std::move(handle);
	bytes = std::move(view);
	base = position;
	stitch_bytes = 0;
}

bool CsvScan::ReadMore(idx_t keep) {
	if (!pending) {
		if (last_buffer) {
			return false;
		}
		auto &manager = *source.buffer_manager;
		shared_ptr<CSVBufferHandle> handle;
		if (!global.parallel) {
			handle = manager.GetBuffer(next_buffer);
		} else if (next_buffer < global.buffer_count) {
			handle = global.Buffer(next_buffer);
		}
		if (!handle) {
			last_buffer = true;
			return false;
		}
		if (!global.parallel && !source.sniffing) {
			*source.bytes_read += handle->actual_size;
			if (next_buffer > 0) {
				manager.ResetBuffer(next_buffer - 1);
			}
			if (handle->is_last_buffer) {
				manager.ResetBuffer(next_buffer);
			}
		}
		pending = std::move(handle);
		pending_start = next_buffer_start;
		pending_used = 0;
		next_buffer++;
		next_buffer_start += pending->actual_size;
	}
	const idx_t span_end = bytes ? base + bytes->size : pending_start;
	keep = bytes ? MinValue(MaxValue(keep, base), span_end) : pending_start;
	if (keep >= pending_start) {
		const bool is_last = pending->is_last_buffer;
		View(std::move(pending), pending_start);
		last_buffer = is_last;
		return true;
	}
	stitch_bytes = MaxValue(stitch_bytes * 2, STITCH_BYTES);
	const idx_t piece = MinValue(stitch_bytes, pending->actual_size - pending_used);
	const idx_t kept = span_end - keep;
	auto stitched = make_shared_ptr<CsvBytes>();
	stitched->owned = make_unsafe_uniq_array_uninitialized<char>(MaxValue<idx_t>(kept + piece, 1));
	memcpy(stitched->owned.get(), bytes->data + (keep - base), kept);
	memcpy(stitched->owned.get() + kept, pending->Ptr() + pending_used, piece);
	stitched->data = stitched->owned.get();
	stitched->size = kept + piece;
	bytes = std::move(stitched);
	base = keep;
	pending_used += piece;
	if (pending_used == pending->actual_size) {
		last_buffer = pending->is_last_buffer;
		pending.reset();
	}
	return true;
}

void CsvScan::SkipRows() {
	ReadMore(0);
	if (global.skip_rows == 0) {
		splitter->Reset(bytes && global.skip_bom ? SkipBom(bytes->data, bytes->size) : 0, 0);
		return;
	}
	CsvSkip skip(grammar, global.skip_rows);
	bool skipped = false;
	while (bytes) {
		skipped = skip.Feed(bytes->data, base, base + bytes->size);
		if (skipped || !ReadMore(base + bytes->size)) {
			break;
		}
	}
	if (!skipped && source.sniffing) {
		bytes.reset();
		pending.reset();
		next_buffer = 0;
		next_buffer_start = 0;
		last_buffer = false;
		ReadMore(0);
		splitter->Reset(0, 0);
		return;
	}
	splitter->Reset(skip.Start(options), skip.Lines());
}

bool CsvScan::ReadRange() {
	range_begin = global.RangeBegin(range);
	range_end = global.RangeEnd(range);
	if (range_begin >= range_end) {
		return false;
	}
	global.StartComputing(range);
	const idx_t buffer = global.RangeBuffer(range);
	auto handle = global.Buffer(buffer);
	last_buffer = handle->is_last_buffer;
	View(std::move(handle), buffer * global.buffer_size);
	next_buffer = buffer + 1;
	next_buffer_start = next_buffer * global.buffer_size;
	CsvRangeTransfer transfer;
	CsvTransfer(grammar, bytes->data, base, range_begin, range_end, transfer);
	global.Publish(range, transfer);
	const auto start = global.StartState(range);
	if (range_begin == global.start) {
		splitter->Reset(range_begin, global.start_lines);
		return true;
	}
	const auto state = start.states[1];
	if (state == CSVState::RECORD_SEPARATOR || state == CSVState::CARRIAGE_RETURN) {
		splitter->Reset(range_begin, 0, start);
		return true;
	}
	auto &entry = transfer[static_cast<uint8_t>(state)];
	if (entry.first_boundary == DConstants::INVALID_INDEX) {
		return false;
	}
	auto boundary = entry.boundary;
	boundary.Inherit(start);
	splitter->Reset(entry.first_boundary, 0, boundary);
	return true;
}

idx_t CsvScan::Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output) {
	D_ASSERT(offsets.size() <= splitter->Capacity());
	auto arena = make_shared_ptr<CsvBytes>();
	idx_t capacity = 0;
	idx_t window_begin = 0;
	idx_t window_end = 0;
	idx_t window_offset = 0;
	bool window_eof = false;
	auto read = [&](idx_t from, idx_t want) {
		if (arena->size + want > capacity) {
			capacity = MaxValue(arena->size + want, capacity * 2);
			auto grown = make_unsafe_uniq_array_uninitialized<char>(capacity);
			if (arena->size) {
				memcpy(grown.get(), arena->owned.get(), arena->size);
			}
			arena->owned = std::move(grown);
			arena->data = arena->owned.get();
		}
		const idx_t got = global.Read(arena->owned.get() + arena->size, want, from);
		arena->size += got;
		window_end = from + got;
		window_eof = got < want;
	};
	splitter->ClearRows();
	lookup_offsets = offsets;
	lookup_rows.clear();
	idx_t count = 0;
	for (idx_t i = 0; i < offsets.size(); i++) {
		const auto offset = static_cast<idx_t>(offsets[i]);
		if (offsets[i] < 0 || offset < global.start) {
			continue;
		}
		const idx_t first = offset > global.start ? offset - 1 : offset;
		if (first < window_begin || offset >= window_end) {
			if (window_end > window_begin && !window_eof && first >= window_end && first - window_end < LOOKUP_BYTES) {
				read(window_end, offset + LOOKUP_BYTES - window_end);
			} else {
				window_offset = arena->size;
				window_begin = first;
				read(first, offset + LOOKUP_BYTES - first);
			}
			if (offset >= window_end) {
				continue;
			}
		}
		const idx_t position = window_offset + (offset - window_begin);
		CsvRangeStart start;
		start.line_end = position;
		start.last_position = position;
		if (offset > global.start) {
			const bool carriage_return = arena->data[position - 1] == '\r' && grammar.carry_on_newline;
			start.states[1] = carriage_return ? CSVState::CARRIAGE_RETURN : CSVState::RECORD_SEPARATOR;
			start.line_end = position - 1;
			start.last_position = position + carriage_return;
		}
		splitter->Reset(position, 0, start);
		idx_t grow = LOOKUP_BYTES;
		while (true) {
			const idx_t arena_end = window_offset + (window_end - window_begin);
			const auto rows = splitter->Split(arena->data, 0, arena_end, count + 1, window_eof,
			                                  NumericLimits<idx_t>::Maximum(), true);
			if (rows > count || splitter->Finished() || window_eof) {
				count = rows;
				break;
			}
			read(window_end, grow);
			grow *= 2;
		}
		while (lookup_rows.size() < count) {
			lookup_rows.push_back(i);
		}
	}
	bytes = arena;
	base = 0;
	idx_t found = 0;
	if (count > 0) {
		Decode(count, output);
		for (idx_t row = 0; row < count; row++) {
			if (!failed[row] && !dropped[row] && selected[row]) {
				survivors[found++] = lookup_rows[row];
			}
		}
	}
	output.SetCardinality(found);
	lookup_offsets = {};
	bytes.reset();
	return found;
}

bool CsvScan::IsNull(const CsvField &field, const char *value, idx_t size) const {
	const bool quoted = field.flags & CSV_FIELD_QUOTED;
	if (quoted && !options.allow_quoted_nulls) {
		return false;
	}
	const bool escaped_null = !grammar.strict_mode && (field.flags & CSV_FIELD_ESCAPED) && !quoted && size == 1;
	for (auto &null_string : global.null_strings) {
		if (null_string.size() == 2 && null_string[0] == grammar.escape) {
			if (escaped_null && null_string[1] == value[0]) {
				return true;
			}
		} else if (size == null_string.size() && !escaped_null && memcmp(null_string.c_str(), value, size) == 0) {
			return true;
		}
	}
	return false;
}

bool CsvScan::DecodeValue(const CsvScanColumn &column, const CsvField &field, Vector &vector, idx_t row,
                          CsvRowError &error) {
	const char *value = bytes->data + field.offset;
	idx_t size = field.size;
	const auto parse_id = column.parse_type.id();
	string_t unescaped;
	if (field.flags & CSV_FIELD_ESCAPED) {
		if (parse_id != LogicalTypeId::VARCHAR) {
			error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start,
			         global.ignore_errors ? string() : CastMessage(value, size, LogicalTypeIdToString(parse_id))};
			return false;
		}
		unescaped = CsvRemoveEscape(value, size, grammar.escape, grammar.quote, grammar.strict_mode, vector);
		value = unescaped.GetData();
		size = unescaped.GetSize();
	}
	if (IsNull(field, value, size)) {
		if (column.force_not_null) {
			if (parse_id != LogicalTypeId::VARCHAR) {
				error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start, string()};
				return false;
			}
			FlatVector::GetDataMutable<string_t>(vector)[row] = string_t();
		} else {
			FlatVector::SetNull(vector, row, true);
		}
		return true;
	}
	string thousands;
	if ((LogicalType::IsNumeric(parse_id) || parse_id == LogicalTypeId::BIGNUM) &&
	    options.thousands_separator != '\0') {
		thousands = CsvStripSeparator(value, size, options.thousands_separator);
		value = thousands.c_str();
		size = thousands.size();
	}
	const char decimal_separator = global.decimal_separator;
	bool success = true;
	switch (parse_id) {
	case LogicalTypeId::BOOLEAN:
		success = TryCastStringBool(value, size, FlatVector::GetDataMutable<bool>(vector)[row], false);
		break;
	case LogicalTypeId::TINYINT:
		success = CastInteger<int8_t>(value, size, vector, row);
		break;
	case LogicalTypeId::SMALLINT:
		success = CastInteger<int16_t>(value, size, vector, row);
		break;
	case LogicalTypeId::INTEGER:
		success = CastInteger<int32_t>(value, size, vector, row);
		break;
	case LogicalTypeId::BIGINT:
		success = CastInteger<int64_t>(value, size, vector, row);
		break;
	case LogicalTypeId::UTINYINT:
		success = CastUnsigned<uint8_t>(value, size, vector, row);
		break;
	case LogicalTypeId::USMALLINT:
		success = CastUnsigned<uint16_t>(value, size, vector, row);
		break;
	case LogicalTypeId::UINTEGER:
		success = CastUnsigned<uint32_t>(value, size, vector, row);
		break;
	case LogicalTypeId::UBIGINT:
		success = CastUnsigned<uint64_t>(value, size, vector, row);
		break;
	case LogicalTypeId::HUGEINT:
		success = TryCast::Operation<string_t, hugeint_t>(string_t(value, NumericCast<uint32_t>(size)),
		                                                  FlatVector::GetDataMutable<hugeint_t>(vector)[row], false);
		break;
	case LogicalTypeId::UHUGEINT:
		success = TryCast::Operation<string_t, uhugeint_t>(string_t(value, NumericCast<uint32_t>(size)),
		                                                   FlatVector::GetDataMutable<uhugeint_t>(vector)[row], false);
		break;
	case LogicalTypeId::BIGNUM: {
		while (size > 0 && StringUtil::CharacterIsSpace(*value)) {
			value++;
			size--;
		}
		while (size > 0 && StringUtil::CharacterIsSpace(value[size - 1])) {
			size--;
		}
		try {
			auto bignum = Bignum::VarcharToBignum(string_t(value, NumericCast<uint32_t>(size)));
			FlatVector::GetDataMutable<string_t>(vector)[row] =
			    StringVector::AddStringOrBlob(vector, bignum.data(), bignum.size());
		} catch (const ConversionException &) {
			success = false;
		}
		break;
	}
	case LogicalTypeId::DOUBLE:
		success = TryDoubleCast<double>(value, size, FlatVector::GetDataMutable<double>(vector)[row], false,
		                                decimal_separator);
		break;
	case LogicalTypeId::FLOAT:
		success =
		    TryDoubleCast<float>(value, size, FlatVector::GetDataMutable<float>(vector)[row], false, decimal_separator);
		break;
	case LogicalTypeId::DATE: {
		auto &out = FlatVector::GetDataMutable<date_t>(vector)[row];
		if (!global.date_format.Empty()) {
			success = global.date_format.TryParseDate(value, size, out);
		} else {
			idx_t pos;
			bool special;
			success = Date::TryConvertDate(value, size, pos, out, special, false) == DateCastResult::SUCCESS;
		}
		break;
	}
	case LogicalTypeId::TIME: {
		idx_t pos;
		success = Time::TryConvertTime(value, size, pos, FlatVector::GetDataMutable<dtime_t>(vector)[row], false);
		break;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto &out = FlatVector::GetDataMutable<timestamp_t>(vector)[row];
		if (!global.timestamp_format.Empty()) {
			success = global.timestamp_format.TryParseTimestamp(value, size, out);
		} else {
			success = Timestamp::TryConvertTimestamp(value, size, out, parse_id == LogicalTypeId::TIMESTAMP_TZ) ==
			          TimestampCastResult::SUCCESS;
		}
		break;
	}
	case LogicalTypeId::DECIMAL: {
		uint8_t width;
		uint8_t scale;
		column.parse_type.GetDecimalProperties(width, scale);
		switch (column.parse_type.InternalType()) {
		case PhysicalType::INT16:
			success = CastDecimal<int16_t>(value, size, vector, row, decimal_separator, width, scale);
			break;
		case PhysicalType::INT32:
			success = CastDecimal<int32_t>(value, size, vector, row, decimal_separator, width, scale);
			break;
		case PhysicalType::INT64:
			success = CastDecimal<int64_t>(value, size, vector, row, decimal_separator, width, scale);
			break;
		case PhysicalType::INT128:
			success = CastDecimal<hugeint_t>(value, size, vector, row, decimal_separator, width, scale);
			break;
		default:
			throw InternalException("Invalid Physical Type for Decimal Value. Physical Type: " +
			                        TypeIdToString(column.parse_type.InternalType()));
		}
		break;
	}
	default: {
		auto &out = FlatVector::GetDataMutable<string_t>(vector)[row];
		if (column.validate_utf8 && !Utf8Proc::IsValid(value, UnsafeNumericCast<uint32_t>(size))) {
			out = string_t();
			error = {CSVErrorType::INVALID_ENCODING, column.file_column, base + field.start, string()};
			return false;
		}
		if ((field.flags & CSV_FIELD_ESCAPED) || !thousands.empty()) {
			out = StringVector::AddStringOrBlob(vector, value, size);
		} else {
			out = string_t(value, UnsafeNumericCast<uint32_t>(size));
		}
		break;
	}
	}
	if (!success) {
		error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start,
		         global.ignore_errors ? string() : CastMessage(value, size, LogicalTypeIdToString(parse_id))};
		return false;
	}
	return true;
}

string CsvScan::LineText(const CsvRow &row, bool &first_nl) const {
	if (row.begin < base) {
		first_nl = true;
	}
	if (!global.print_line || row.begin == row.end || row.end <= base) {
		return string();
	}
	const char *data = bytes->data;
	idx_t start = 0;
	if (row.begin >= base) {
		start = row.begin - base;
		first_nl = data[start] == '\n' || data[start] == '\r';
		start += first_nl;
	}
	const idx_t stop = MinValue(row.end - base, bytes->size);
	if (start >= stop) {
		return string();
	}
	string line(data + start, stop - start);
	CsvSanitize(line);
	return line;
}

void CsvScan::ReportRow(idx_t row_idx, vector<CsvRowError> &errors) {
	if (source.sniffing) {
		for (auto &error : errors) {
			if (error.type != CSVErrorType::INVALID_ENCODING) {
				continue;
			}
			if (options.ignore_errors.GetValue()) {
				return;
			}
			auto encoding = std::move(error);
			errors.clear();
			errors.push_back(std::move(encoding));
			break;
		}
	}
	auto &row = splitter->Row(row_idx);
	bool first_nl = false;
	auto line = LineText(row, first_nl);
	const LinesPerBoundary lines(range, row.lines);
	const auto row_position = row.begin + first_nl;
	const auto &names = source.names;
	const String path(source.path);
	for (auto &error : errors) {
		const optional_idx error_position(error.position == row.begin ? error.position + first_nl : error.position);
		CSVError csv_error;
		switch (error.type) {
		case CSVErrorType::TOO_MANY_COLUMNS:
		case CSVErrorType::TOO_FEW_COLUMNS:
			csv_error = CSVError::IncorrectColumnAmountError(options, error.column, lines, line, row_position,
			                                                 error_position, path);
			break;
		case CSVErrorType::INVALID_ENCODING:
			csv_error = CSVError::InvalidUTF8(options, error.column, lines, line, row_position, error_position, path);
			break;
		case CSVErrorType::UNTERMINATED_QUOTES:
			csv_error = CSVError::UnterminatedQuotesError(options, error.column, lines, line, row_position,
			                                              error_position, path);
			break;
		case CSVErrorType::CAST_ERROR: {
			string column_name;
			LogicalTypeId type_id = LogicalTypeId::INVALID;
			if (error.column < names.size()) {
				column_name = names[error.column];
			}
			for (auto &column : global.columns) {
				if (column.file_column == error.column) {
					type_id = column.parse_type.id();
				}
			}
			csv_error = CSVError::CastError(options, column_name, error.message, error.column, line, lines,
			                                row_position, error_position, type_id, path);
			break;
		}
		case CSVErrorType::MAXIMUM_LINE_SIZE:
			csv_error = CSVError::LineSizeError(options, lines, line, row_position, path);
			break;
		case CSVErrorType::INVALID_STATE:
			csv_error = CSVError::InvalidState(options, error.column, lines, line, row_position, error_position, path);
			break;
		default:
			throw InvalidInputException("CSV Error not allowed when inserting row");
		}
		const bool force = error.type == CSVErrorType::INVALID_STATE ||
		                   (source.sniffing && error.type == CSVErrorType::INVALID_ENCODING);
		Report(row_idx, csv_error, force);
	}
}

void CsvScan::Report(idx_t row, CSVError &error, bool force) {
	if (lookup_offsets.empty()) {
		source.error_handler->Error(error, force);
		return;
	}
	throw InvalidInputException("Could not read the CSV row at byte %llu of \"%s\": %s",
	                            lookup_offsets[lookup_rows[row]], source.path, error.error_message);
}

bool CsvScan::NeedsFlushCast(const CsvScanColumn &column) {
	auto &type = column.type;
	return type.IsJSONType() ||
	       (type.id() != LogicalTypeId::VARCHAR && column.parse_type.id() == LogicalTypeId::VARCHAR);
}

void CsvScan::FlushCasts(idx_t count, DataChunk &output, bool late, bool filter_pass) {
	const auto &names = source.names;
	const String path(source.path);
	for (idx_t i = 0; i < global.columns.size(); i++) {
		auto &column = global.columns[i];
		if (!NeedsFlushCast(column) || column.output_column >= output.ColumnCount()) {
			continue;
		}
		if (late && global.filtered[i] != filter_pass) {
			continue;
		}
		auto &parse = parse_chunk.data[i];
		auto &result = output.data[column.output_column];
		string error_message;
		if (VectorOperations::TryCast(global.context, parse, result, count, &error_message, false, true)) {
			continue;
		}
		auto &result_validity = FlatVector::Validity(result);
		auto &parse_validity = FlatVector::Validity(parse);
		bool first = true;
		const idx_t lines_now = splitter->LinesRead();
		for (idx_t row = 0; row < count; row++) {
			if (dropped[row] || result_validity.RowIsValid(row) || !parse_validity.RowIsValid(row)) {
				continue;
			}
			failed[row] = true;
			if (!global.ignore_errors) {
				auto &csv_row = splitter->Row(row);
				bool first_nl = false;
				auto line = LineText(csv_row, first_nl);
				auto value = FlatVector::GetData<string_t>(parse)[row];
				auto message = CastMessage(value.GetData(), value.GetSize(),
				                           first ? column.type.ToString() : LogicalTypeIdToString(column.type.id()));
				idx_t row_position = csv_row.begin + first_nl;
				if (first && csv_row.begin == csv_row.end) {
					row_position = 0;
				}
				const LinesPerBoundary lines(range, lines_now - count + row);
				auto csv_error =
				    CSVError::CastError(options, names[column.file_column], message, column.file_column, line, lines,
				                        row_position, optional_idx::Invalid(), column.type.id(), path);
				Report(row, csv_error, false);
			}
			first = false;
		}
	}
}

void CsvScan::RowErrors(idx_t row_idx, vector<CsvRowError> &row_value_errors, vector<CsvRowError> &errors) {
	auto &row = splitter->Row(row_idx);
	const idx_t num_cols = splitter->Columns();
	auto &events = splitter->Events();
	auto &extras = splitter->Extras();
	idx_t event = event_cursor;
	while (event < events.size() && events[event].row < row_idx) {
		event++;
	}
	idx_t value = 0;
	auto add_events_until = [&](idx_t column) {
		while (event < events.size() && events[event].row == row_idx && events[event].column <= column) {
			auto &e = events[event];
			errors.push_back({e.kind == CsvRowEventKind::UNTERMINATED_QUOTES ? CSVErrorType::UNTERMINATED_QUOTES
			                                                                 : CSVErrorType::INVALID_STATE,
			                  e.column, e.position, string()});
			event++;
		}
	};
	for (idx_t column = 0; column < MinValue<idx_t>(row.field_count, num_cols); column++) {
		add_events_until(column);
		while (value < row_value_errors.size() && row_value_errors[value].column == column) {
			errors.push_back(std::move(row_value_errors[value++]));
		}
	}
	if (row.field_count > num_cols && !grammar.strict_mode) {
		used_unstrictness = true;
	}
	if (row.field_count > num_cols && grammar.strict_mode) {
		while (extra_cursor < extras.size() && extras[extra_cursor].row < row_idx) {
			extra_cursor++;
		}
		for (idx_t extra = extra_cursor; extra < extras.size() && extras[extra].row == row_idx; extra++) {
			auto &field = extras[extra].field;
			add_events_until(extras[extra].column);
			if (extras[extra].column == num_cols && (!(field.flags & CSV_FIELD_QUOTED) || options.allow_quoted_nulls)) {
				const char *data = bytes->data + field.offset;
				bool is_null = false;
				for (auto &null_string : global.null_strings) {
					is_null = is_null ||
					          (field.size == null_string.size() && memcmp(null_string.c_str(), data, field.size) == 0);
				}
				if (is_null) {
					continue;
				}
			}
			errors.push_back({CSVErrorType::TOO_MANY_COLUMNS, extras[extra].column, base + field.start, string()});
		}
	}
	add_events_until(NumericLimits<idx_t>::Maximum());
	if (row.flags & CSV_ROW_UNFINISHED) {
		return;
	}
	if (row.end - row.begin > options.maximum_line_size.GetValue()) {
		errors.push_back({CSVErrorType::MAXIMUM_LINE_SIZE, 1, row.last_position, string()});
	}
	if (!options.null_padding) {
		for (idx_t column = row.field_count; column < num_cols; column++) {
			errors.push_back({CSVErrorType::TOO_FEW_COLUMNS, column - 1, row.last_position, string()});
		}
	}
}

Vector &CsvScan::Target(idx_t column, DataChunk &output) {
	auto &scan_column = global.columns[column];
	if (NeedsFlushCast(scan_column)) {
		return parse_chunk.data[column];
	}
	if (scan_column.output_column < output.ColumnCount()) {
		return output.data[scan_column.output_column];
	}
	return scratch_chunk.data[0];
}

void CsvScan::DecodeColumn(idx_t column_idx, idx_t count, DataChunk &output, bool selected_only) {
	auto &column = global.columns[column_idx];
	auto &vector = Target(column_idx, output);
	if (vector.GetType().InternalType() == PhysicalType::VARCHAR) {
		StringVector::AddAuxiliaryData(vector, make_uniq<CsvBytesHolder>(bytes));
	}
	const CsvField *fields = splitter->Column(column.file_column);
	for (idx_t row = 0; row < count; row++) {
		if (selected_only && !selected[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		auto &csv_row = splitter->Row(row);
		if (csv_row.flags & CSV_ROW_EMPTY_LINE) {
			if (!global.empty_is_null) {
				dropped[row] = true;
				FlatVector::SetNull(vector, row, true);
			} else if (!options.force_not_null.empty() && options.force_not_null[0] &&
			           vector.GetType().InternalType() == PhysicalType::VARCHAR) {
				FlatVector::GetDataMutable<string_t>(vector)[row] = string_t();
			} else {
				FlatVector::SetNull(vector, row, true);
			}
			continue;
		}
		if (column.file_column >= csv_row.field_count) {
			const bool padded_empty = options.null_padding && column.file_column < options.force_not_null.size() &&
			                          options.force_not_null[column.file_column] &&
			                          vector.GetType().InternalType() == PhysicalType::VARCHAR;
			if (padded_empty) {
				FlatVector::GetDataMutable<string_t>(vector)[row] = string_t();
			} else {
				FlatVector::SetNull(vector, row, true);
			}
			continue;
		}
		CsvRowError error;
		if (!DecodeValue(column, fields[row], vector, row, error)) {
			FlatVector::SetNull(vector, row, true);
			value_errors[row].push_back(std::move(error));
		}
	}
}

void CsvScan::FillRowNumbers(idx_t count, DataChunk &output) {
	if (source.file_row_number == DConstants::INVALID_INDEX || source.file_row_number >= output.ColumnCount()) {
		return;
	}
	auto data = FlatVector::GetDataMutable<int64_t>(output.data[source.file_row_number]);
	for (idx_t row = 0; row < count; row++) {
		data[row] =
		    lookup_offsets.empty() ? static_cast<int64_t>(splitter->Row(row).start) : lookup_offsets[lookup_rows[row]];
	}
}

void CsvScan::Filter(idx_t count, DataChunk &output) {
	idx_t approved = count;
	filter_selection.Initialize(nullptr);
	for (idx_t i = 0; i < global.filters.size() && approved > 0; i++) {
		auto &vector = output.data[global.filters[i].output_column];
		FlatVector::SetSize(vector, count_t(count));
		ColumnSegment::FilterSelection(filter_selection, vector, *filter_states[i], count, approved);
	}
	for (idx_t row = 0; row < count; row++) {
		selected[row] = false;
	}
	for (idx_t i = 0; i < approved; i++) {
		selected[filter_selection.get_index(i)] = true;
	}
}

void CsvScan::Decode(idx_t count, DataChunk &output) {
	output.Reset();
	parse_chunk.Reset();
	scratch_chunk.Reset();
	const idx_t num_cols = splitter->Columns();
	const bool filters = !global.filters.empty();
	const bool late = global.late_materialization;
	for (idx_t row = 0; row < count; row++) {
		failed[row] = false;
		dropped[row] = false;
		selected[row] = true;
		value_errors[row].clear();
	}
	for (idx_t i = 0; i < global.columns.size(); i++) {
		if (!late || global.filtered[i]) {
			DecodeColumn(i, count, output, false);
		}
	}
	if (late) {
		FlushCasts(count, output, true, true);
		FillRowNumbers(count, output);
		Filter(count, output);
		for (idx_t i = 0; i < global.columns.size(); i++) {
			if (!global.filtered[i]) {
				DecodeColumn(i, count, output, true);
			}
		}
	}
	event_cursor = 0;
	extra_cursor = 0;
	for (idx_t row = 0; row < count; row++) {
		auto &csv_row = splitter->Row(row);
		if (dropped[row] || (csv_row.flags & CSV_ROW_EMPTY_LINE)) {
			continue;
		}
		const bool may_fail = !value_errors[row].empty() || (csv_row.flags & CSV_ROW_EVENTS) ||
		                      csv_row.field_count != num_cols ||
		                      csv_row.end - csv_row.begin > options.maximum_line_size.GetValue();
		if (!may_fail) {
			continue;
		}
		row_errors.clear();
		RowErrors(row, value_errors[row], row_errors);
		if (row_errors.empty()) {
			continue;
		}
		failed[row] = true;
		if (!global.ignore_errors) {
			ReportRow(row, row_errors);
		}
	}
	FlushCasts(count, output, late, false);
	if (!late) {
		FillRowNumbers(count, output);
		if (filters) {
			Filter(count, output);
		}
	}
	idx_t kept = 0;
	for (idx_t row = 0; row < count; row++) {
		if (!failed[row] && !dropped[row] && selected[row]) {
			keep.set_index(kept++, row);
		}
	}
	output.SetChildCardinality(count);
	if (kept != count) {
		output.Slice(keep, kept);
	}
	output.SetChildCardinality(kept);
}

bool CsvScan::Scan(DataChunk &output, idx_t max_rows) {
	source.error_handler->ErrorIfNeeded();
	if (!started) {
		started = true;
		if (global.parallel) {
			if (!ReadRange()) {
				done = true;
				no_rows = true;
			}
		} else {
			SkipRows();
		}
	}
	while (!done) {
		idx_t count = 0;
		if (bytes) {
			count = splitter->Split(bytes->data, base, base + bytes->size, max_rows, last_buffer, range_end);
		}
		if (count > 0) {
			Decode(count, output);
			if (output.size() > 0) {
				return false;
			}
			continue;
		}
		if (splitter->Finished() || last_buffer) {
			done = true;
			break;
		}
		ReadMore(splitter->KeepFrom());
	}
	output.Reset();
	if (reported) {
		return false;
	}
	reported = true;
	if (global.parallel && range_begin < range_end) {
		*source.bytes_read += range_end - range_begin;
	}
	bytes.reset();
	pending.reset();
	uses.clear();
	if (source.sniffing) {
		return true;
	}
	return global.FinishRange(range, no_rows ? 0 : splitter->LinesRead());
}

} // namespace duckdb
