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
	scan->Start(0, {});
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
      skipper(state_machine->transition_array.stop_patterns) {
	if (start == DConstants::INVALID_INDEX) {
		done = true;
		start = 0;
	}
	const idx_t requested = buffer_manager->GetBufferSize();
	buffer_idx = start / requested;
	buffer_pos = start % requested;
	cur_buffer_handle = buffer_manager->GetBuffer(buffer_idx);
	if (cur_buffer_handle) {
		buffer_ptr = cur_buffer_handle->Ptr();
		current_buffer_size = cur_buffer_handle->actual_size;
		buffer_handles[0] = cur_buffer_handle;
	}
	cur_buffer_idx = buffer_idx;
	last_position = {buffer_idx, buffer_pos, current_buffer_size};
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
	if (!cur_buffer_handle) {
		return true;
	}
	if (!buffer_manager->Done()) {
		return false;
	}
	if (buffer_idx != buffer_manager->BufferCount()) {
		return false;
	}
	return buffer_pos + 1 == cur_buffer_handle->actual_size;
}

string CsvColumnCounter::LineText(const Position &begin, const Position &end) {
	if (begin.buffer_pos == end.buffer_pos && begin.buffer_idx == end.buffer_idx &&
	    begin.buffer_size == end.buffer_size) {
		return string();
	}
	string line;
	if (end.buffer_idx == begin.buffer_idx || begin.buffer_pos == begin.buffer_size) {
		auto entry = buffer_handles.find(end.buffer_idx);
		if (entry == buffer_handles.end()) {
			return string();
		}
		const idx_t start_pos = begin.buffer_pos == begin.buffer_size ? 0 : begin.buffer_pos;
		auto data = entry->second->Ptr();
		const bool first_nl = data[start_pos] == '\n' || data[start_pos] == '\r';
		for (idx_t i = start_pos + first_nl; i < end.buffer_pos; i++) {
			line += data[i];
		}
	} else {
		auto first = buffer_handles.find(begin.buffer_idx);
		auto second = buffer_handles.find(end.buffer_idx);
		if (first == buffer_handles.end() || second == buffer_handles.end()) {
			return string();
		}
		if (begin.buffer_pos >= begin.buffer_size) {
			throw InternalException("CSV reader: buffer pos out of range for buffer");
		}
		auto first_data = first->second->Ptr();
		const bool first_nl = first_data[begin.buffer_pos] == '\n' || first_data[begin.buffer_pos] == '\r';
		for (idx_t i = begin.buffer_pos + first_nl; i < first->second->actual_size; i++) {
			line += first_data[i];
		}
		auto second_data = second->second->Ptr();
		for (idx_t i = 0; i < end.buffer_pos; i++) {
			line += second_data[i];
		}
	}
	CsvSanitize(line);
	return line;
}

bool CsvColumnCounter::LineTooLong(const Position &end) {
	auto &options = state_machine->options;
	if (end - last_position <= options.maximum_line_size.GetValue()) {
		return false;
	}
	auto line = LineText(last_position, end);
	const idx_t position = options.buffer_size_option.GetValue() * last_position.buffer_idx + last_position.buffer_pos;
	auto error = CSVError::LineSizeError(options, LinesPerBoundary(), line, position, options.file_path);
	error_handler->Error(error);
	result.error = true;
	return true;
}

bool CsvColumnCounter::AddRow(idx_t position) {
	const Position current {cur_buffer_idx, position + 1, current_buffer_size};
	if (position != NumericLimits<idx_t>::Maximum()) {
		LineTooLong(current);
	}
	const idx_t column_count = current_column_count + 1;
	auto &count = result.column_counts[result.result_position];
	count.number_of_columns = column_count;
	count.empty_lines = empty_lines;
	result.rows_per_column_count[column_count]++;
	current_column_count = 0;
	last_position = current;
	const bool empty_last_value = (states[0] == CSVState::DELIMITER &&
	                               (states[1] == CSVState::RECORD_SEPARATOR || states[1] == CSVState::CARRIAGE_RETURN ||
	                                states[1] == CSVState::DELIMITER)) ||
	                              (states[0] == CSVState::STANDARD && states[1] == CSVState::DELIMITER);
	if (!empty_last_value) {
		idx_t index = result.result_position;
		for (idx_t i = 0; i < result.result_position + 1; i++) {
			if (!result.column_counts[index].last_value_always_empty) {
				break;
			}
			result.column_counts[index--].last_value_always_empty = false;
		}
	}
	result.result_position++;
	return result.result_position >= result.result_size;
}

void CsvColumnCounter::SetComment() {
	if (states[0] != CSVState::STANDARD && states[0] != CSVState::DELIMITER) {
		cur_line_starts_as_comment = true;
	}
	comment = true;
}

bool CsvColumnCounter::UnsetComment(idx_t position) {
	const bool full = AddRow(position);
	auto &count = result.column_counts[result.result_position - 1];
	if (cur_line_starts_as_comment) {
		count.is_comment = true;
	} else {
		count.is_mid_comment = true;
	}
	comment = false;
	cur_line_starts_as_comment = false;
	return full;
}

void CsvColumnCounter::Process() {
	auto &dialect = state_machine->dialect_options.state_machine_options;
	auto &machine = state_machine->state_machine_options;
	const bool has_escape = dialect.escape.GetValue() != '\0';
	const char quote = dialect.quote.GetValue();
	const bool strict = dialect.strict_mode.GetValue();
	const bool only_rn_newlines = machine.strict_mode.GetValue() && machine.strict_mode.IsSetByUser() &&
	                              machine.new_line.GetValue() == NewLineIdentifier::CARRY_ON &&
	                              machine.new_line.IsSetByUser();
	const auto &table = state_machine->transition_array;
	const idx_t to_pos = cur_buffer_handle->actual_size;
	skipper.SetBuffer(*cur_buffer_handle);
	auto skip = [&](const bool(&skippable)[StateMachine::NUM_TRANSITIONS]) {
		skipper.SkipToStop(skippable, to_pos, buffer_pos);
	};
	while (buffer_pos < to_pos) {
		const char byte = buffer_ptr[buffer_pos];
		states[0] = states[1];
		states[1] = table.state_machine[static_cast<uint8_t>(byte)][static_cast<uint8_t>(states[1])];
		switch (states[1]) {
		case CSVState::INVALID:
			result.result_position = 0;
			result.error = true;
			buffer_pos++;
			return;
		case CSVState::RECORD_SEPARATOR:
		case CSVState::CARRIAGE_RETURN:
			if (states[0] == CSVState::RECORD_SEPARATOR || states[0] == CSVState::NOT_SET) {
				empty_lines++;
			} else if (states[0] != CSVState::CARRIAGE_RETURN) {
				bool full = false;
				if (comment) {
					full = UnsetComment(buffer_pos);
				} else if (states[1] == CSVState::RECORD_SEPARATOR || !only_rn_newlines) {
					full = AddRow(buffer_pos);
				}
				if (full) {
					buffer_pos++;
					return;
				}
			}
			buffer_pos++;
			break;
		case CSVState::DELIMITER:
			current_column_count++;
			buffer_pos++;
			break;
		case CSVState::QUOTED:
			if ((states[0] == CSVState::UNQUOTED || states[0] == CSVState::MAYBE_QUOTED) && has_escape) {
				ever_escaped = true;
			}
			if ((states[0] == CSVState::ESCAPE || states[0] == CSVState::ESCAPED_RETURN ||
			     states[0] == CSVState::UNQUOTED_ESCAPE) &&
			    (byte == quote || !strict)) {
				ever_escaped = true;
				used_unstrictness = used_unstrictness || states[0] == CSVState::UNQUOTED_ESCAPE;
			}
			ever_quoted = true;
			buffer_pos++;
			skip(table.skip_quoted);
			break;
		case CSVState::UNQUOTED:
			ever_escaped = ever_escaped || states[0] == CSVState::MAYBE_QUOTED;
			buffer_pos++;
			break;
		case CSVState::UNQUOTED_ESCAPE:
			used_unstrictness = true;
			buffer_pos++;
			break;
		case CSVState::STANDARD:
			buffer_pos++;
			skip(table.skip_standard);
			break;
		case CSVState::COMMENT:
			SetComment();
			buffer_pos++;
			skip(table.skip_comment);
			break;
		default:
			buffer_pos++;
			break;
		}
	}
}

void CsvColumnCounter::FinalizeChunkProcess() {
	if (result.result_position == result.result_size || result.error) {
		return;
	}
	while (!FinishedFile() && result.result_position < result.result_size && !result.error) {
		if (buffer_pos == cur_buffer_handle->actual_size) {
			cur_buffer_handle = buffer_manager->GetBuffer(++buffer_idx);
			if (!cur_buffer_handle) {
				buffer_ptr = nullptr;
				const bool quoted = states[1] == CSVState::QUOTED || states[1] == CSVState::QUOTED_NEW_LINE;
				if (quoted && states[0] != CSVState::UNQUOTED) {
					result.error = true;
					return;
				}
				const bool new_line = states[1] == CSVState::RECORD_SEPARATOR || states[1] == CSVState::CARRIAGE_RETURN;
				if (new_line || states[1] == CSVState::NOT_SET) {
					return;
				}
				if (comment) {
					UnsetComment(NumericLimits<idx_t>::Maximum());
				} else {
					AddRow(NumericLimits<idx_t>::Maximum());
				}
				return;
			}
			buffer_handles[buffer_idx] = cur_buffer_handle;
			cur_buffer_idx = buffer_idx;
			current_buffer_size = cur_buffer_handle->actual_size;
			if (LineTooLong({cur_buffer_idx, 0, current_buffer_size})) {
				return;
			}
			if (buffer_handles.size() > 2) {
				buffer_handles.erase(buffer_handles.begin());
			}
			buffer_pos = 0;
			buffer_ptr = cur_buffer_handle->Ptr();
		}
		Process();
	}
}

CsvColumnCounts &CsvColumnCounter::ParseChunk() {
	result.result_position = 0;
	if (cur_buffer_handle) {
		current_buffer_size = cur_buffer_handle->actual_size;
	}
	if (done) {
		return result;
	}
	if (!initialized) {
		states[0] = CSVState::NOT_SET;
		states[1] = CSVState::NOT_SET;
		initialized = true;
	}
	if (cur_buffer_handle) {
		Process();
	}
	FinalizeChunkProcess();
	return result;
}

} // namespace duckdb
