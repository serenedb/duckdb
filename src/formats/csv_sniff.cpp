#include "duckdb/formats/csv_sniff.hpp"

#include "duckdb/formats/csv_simd.hpp"

namespace duckdb {

CsvColumnCounts::CsvColumnCounts(CSVStateMachine &state_machine_p, idx_t result_size_p)
    : state_machine(state_machine_p), column_counts(result_size_p), result_size(result_size_p) {
}

idx_t CsvColumnCounts::GetMostFrequentColumnCount() const {
	if (rows_per_column_count.empty()) {
		return 1;
	}
	idx_t column_count = 0;
	idx_t current_max = 0;
	for (auto &entry : rows_per_column_count) {
		if (entry.second > current_max || (entry.second == current_max && entry.first > column_count)) {
			current_max = entry.second;
			column_count = entry.first;
		}
	}
	return column_count;
}

CsvSniffScanner::CsvSniffScanner(shared_ptr<CSVBufferManager> buffer_manager,
                                 shared_ptr<CSVStateMachine> state_machine_p,
                                 shared_ptr<CSVErrorHandler> error_handler_p, idx_t result_size_p)
    : state_machine(std::move(state_machine_p)), error_handler(std::move(error_handler_p)), result_size(result_size_p) {
	CsvScanSource source;
	source.state_machine = state_machine;
	source.error_handler = error_handler;
	source.path = buffer_manager->GetFilePath();
	source.sniffing = true;
	const idx_t columns = state_machine->dialect_options.num_cols;
	vector<LogicalType> types(columns, LogicalType::VARCHAR);
	for (idx_t i = 0; i < columns; i++) {
		source.projection.emplace_back(i, i);
	}
	source.types = types;
	auto &context = buffer_manager->context;
	source.buffer_manager = std::move(buffer_manager);
	global = make_uniq<CsvScanGlobal>(context, state_machine->options, std::move(source));
	scan = make_uniq<CsvScan>(*global);
	scan->Start(0);
	auto &allocator = Allocator::Get(context);
	chunk.Initialize(allocator, types);
	part.Initialize(allocator, types);
}

CsvSniffScanner &CsvSniffScanner::ParseChunk() {
	chunk.Reset();
	while (chunk.size() < result_size && !scan->Done()) {
		if (chunk.size() == 0) {
			scan->Scan(chunk, result_size);
			chunk.Flatten();
			continue;
		}
		part.Reset();
		scan->Scan(part, result_size - chunk.size());
		if (part.size() > 0) {
			chunk.Append(part);
		}
	}
	ever_quoted = scan->EverQuoted();
	ever_escaped = scan->EverEscaped();
	used_unstrictness = scan->UsedUnstrictness();
	return *this;
}

CsvColumnCounter::CsvColumnCounter(shared_ptr<CSVBufferManager> buffer_manager_p,
                                   shared_ptr<CSVStateMachine> state_machine_p,
                                   shared_ptr<CSVErrorHandler> error_handler_p, idx_t result_size, idx_t start)
    : buffer_manager(std::move(buffer_manager_p)), state_machine(std::move(state_machine_p)),
      error_handler(std::move(error_handler_p)), result(*state_machine, result_size),
      lexer(RowDialect::DuckDB(CsvGrammar::For(*state_machine)), 1, state_machine->options.maximum_line_size.GetValue(),
            false) {
	if (start == DConstants::INVALID_INDEX) {
		done = true;
		start = 0;
	}
	const idx_t requested = buffer_manager->GetBufferSize();
	buffer_idx = start / requested;
	buffer_start = buffer_idx * requested;
	buffer_used = start - buffer_start;
	window_start = start;
	cur_buffer_handle = buffer_manager->GetBuffer(buffer_idx);
	lexer.Start(cursor, start, true, false);
	cursor.file_start = true;
	cursor.line_start = start;
	rows.reserve(result_size + 1);
}

idx_t CsvColumnCounter::SkipRows(CSVBufferManager &buffer_manager, const CSVStateMachine &state_machine, idx_t rows) {
	if (rows == 0) {
		return 0;
	}
	RowSkip skip(RowDialect::DuckDB(CsvGrammar::For(state_machine)), rows);
	idx_t offset = 0;
	for (idx_t index = 0;; index++) {
		auto handle = buffer_manager.GetBuffer(index);
		if (!handle) {
			return skip.Feed(nullptr, offset, offset, true) ? skip.Start() : DConstants::INVALID_INDEX;
		}
		if (skip.Feed(handle->Ptr(), offset, offset + handle->actual_size, handle->is_last_buffer)) {
			return skip.Start();
		}
		offset += handle->actual_size;
		if (handle->is_last_buffer) {
			return DConstants::INVALID_INDEX;
		}
	}
}

unique_ptr<CsvSniffScanner> CsvColumnCounter::UpgradeToStringValueScanner() const {
	return make_uniq<CsvSniffScanner>(buffer_manager, state_machine, error_handler, result.result_size);
}

bool CsvColumnCounter::FinishedFile() const {
	return finished || !cur_buffer_handle;
}

string CsvColumnCounter::LineText(idx_t begin, idx_t end) const {
	string line;
	const bool direct = !stitching;
	const char *data = direct ? cur_buffer_handle->Ptr() : window.data();
	const idx_t base = direct ? buffer_start : window_start;
	const idx_t from = MaxValue(begin, base);
	const idx_t to = MinValue(end, base + (direct ? cur_buffer_handle->actual_size : window.size()));
	if (from < to) {
		idx_t position = from;
		const char first = data[position - base];
		if (position == begin && (first == '\n' || first == '\r')) {
			position++;
		}
		line.append(data + (position - base), to - position);
	}
	CsvSanitize(line);
	return line;
}

bool CsvColumnCounter::LineTooLong(idx_t begin, idx_t end) {
	auto &options = state_machine->options;
	if (end - begin <= options.maximum_line_size.GetValue()) {
		return false;
	}
	auto line = LineText(begin, end);
	auto error = CSVError::LineSizeError(options, LinesPerBoundary(), line, begin, options.file_path);
	error_handler->Error(error);
	result.error = true;
	return true;
}

bool CsvColumnCounter::AddRows(idx_t first) {
	for (idx_t i = first; i < rows.size(); i++) {
		auto &row = rows[i];
		while (next_boundary < boundaries.size() && boundaries[next_boundary] < row.end) {
			const idx_t boundary = boundaries[next_boundary++];
			if (row.begin < boundary && LineTooLong(row.begin, boundary)) {
				return false;
			}
		}
		AddRow(row);
	}
	while (next_boundary < boundaries.size() && boundaries[next_boundary] <= cursor.pos) {
		const idx_t boundary = boundaries[next_boundary++];
		if (cursor.line_start < boundary && LineTooLong(cursor.line_start, boundary)) {
			return false;
		}
	}
	return true;
}

void CsvColumnCounter::AddRow(const RowCount &row) {
	if (row.end != DConstants::INVALID_INDEX) {
		LineTooLong(row.begin, row.end);
	}
	auto &count = result.column_counts[result.result_position];
	count.number_of_columns = row.columns;
	count.empty_lines = row.empty_lines;
	count.is_comment = (row.flags & ROW_COUNT_COMMENT) != 0;
	count.is_mid_comment = (row.flags & ROW_COUNT_MID_COMMENT) != 0;
	count.last_value_always_empty = true;
	result.rows_per_column_count[row.columns]++;
	if (!(row.flags & ROW_COUNT_EMPTY_LAST)) {
		for (idx_t index = result.result_position + 1; index-- > 0;) {
			if (!result.column_counts[index].last_value_always_empty) {
				break;
			}
			result.column_counts[index].last_value_always_empty = false;
		}
	}
	result.result_position++;
}

bool CsvColumnCounter::NextBuffer() {
	auto next = buffer_manager->GetBuffer(buffer_idx + 1);
	if (!next) {
		return false;
	}
	buffer_start += cur_buffer_handle->actual_size;
	buffer_idx++;
	buffer_used = 0;
	cur_buffer_handle = std::move(next);
	return true;
}

CsvColumnCounts &CsvColumnCounter::ParseChunk() {
	result.result_position = 0;
	if (done || FinishedFile()) {
		return result;
	}
	rows.clear();
	RowShapes shapes {rows, result.result_size};
	const idx_t need = CSV_BLOCK_BYTES + lexer.Lookahead();
	while (rows.size() < result.result_size && !cursor.invalid) {
		if (stitching && buffer_used < cur_buffer_handle->actual_size &&
		    MinValue(cursor.pos, cursor.line_start) >= buffer_start) {
			window.clear();
			stitching = false;
		}
		const bool direct = !stitching;
		const char *data = direct ? cur_buffer_handle->Ptr() : window.data();
		const idx_t base = direct ? buffer_start : window_start;
		const idx_t window_end = direct ? buffer_start + cur_buffer_handle->actual_size : window_start + window.size();
		const bool exhausted = direct || buffer_used >= cur_buffer_handle->actual_size;
		const bool at_end = exhausted && (eof || cur_buffer_handle->is_last_buffer);
		if (window_end - cursor.pos < need && !at_end) {
			if (direct) {
				const idx_t keep = MinValue(cursor.pos, cursor.line_start);
				const char *buffer = cur_buffer_handle->Ptr();
				window.assign(buffer + (keep - buffer_start), buffer + cur_buffer_handle->actual_size);
				window_start = keep;
				buffer_used = cur_buffer_handle->actual_size;
				stitching = true;
				continue;
			}
			if (exhausted) {
				if (result.error) {
					break;
				}
				if (!NextBuffer()) {
					eof = true;
					continue;
				}
				boundaries.push_back(buffer_start);
			}
			const idx_t keep = MinValue(MinValue(cursor.pos, cursor.line_start), window_end) - window_start;
			window.erase(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(keep));
			window_start += keep;
			const idx_t piece = MinValue<idx_t>(cur_buffer_handle->actual_size - buffer_used, 1 << 16);
			const char *source = cur_buffer_handle->Ptr() + buffer_used;
			window.insert(window.end(), source, source + piece);
			buffer_used += piece;
			continue;
		}
		if (!cursor.ok) {
			result.error = true;
			break;
		}
		const idx_t first = rows.size();
		lexer.CountRows(cursor, data, base, window_end, at_end, shapes);
		if (!AddRows(first)) {
			break;
		}
		if (at_end && cursor.pos >= window_end && !cursor.invalid && rows.size() < result.result_size) {
			const idx_t before = rows.size();
			lexer.FinishCount(cursor, rows);
			AddRows(before);
			finished = true;
			result.error = result.error || cursor.unterminated;
			break;
		}
	}
	if (cursor.invalid) {
		result.result_position = 0;
		result.error = true;
	}
	ever_quoted = cursor.ever_quoted;
	ever_escaped = cursor.ever_escaped;
	used_unstrictness = cursor.unstrict;
	return result;
}

} // namespace duckdb
