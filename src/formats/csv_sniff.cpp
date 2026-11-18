#include "duckdb/formats/csv_sniff.hpp"

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
	cur_buffer_handle = buffer_manager->GetBuffer(buffer_idx);
	if (cur_buffer_handle) {
		buffer_handles[buffer_start] = cur_buffer_handle;
	}
	cursor.pos = start;
	cursor.line_start = start;
	rows.reserve(result_size + 1);
}

idx_t CsvColumnCounter::SkipRows(CSVBufferManager &buffer_manager, const CSVStateMachine &state_machine, idx_t rows) {
	if (rows == 0) {
		return 0;
	}
	const auto grammar = CsvGrammar::For(state_machine);
	CsvSkip skip(grammar, rows);
	idx_t offset = 0;
	for (idx_t index = 0;; index++) {
		auto handle = buffer_manager.GetBuffer(index);
		if (!handle) {
			return DConstants::INVALID_INDEX;
		}
		if (skip.Feed(handle->Ptr(), offset, offset + handle->actual_size)) {
			return skip.Start(state_machine.options);
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
	for (auto &entry : buffer_handles) {
		const idx_t start = entry.first;
		const idx_t from = MaxValue(begin, start);
		const idx_t to = MinValue(end, start + entry.second->actual_size);
		if (from >= to) {
			continue;
		}
		const char *data = entry.second->Ptr();
		idx_t position = from;
		if (position == begin && (data[position - start] == '\n' || data[position - start] == '\r')) {
			position++;
		}
		line.append(data + (position - start), to - position);
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
	cur_buffer_handle = std::move(next);
	buffer_handles[buffer_start] = cur_buffer_handle;
	if (buffer_handles.size() > 2) {
		buffer_handles.erase(buffer_handles.begin());
	}
	return true;
}

CsvColumnCounts &CsvColumnCounter::ParseChunk() {
	result.result_position = 0;
	if (done || FinishedFile()) {
		return result;
	}
	rows.clear();
	while (rows.size() < result.result_size && !cursor.invalid) {
		const idx_t buffer_end = buffer_start + cur_buffer_handle->actual_size;
		const idx_t first = rows.size();
		if (cursor.pos >= buffer_end) {
			if (result.error) {
				break;
			}
			if (!NextBuffer()) {
				lexer.FinishCount(cursor, rows);
				finished = true;
				for (idx_t i = first; i < rows.size(); i++) {
					AddRow(rows[i]);
				}
				result.error = result.error || cursor.unterminated;
				break;
			}
			if (LineTooLong(cursor.line_start, buffer_start)) {
				break;
			}
			continue;
		}
		lexer.CountRows(cursor, cur_buffer_handle->Ptr(), buffer_start, buffer_end, result.result_size - rows.size(),
		                rows);
		for (idx_t i = first; i < rows.size(); i++) {
			AddRow(rows[i]);
		}
	}
	if (cursor.invalid) {
		result.result_position = 0;
		result.error = true;
	}
	ever_quoted = cursor.ever_quoted;
	ever_escaped = cursor.ever_escaped;
	used_unstrictness = cursor.used_unstrictness;
	return result;
}

} // namespace duckdb
