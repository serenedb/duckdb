#include "shell_renderer.hpp"

#include "shell_state.hpp"
#include "duckdb/common/printer.hpp"
#include <absl/strings/str_cat.h>
#include <cstring>

namespace duckdb_shell {

namespace {

struct PsqlLine {
	const char *data = nullptr;
	idx_t size = 0;
	idx_t width = 0;
};

class PsqlCellLines {
public:
	void Start(const char *data_p, idx_t size_p) {
		data = data_p;
		size = size_p;
		position = 0;
		has_next = true;
	}

	bool Next(PsqlLine &line) {
		if (!has_next) {
			return false;
		}
		idx_t end = position;
		bool plain = true;
		while (end < size && data[end] != '\n') {
			auto c = static_cast<unsigned char>(data[end]);
			plain = plain && c >= 0x20 && c != 0x7f;
			end++;
		}
		has_next = end < size;
		if (plain) {
			line.data = data + position;
			line.size = end - position;
			line.width = ShellState::RenderLength(line.data, line.size);
		} else {
			Format(end, line);
		}
		position = end + 1;
		return true;
	}

private:
	void Format(idx_t end, PsqlLine &line) {
		static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
		buffer.clear();
		idx_t width = 0;
		idx_t run_start = position;
		for (idx_t i = position; i < end; i++) {
			auto c = static_cast<unsigned char>(data[i]);
			if (c >= 0x20 && c != 0x7f) {
				continue;
			}
			buffer.append(data + run_start, i - run_start);
			width += ShellState::RenderLength(data + run_start, i - run_start);
			if (c == '\t') {
				do {
					buffer += ' ';
					width++;
				} while (width % 8 != 0);
			} else {
				buffer += "\\x";
				buffer += HEX_DIGITS[c >> 4];
				buffer += HEX_DIGITS[c & 0xf];
				width += 4;
			}
			run_start = i + 1;
		}
		buffer.append(data + run_start, end - run_start);
		width += ShellState::RenderLength(data + run_start, end - run_start);
		line.data = buffer.data();
		line.size = buffer.size();
		line.width = width;
	}

	string buffer;
	const char *data = nullptr;
	idx_t size = 0;
	idx_t position = 0;
	bool has_next = false;
};

class PsqlRenderer : public RowRenderer {
public:
	explicit PsqlRenderer(ShellState &state)
	    : RowRenderer(state), null_value(state.PsqlNullValue()), null_size(strlen(null_value)),
	      footer(state.psql_footer) {
	}

	unique_ptr<duckdb::DataChunk> ConvertChunk(duckdb::DataChunk &chunk) override {
		auto render_text = state.engine.render_text;
		if (!render_text) {
			return ShellRenderer::ConvertChunk(chunk);
		}
		auto text = make_uniq<duckdb::DataChunk>();
		vector<duckdb::LogicalType> types(chunk.ColumnCount(), duckdb::LogicalType::VARCHAR);
		text->Initialize(duckdb::Allocator::DefaultAllocator(), types);
		render_text(*state.conn->context, chunk, *text);
		text->SetChildCardinality(chunk.size());
		text->Flatten();
		return text;
	}

	const char *NullValue() override {
		return null_value;
	}

protected:
	void AppendFooter(idx_t row_count) {
		absl::StrAppend(&buffer, "(", row_count, row_count == 1 ? " row)" : " rows)");
	}

	const char *null_value;
	idx_t null_size;
	bool footer;
	string buffer;
};

class PsqlAlignedRenderer : public PsqlRenderer {
public:
	explicit PsqlAlignedRenderer(ShellState &state)
	    : PsqlRenderer(state), border(duckdb::MinValue<int64_t>(duckdb::MaxValue<int64_t>(state.psql_border, 0), 2)),
	      expanded_mode(state.psql_expanded) {
	}

	bool RequireMaterializedResult() const override {
		return true;
	}

	void Analyze(RenderingQueryResult &result) override {
		while (result.TryConvertChunk()) {
			if (state.seenInterrupt) {
				return;
			}
		}
		column_count = result.ColumnCount();
		row_count = result.loaded_row_count;
		column_widths.assign(column_count, 0);
		right_align.assign(column_count, false);
		PsqlLine line;
		for (idx_t c = 0; c < column_count; c++) {
			auto &name = result.metadata.column_names[c];
			cell.Start(name.c_str(), name.size());
			idx_t line_count = 0;
			while (cell.Next(line)) {
				line_count++;
				column_widths[c] = duckdb::MaxValue(column_widths[c], line.width);
			}
			header_multiline = header_multiline || line_count > 1;
			header_width = duckdb::MaxValue(header_width, column_widths[c]);
			right_align[c] = result.metadata.types[c].IsNumeric();
		}
		for (auto &chunk : result.chunks) {
			for (idx_t c = 0; c < column_count; c++) {
				auto &vector = chunk->data[c];
				auto strings = duckdb::FlatVector::GetData<duckdb::string_t>(vector);
				for (idx_t r = 0; r < chunk->size(); r++) {
					StartCell(cell, vector, strings, r);
					idx_t line_count = 0;
					while (cell.Next(line)) {
						line_count++;
						column_widths[c] = duckdb::MaxValue(column_widths[c], line.width);
						data_width = duckdb::MaxValue(data_width, line.width);
					}
					data_multiline = data_multiline || line_count > 1;
				}
			}
		}
		expanded = expanded_mode == PsqlExpanded::ON;
		if (expanded_mode == PsqlExpanded::AUTO && column_count > 1 && state.stdout_is_console &&
		    state.outfile.empty()) {
			auto terminal_width = duckdb::Printer::TerminalWidth();
			expanded = terminal_width > 0 && TableWidth() > terminal_width;
		}
	}

	bool ShouldUsePager(RenderingQueryResult &result, PagerMode global_mode) override {
		if (global_mode == PagerMode::PAGER_ON) {
			return true;
		}
		if (expanded) {
			return row_count * (column_count + 1) >= state.pager_min_rows ||
			       header_width + data_width + 3 > state.GetMaxRenderWidth();
		}
		return row_count >= state.pager_min_rows || TableWidth() > state.GetMaxRenderWidth();
	}

	SuccessState RenderQueryResult(PrintStream &out, ShellState &shell, RenderingQueryResult &result) override {
		auto complete = expanded ? RenderExpanded(out, result) : RenderTable(out, result);
		if (!complete) {
			shell.PrintF("Interrupt\n");
			return SuccessState::FAILURE;
		}
		return SuccessState::SUCCESS;
	}

private:
	template <class STRING>
	void StartCell(PsqlCellLines &lines, duckdb::Vector &vector, STRING strings, idx_t row) {
		if (duckdb::FlatVector::IsNull(vector, row)) {
			lines.Start(null_value, null_size);
			return;
		}
		lines.Start(strings[row].GetData(), strings[row].GetSize());
	}

	idx_t TableWidth() const {
		idx_t width;
		if (border == 0) {
			width = column_count;
		} else if (border == 1) {
			width = column_count * 3 - (column_count > 0 ? 1 : 0);
		} else {
			width = column_count * 3 + 1;
		}
		for (auto column_width : column_widths) {
			width += column_width;
		}
		return width;
	}

	void RenderRule(PrintStream &out) {
		buffer.clear();
		if (border == 1) {
			buffer += '-';
		} else if (border == 2) {
			buffer += "+-";
		}
		for (idx_t c = 0; c < column_count; c++) {
			buffer.append(column_widths[c], '-');
			if (c + 1 < column_count) {
				buffer += border == 0 ? " " : "-+-";
			}
		}
		if (border == 2) {
			buffer += "-+";
		} else if (border == 1) {
			buffer += '-';
		}
		buffer += '\n';
		out.Print(buffer);
	}

	void PrepareCells() {
		cells.resize(column_count);
		cell_lines.resize(column_count);
		has_line.resize(column_count);
	}

	void RenderHeaderLines(PrintStream &out, ResultMetadata &metadata) {
		PrepareCells();
		for (idx_t c = 0; c < column_count; c++) {
			auto &name = metadata.column_names[c];
			cells[c].Start(name.c_str(), name.size());
			has_line[c] = cells[c].Next(cell_lines[c]);
		}
		bool first_line = true;
		bool more_lines = true;
		while (more_lines) {
			more_lines = false;
			buffer.clear();
			if (border == 2) {
				buffer += '|';
			}
			for (idx_t c = 0; c < column_count; c++) {
				if (border != 0) {
					buffer += first_line ? ' ' : '+';
				}
				if (has_line[c]) {
					auto &line = cell_lines[c];
					auto spaces = column_widths[c] - line.width;
					buffer.append(spaces / 2, ' ');
					buffer.append(line.data, line.size);
					buffer.append((spaces + 1) / 2, ' ');
					has_line[c] = cells[c].Next(cell_lines[c]);
					more_lines = more_lines || has_line[c];
				} else {
					buffer.append(column_widths[c], ' ');
				}
				buffer += has_line[c] ? '+' : ' ';
				if (border != 0 && c + 1 < column_count) {
					buffer += '|';
				}
			}
			if (border == 2) {
				buffer += '|';
			}
			buffer += '\n';
			out.Print(buffer);
			first_line = false;
		}
	}

	void RenderRowLines(PrintStream &out) {
		bool more_lines = true;
		while (more_lines) {
			more_lines = false;
			buffer.clear();
			if (border == 2) {
				buffer += '|';
			}
			for (idx_t c = 0; c < column_count; c++) {
				bool last = c + 1 == column_count;
				bool final_spaces = border == 2 || !last;
				if (border != 0) {
					buffer += ' ';
				}
				idx_t printed = column_widths[c];
				bool continues = false;
				if (has_line[c]) {
					auto &line = cell_lines[c];
					printed = line.width;
					if (right_align[c] && column_widths[c] > printed) {
						buffer.append(column_widths[c] - printed, ' ');
					}
					buffer.append(line.data, line.size);
					has_line[c] = cells[c].Next(cell_lines[c]);
					continues = has_line[c];
					more_lines = more_lines || continues;
				} else if (final_spaces) {
					buffer.append(column_widths[c], ' ');
				}
				if (!right_align[c] && (final_spaces || continues) && column_widths[c] > printed) {
					buffer.append(column_widths[c] - printed, ' ');
				}
				if (continues) {
					buffer += '+';
				} else if (final_spaces) {
					buffer += ' ';
				}
				if (border != 0 && !last) {
					buffer += '|';
				}
			}
			if (border == 2) {
				buffer += '|';
			}
			buffer += '\n';
			out.Print(buffer);
		}
	}

	bool RenderTable(PrintStream &out, RenderingQueryResult &result) {
		if (show_header) {
			if (border == 2) {
				RenderRule(out);
			}
			RenderHeaderLines(out, result.metadata);
			RenderRule(out);
		}
		PrepareCells();
		for (auto &chunk : result.chunks) {
			for (idx_t r = 0; r < chunk->size(); r++) {
				if (state.seenInterrupt) {
					return false;
				}
				for (idx_t c = 0; c < column_count; c++) {
					auto &vector = chunk->data[c];
					StartCell(cells[c], vector, duckdb::FlatVector::GetData<duckdb::string_t>(vector), r);
					has_line[c] = cells[c].Next(cell_lines[c]);
				}
				RenderRowLines(out);
			}
		}
		if (border == 2) {
			RenderRule(out);
		}
		buffer.clear();
		if (show_header && footer) {
			AppendFooter(row_count);
			buffer += '\n';
		}
		buffer += '\n';
		out.Print(buffer);
		return true;
	}

	void RenderRecordLine(PrintStream &out, idx_t record) {
		buffer.clear();
		if (border == 2) {
			buffer += "+-";
		} else if (border == 1) {
			buffer += '-';
		}
		int64_t label = 0;
		if (record > 0) {
			auto label_start = buffer.size();
			if (border == 0) {
				absl::StrAppend(&buffer, "* Record ", record);
			} else {
				absl::StrAppend(&buffer, "[ RECORD ", record, " ]");
			}
			label = static_cast<int64_t>(buffer.size() - label_start);
		}
		if (border != 2) {
			label++;
		}
		auto fill = border > 0 ? '-' : ' ';
		auto header = static_cast<int64_t>(header_width);
		if (label < header) {
			buffer.append(static_cast<size_t>(header - label), fill);
		}
		label -= header;
		if (border > 0) {
			if (label-- <= 0) {
				buffer += '-';
			}
			if (label-- <= 0) {
				buffer += '+';
			}
			if (label-- <= 0) {
				buffer += '-';
			}
		} else if (label-- <= 0) {
			buffer += ' ';
		}
		label = duckdb::MaxValue<int64_t>(label, 0);
		auto data = static_cast<int64_t>(data_width);
		if (label < data) {
			buffer.append(static_cast<size_t>(data - label), fill);
		}
		if (border == 2) {
			buffer += "-+";
		}
		buffer += '\n';
		out.Print(buffer);
	}

	void RenderField(PrintStream &out, PsqlCellLines &header_lines, PsqlCellLines &value_lines) {
		PsqlLine header_line;
		PsqlLine value_line;
		bool header_complete = !header_lines.Next(header_line);
		bool value_complete = !value_lines.Next(value_line);
		bool first_header_line = true;
		while (!header_complete || !value_complete) {
			buffer.clear();
			if (border == 2) {
				buffer += '|';
			}
			if (!header_complete) {
				if (border == 2) {
					buffer += first_header_line ? ' ' : '+';
				}
				buffer.append(header_line.data, header_line.size);
				if (header_width > header_line.width) {
					buffer.append(header_width - header_line.width, ' ');
				}
				header_complete = !header_lines.Next(header_line);
				if (border > 0 || header_multiline) {
					buffer += header_complete ? ' ' : '+';
				}
				first_header_line = false;
			} else {
				auto spaces = header_width + static_cast<idx_t>(border);
				if (border == 0 && header_multiline) {
					spaces++;
				}
				buffer.append(duckdb::MaxValue<idx_t>(spaces, 1), ' ');
			}
			if (border > 0) {
				buffer += '|';
			}
			if (!value_complete) {
				buffer += ' ';
				buffer.append(value_line.data, value_line.size);
				auto spaces = data_width > value_line.width ? data_width - value_line.width : 0;
				value_complete = !value_lines.Next(value_line);
				if (!value_complete) {
					if (border > 1 || data_multiline) {
						buffer.append(spaces, ' ');
						buffer += '+';
					}
				} else if (border > 1) {
					buffer.append(spaces, ' ');
					buffer += ' ';
				}
				if (border == 2) {
					buffer += '|';
				}
				buffer += '\n';
			} else if (border < 2) {
				buffer += '\n';
			} else {
				buffer.append(data_width + 2, ' ');
				buffer += "|\n";
			}
			out.Print(buffer);
		}
	}

	bool RenderExpanded(PrintStream &out, RenderingQueryResult &result) {
		buffer.clear();
		if (row_count == 0) {
			if (show_header && footer) {
				AppendFooter(0);
				buffer += '\n';
			}
			buffer += '\n';
			out.Print(buffer);
			return true;
		}
		auto &names = result.metadata.column_names;
		idx_t record = 0;
		for (auto &chunk : result.chunks) {
			for (idx_t r = 0; r < chunk->size(); r++) {
				if (state.seenInterrupt) {
					return false;
				}
				record++;
				if (show_header) {
					RenderRecordLine(out, record);
				} else if (record > 1 || border == 2) {
					RenderRecordLine(out, 0);
				}
				for (idx_t c = 0; c < column_count; c++) {
					header_cell.Start(names[c].c_str(), names[c].size());
					auto &vector = chunk->data[c];
					StartCell(cell, vector, duckdb::FlatVector::GetData<duckdb::string_t>(vector), r);
					RenderField(out, header_cell, cell);
				}
			}
		}
		if (border == 2) {
			RenderRecordLine(out, 0);
		}
		out.Print("\n");
		return true;
	}

	int64_t border;
	PsqlExpanded expanded_mode;
	bool expanded = false;
	idx_t column_count = 0;
	idx_t row_count = 0;
	vector<idx_t> column_widths;
	vector<bool> right_align;
	idx_t header_width = 0;
	idx_t data_width = 0;
	bool header_multiline = false;
	bool data_multiline = false;
	PsqlCellLines cell;
	PsqlCellLines header_cell;
	vector<PsqlCellLines> cells;
	vector<PsqlLine> cell_lines;
	vector<bool> has_line;
};

class PsqlUnalignedRenderer : public PsqlRenderer {
public:
	PsqlUnalignedRenderer(ShellState &state, bool expanded) : PsqlRenderer(state), expanded(expanded) {
	}

	SuccessState RenderQueryResult(PrintStream &out, ShellState &shell, RenderingQueryResult &result) override {
		row_count = 0;
		need_separator = false;
		return ShellRenderer::RenderQueryResult(out, shell, result);
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (expanded || !show_header) {
			return;
		}
		buffer.clear();
		for (idx_t c = 0; c < result.ColumnCount(); c++) {
			if (c > 0) {
				buffer += col_sep;
			}
			buffer += result.column_names[c];
		}
		need_separator = true;
		out.Print(buffer);
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		buffer.clear();
		if (need_separator) {
			buffer += row_sep;
			if (expanded) {
				buffer += row_sep;
			}
		}
		for (idx_t c = 0; c < row.data.size(); c++) {
			if (expanded) {
				if (c > 0) {
					buffer += row_sep;
				}
				buffer += result.column_names[c];
				buffer += col_sep;
			} else if (c > 0) {
				buffer += col_sep;
			}
			buffer.append(row.data[c].GetData(), row.data[c].GetSize());
		}
		need_separator = true;
		row_count++;
		out.Print(buffer);
	}

	void RenderFooter(PrintStream &out, ResultMetadata &result) override {
		buffer.clear();
		if (!expanded && show_header && footer) {
			if (need_separator) {
				buffer += row_sep;
			}
			AppendFooter(row_count);
			need_separator = true;
		}
		if (need_separator) {
			buffer += row_sep.size() == 1 && row_sep[0] == '\0' ? '\0' : '\n';
		}
		out.Print(buffer);
	}

private:
	bool expanded;
	bool need_separator = false;
	idx_t row_count = 0;
};

class PsqlCsvRenderer : public PsqlRenderer {
public:
	PsqlCsvRenderer(ShellState &state, bool expanded)
	    : PsqlRenderer(state), expanded(expanded), separator(state.psql_csv_separator) {
	}

	void RenderHeader(PrintStream &out, ResultMetadata &result) override {
		if (expanded || !show_header) {
			return;
		}
		buffer.clear();
		for (idx_t c = 0; c < result.ColumnCount(); c++) {
			if (c > 0) {
				buffer += separator;
			}
			auto &name = result.column_names[c];
			AppendField(name.c_str(), name.size(), true);
		}
		buffer += '\n';
		out.Print(buffer);
	}

	void RenderRow(PrintStream &out, ResultMetadata &result, RowData &row) override {
		buffer.clear();
		for (idx_t c = 0; c < row.data.size(); c++) {
			auto &value = row.data[c];
			if (expanded) {
				auto &name = result.column_names[c];
				AppendField(name.c_str(), name.size(), true);
				buffer += separator;
			} else if (c > 0) {
				buffer += separator;
			}
			AppendField(value.GetData(), value.GetSize(), row.is_null[c]);
			if (expanded) {
				buffer += '\n';
			}
		}
		if (!expanded) {
			buffer += '\n';
		}
		out.Print(buffer);
	}

private:
	void AppendField(const char *data, idx_t size, bool unambiguous) {
		bool quote = separator == '\\' || separator == '.' || (size == 2 && data[0] == '\\' && data[1] == '.');
		for (idx_t i = 0; i < size && !quote; i++) {
			auto c = data[i];
			quote = c == separator || c == '"' || c == '\n' || c == '\r';
		}
		if (!quote && !unambiguous && size == null_size && memcmp(data, null_value, size) == 0) {
			quote = true;
		}
		if (!quote) {
			buffer.append(data, size);
			return;
		}
		buffer += '"';
		for (idx_t i = 0; i < size; i++) {
			if (data[i] == '"') {
				buffer += '"';
			}
			buffer += data[i];
		}
		buffer += '"';
	}

	bool expanded;
	char separator;
};

} // namespace

unique_ptr<ShellRenderer> CreatePsqlRenderer(ShellState &state) {
	auto expanded = state.psql_expanded == PsqlExpanded::ON;
	switch (state.psql_format) {
	case PsqlFormat::UNALIGNED:
		return make_uniq<PsqlUnalignedRenderer>(state, expanded);
	case PsqlFormat::CSV:
		return make_uniq<PsqlCsvRenderer>(state, expanded);
	default:
		return make_uniq<PsqlAlignedRenderer>(state);
	}
}

} // namespace duckdb_shell
