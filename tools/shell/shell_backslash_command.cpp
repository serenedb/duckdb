#include "shell_state.hpp"

#include "duckdb/common/file_system.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace duckdb_shell {

namespace {

class BackslashLine {
public:
	explicit BackslashLine(std::string_view line_p) : line(line_p) {
	}

	bool NextCommand(string &name) {
		SkipSpaces();
		if (position >= line.size() || line[position] != '\\') {
			return false;
		}
		auto start = ++position;
		while (position < line.size() && !ShellState::IsSpace(line[position]) && line[position] != '\\') {
			position++;
		}
		name = string(line.substr(start, position - start));
		return true;
	}

	bool NextArgument(string &argument) {
		SkipSpaces();
		if (position >= line.size() || line[position] == '\\') {
			return false;
		}
		argument.clear();
		quoted = false;
		while (position < line.size() && !ShellState::IsSpace(line[position]) && line[position] != '\\') {
			auto c = line[position];
			if (c == '\'') {
				ReadSingleQuoted(argument);
			} else if (c == '"') {
				ReadDoubleQuoted(argument);
			} else {
				argument += c;
				position++;
			}
		}
		return true;
	}

	bool NextFileArgument(string &argument) {
		SkipSpaces();
		if (position < line.size() && line[position] == '|') {
			argument = RestOfLine();
			return true;
		}
		return NextArgument(argument);
	}

	string RestOfLine() {
		SkipSpaces();
		auto end = line.size();
		while (end > position && ShellState::IsSpace(line[end - 1])) {
			end--;
		}
		string rest(line.substr(position, end - position));
		position = line.size();
		return rest;
	}

	bool LastArgumentQuoted() const {
		return quoted;
	}

private:
	void SkipSpaces() {
		while (position < line.size() && ShellState::IsSpace(line[position])) {
			position++;
		}
	}

	static bool IsOctalDigit(char c) {
		return c >= '0' && c <= '7';
	}

	static int HexValue(char c) {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}
		if (c >= 'a' && c <= 'f') {
			return c - 'a' + 10;
		}
		if (c >= 'A' && c <= 'F') {
			return c - 'A' + 10;
		}
		return -1;
	}

	void ReadSingleQuoted(string &argument) {
		quoted = true;
		position++;
		while (position < line.size()) {
			auto c = line[position++];
			if (c == '\'') {
				if (position < line.size() && line[position] == '\'') {
					argument += '\'';
					position++;
					continue;
				}
				return;
			}
			if (c != '\\' || position >= line.size()) {
				argument += c;
				continue;
			}
			c = line[position++];
			if (c == 'n') {
				argument += '\n';
			} else if (c == 't') {
				argument += '\t';
			} else if (c == 'b') {
				argument += '\b';
			} else if (c == 'r') {
				argument += '\r';
			} else if (c == 'f') {
				argument += '\f';
			} else if (c == 'x' && position < line.size() && HexValue(line[position]) >= 0) {
				int value = HexValue(line[position++]);
				if (position < line.size() && HexValue(line[position]) >= 0) {
					value = value * 16 + HexValue(line[position++]);
				}
				argument += static_cast<char>(value);
			} else if (IsOctalDigit(c)) {
				int value = c - '0';
				for (idx_t digits = 1; digits < 3 && position < line.size() && IsOctalDigit(line[position]); digits++) {
					value = value * 8 + line[position++] - '0';
				}
				argument += static_cast<char>(value);
			} else {
				argument += c;
			}
		}
	}

	void ReadDoubleQuoted(string &argument) {
		quoted = true;
		argument += line[position++];
		while (position < line.size()) {
			auto c = line[position++];
			argument += c;
			if (c == '"') {
				return;
			}
		}
	}

	std::string_view line;
	idx_t position = 0;
	bool quoted = false;
};

void PrintInfo(const string &text) {
	fwrite(text.data(), 1, text.size(), stdout);
}

bool IsPrefixOf(const char *value, const char *word, idx_t min_length) {
	auto length = strlen(value);
	if (length < min_length || length > strlen(word)) {
		return false;
	}
	for (idx_t i = 0; i < length; i++) {
		if (StringUtil::CharacterToLower(value[i]) != word[i]) {
			return false;
		}
	}
	return true;
}

bool ParsePsqlBoolean(const char *value, bool &result) {
	if (IsPrefixOf(value, "true", 1) || IsPrefixOf(value, "yes", 1) || IsPrefixOf(value, "on", 2) ||
	    strcmp(value, "1") == 0) {
		result = true;
		return true;
	}
	if (IsPrefixOf(value, "false", 1) || IsPrefixOf(value, "no", 1) || IsPrefixOf(value, "off", 2) ||
	    strcmp(value, "0") == 0) {
		result = false;
		return true;
	}
	return false;
}

const char *const DUCKDB_MODES[] = {"ascii",    "box",   "column",    "csv",   "duckbox", "html",
                                    "insert",   "json",  "jsonlines", "latex", "line",    "list",
                                    "markdown", "quote", "table",     "tabs",  "tcl",     "trash"};

bool IsDuckDBMode(const string &name) {
	for (auto mode : DUCKDB_MODES) {
		if (name == mode) {
			return true;
		}
	}
	return false;
}

string PrintFormatName(ShellState &state) {
	if (state.mode != RenderMode::PSQL) {
		return ShellState::ModeToString(state.mode);
	}
	switch (state.psql_format) {
	case PsqlFormat::UNALIGNED:
		return "unaligned";
	case PsqlFormat::CSV:
		return "csv";
	default:
		return "aligned";
	}
}

const char *ExpandedName(ShellState &state) {
	if (state.mode != RenderMode::PSQL) {
		return state.mode == RenderMode::LINE ? "on" : "off";
	}
	switch (state.psql_expanded) {
	case PsqlExpanded::ON:
		return "on";
	case PsqlExpanded::AUTO:
		return "auto";
	default:
		return "off";
	}
}

bool IsZeroSeparator(const string &separator) {
	return separator.size() == 1 && separator[0] == '\0';
}

string QuoteSetting(const string &value) {
	string result = "'";
	for (auto c : value) {
		if (c == '\n') {
			result += "\\n";
		} else if (c == '\'') {
			result += "\\'";
		} else {
			result += c;
		}
	}
	result += "'";
	return result;
}

void AddHelpLine(string &help, const string &command, const string &description) {
	help += "  ";
	help += command;
	help.append(command.size() < 23 ? 23 - command.size() : 1, ' ');
	help += description;
	help += '\n';
}

enum class ClientCopyTarget : uint8_t { FILE, SHELL, PROCESS };

struct ClientCopy {
	string before;
	bool from = false;
	ClientCopyTarget target = ClientCopyTarget::SHELL;
	string file;
	string after;
};

void SkipQuoted(const string &text, idx_t &position) {
	auto quote = text[position++];
	while (position < text.size()) {
		if (text[position++] != quote) {
			continue;
		}
		if (position < text.size() && text[position] == quote) {
			position++;
			continue;
		}
		return;
	}
}

string Trim(const string &text) {
	idx_t start = 0;
	auto end = text.size();
	while (start < end && ShellState::IsSpace(text[start])) {
		start++;
	}
	while (end > start && ShellState::IsSpace(text[end - 1])) {
		end--;
	}
	return text.substr(start, end - start);
}

bool ParseClientCopy(const string &arguments, ClientCopy &copy, string &error) {
	if (Trim(arguments).empty()) {
		error = "arguments required";
		return false;
	}
	auto size = arguments.size();
	idx_t position = 0;
	idx_t depth = 0;
	bool seen_token = false;
	idx_t direction_start = string::npos;
	while (position < size) {
		auto c = arguments[position];
		if (ShellState::IsSpace(c)) {
			position++;
			continue;
		}
		if (c == '\'' || c == '"') {
			SkipQuoted(arguments, position);
			seen_token = true;
			continue;
		}
		if (c == '(') {
			depth++;
			position++;
			seen_token = true;
			continue;
		}
		if (c == ')') {
			depth -= depth > 0 ? 1 : 0;
			position++;
			continue;
		}
		auto start = position;
		while (position < size && !ShellState::IsSpace(arguments[position]) && arguments[position] != '(' &&
		       arguments[position] != ')' && arguments[position] != '\'' && arguments[position] != '"') {
			position++;
		}
		auto word = arguments.substr(start, position - start);
		if (depth == 0 && seen_token && (StringUtil::CIEquals(word, "from") || StringUtil::CIEquals(word, "to"))) {
			direction_start = start;
			copy.from = StringUtil::CIEquals(word, "from");
			break;
		}
		seen_token = true;
	}
	if (direction_start == string::npos) {
		error = "parse error at end of line";
		return false;
	}
	copy.before = Trim(arguments.substr(0, direction_start));
	while (position < size && ShellState::IsSpace(arguments[position])) {
		position++;
	}
	if (position >= size) {
		error = "parse error at end of line";
		return false;
	}
	if (arguments[position] == '\'') {
		position++;
		bool closed = false;
		while (position < size) {
			auto c = arguments[position++];
			if (c == '\'') {
				if (position < size && arguments[position] == '\'') {
					copy.file += '\'';
					position++;
					continue;
				}
				closed = true;
				break;
			}
			copy.file += c;
		}
		if (!closed) {
			error = "unterminated quoted string";
			return false;
		}
		copy.target = ClientCopyTarget::FILE;
	} else {
		auto start = position;
		while (position < size && !ShellState::IsSpace(arguments[position]) && arguments[position] != ';') {
			position++;
		}
		auto word = arguments.substr(start, position - start);
		if (StringUtil::CIEquals(word, "stdin") || StringUtil::CIEquals(word, "stdout")) {
			copy.target = ClientCopyTarget::SHELL;
		} else if (StringUtil::CIEquals(word, "pstdin") || StringUtil::CIEquals(word, "pstdout")) {
			copy.target = ClientCopyTarget::PROCESS;
		} else if (StringUtil::CIEquals(word, "program")) {
			error = "PROGRAM is not supported";
			return false;
		} else {
			copy.target = ClientCopyTarget::FILE;
			copy.file = word;
		}
	}
	copy.after = Trim(arguments.substr(position));
	return true;
}

MetadataResult ToResult(bool success) {
	return success ? MetadataResult::SUCCESS : MetadataResult::FAIL;
}

MetadataResult ToResult(SuccessState success) {
	return ToResult(success == SuccessState::SUCCESS);
}

MetadataResult RunBackslashCommand(ShellState &state, const string &command, BackslashLine &line,
                                   std::string_view query_buffer, bool &query_consumed) {
	string argument;
	auto context = "\\" + command;
	if (command == "q" || command == "quit") {
		state.quit_requested = true;
		return MetadataResult::EXIT;
	}
	if (command == "?") {
		line.NextArgument(argument);
		state.PrintBackslashHelp();
		return MetadataResult::SUCCESS;
	}
	if (command == "echo") {
		bool newline = true;
		bool first = true;
		string text;
		while (line.NextArgument(argument)) {
			if (first && newline && !line.LastArgumentQuoted() && argument == "-n") {
				newline = false;
				continue;
			}
			if (!first) {
				text += ' ';
			}
			first = false;
			text += argument;
		}
		if (newline) {
			text += '\n';
		}
		PrintInfo(text);
		return MetadataResult::SUCCESS;
	}
	if (command == "i" || command == "include") {
		if (!line.NextArgument(argument)) {
			state.PrintF(PrintOutput::STDERR, "%s: missing required argument\n", context);
			return MetadataResult::FAIL;
		}
		return ToResult(state.ReadFromFile(duckdb::FileSystem::ExpandPath(argument, nullptr)));
	}
	if (command == "o" || command == "out") {
		vector<string> arguments {"output"};
		if (line.NextFileArgument(argument)) {
			arguments.push_back(argument);
		}
		return ToResult(state.SetOutputFile(arguments, OutputCommand::OUTPUT));
	}
	if (command == "g" || command == "gx") {
		string target;
		line.NextFileArgument(target);
		auto query = query_buffer.empty() ? state.last_query : string(query_buffer);
		query_consumed = true;
		if (query.empty()) {
			return MetadataResult::SUCCESS;
		}
		return ToResult(state.RunQueryBuffer(query, target, command == "gx"));
	}
	if (command == "x") {
		auto value = line.NextArgument(argument) ? argument.c_str() : nullptr;
		return ToResult(state.SetPrintOption("expanded", value, state.quiet, context.c_str()));
	}
	if (command == "a") {
		auto aligned = state.mode == RenderMode::PSQL && state.psql_format == PsqlFormat::ALIGNED;
		auto format = aligned ? "unaligned" : "aligned";
		return ToResult(state.SetPrintOption("format", format, state.quiet, context.c_str()));
	}
	if (command == "t") {
		auto value = line.NextArgument(argument) ? argument.c_str() : nullptr;
		return ToResult(state.SetPrintOption("tuples_only", value, state.quiet, context.c_str()));
	}
	if (command == "pset") {
		string option;
		if (!line.NextArgument(option)) {
			state.PrintPrintOptions();
			return MetadataResult::SUCCESS;
		}
		auto value = line.NextArgument(argument) ? argument.c_str() : nullptr;
		return ToResult(state.SetPrintOption(option, value, state.quiet, context.c_str()));
	}
	if (command == "timing") {
		auto enable = !ShellState::TimerEnabled();
		if (line.NextArgument(argument) && !ParsePsqlBoolean(argument.c_str(), enable)) {
			state.PrintF(PrintOutput::STDERR, "unrecognized value \"%s\" for \"%s\": Boolean expected\n", argument,
			             context);
			return MetadataResult::FAIL;
		}
		if (!state.SetTimer(enable)) {
			return MetadataResult::FAIL;
		}
		if (!state.quiet) {
			PrintInfo(enable ? "Timing is on.\n" : "Timing is off.\n");
		}
		return MetadataResult::SUCCESS;
	}
	if (command == "copy") {
		return ToResult(state.CopyClientSide(line.RestOfLine()));
	}
	if (command == "l" || command == "l+" || command == "list" || command == "list+") {
		return ToResult(state.ExecuteSQL("SHOW DATABASES"));
	}
	if (!command.empty() && command[0] == 'd') {
		state.PrintF(PrintOutput::STDERR,
		             "%s: catalog commands are not supported; use .tables, .schema or DESCRIBE instead\n", context);
		return MetadataResult::FAIL;
	}
	state.PrintF(PrintOutput::STDERR, "invalid command %s\n", context);
	if (!state.in && state.stdin_is_interactive) {
		state.PrintF(PrintOutput::STDERR, "Try \\? for help.\n");
	}
	return MetadataResult::FAIL;
}

} // namespace

int ShellState::DoBackslashCommand(std::string_view text, std::string_view query_buffer, bool &query_consumed) {
	BackslashLine line(text);
	string command;
	string argument;
	while (line.NextCommand(command)) {
		auto result = RunBackslashCommand(*this, command, line, query_buffer, query_consumed);
		if (result == MetadataResult::EXIT) {
			return 2;
		}
		if (result != MetadataResult::SUCCESS) {
			line.RestOfLine();
			return 1;
		}
		while (line.NextArgument(argument)) {
			PrintF(PrintOutput::STDERR, "\\%s: extra argument \"%s\" ignored\n", command, argument);
		}
		if (query_consumed) {
			query_buffer = std::string_view();
		}
	}
	return 0;
}

const char *ShellState::PsqlNullValue() const {
	return null_value_set ? nullValue.c_str() : "";
}

bool ShellState::SetPrintOption(const string &name, const char *value, bool silent, const char *context) {
	auto option = StringUtil::Lower(name);
	auto fail = [&](const string &message) {
		PrintF(PrintOutput::STDERR, "%s: %s\n", context, message);
		return false;
	};
	auto info = [&](const string &message) {
		if (!silent) {
			PrintInfo(message + "\n");
		}
		return true;
	};
	auto boolean_expected = [&]() {
		return fail(StringUtil::Format("unrecognized value \"%s\" for \"%s\": Boolean expected", value, option));
	};
	if (option == "format") {
		if (value) {
			auto format = StringUtil::Lower(value);
			if (format == "aligned" || format == "wrapped") {
				mode = RenderMode::PSQL;
				psql_format = PsqlFormat::ALIGNED;
			} else if (format == "unaligned") {
				mode = RenderMode::PSQL;
				psql_format = PsqlFormat::UNALIGNED;
			} else if (format == "csv" && mode == RenderMode::PSQL) {
				psql_format = PsqlFormat::CSV;
			} else if (IsDuckDBMode(format)) {
				SetOutputMode(format, nullptr);
			} else {
				auto modes = StringUtil::Join(DUCKDB_MODES, ", ", [](const char *entry) { return string(entry); });
				return fail(StringUtil::Format("format \"%s\" is not supported; use a psql format (aligned, unaligned, "
				                               "wrapped, csv, html, latex) or a duckdb mode (%s)",
				                               value, modes));
			}
			cMode = mode;
		}
		return info(StringUtil::Format("Output format is %s.", PrintFormatName(*this)));
	}
	if (option == "expanded" || option == "x" || option == "vertical") {
		if (mode == RenderMode::PSQL) {
			auto expanded = psql_expanded == PsqlExpanded::OFF ? PsqlExpanded::ON : PsqlExpanded::OFF;
			bool on = false;
			if (value && StringUtil::CIEquals(value, "auto")) {
				expanded = PsqlExpanded::AUTO;
			} else if (value && ParsePsqlBoolean(value, on)) {
				expanded = on ? PsqlExpanded::ON : PsqlExpanded::OFF;
			} else if (value) {
				return fail(StringUtil::Format(
				    "unrecognized value \"%s\" for \"expanded\"; available values are: on, off, auto", value));
			}
			psql_expanded = expanded;
		} else {
			auto on = mode != RenderMode::LINE;
			if (value && StringUtil::CIEquals(value, "auto")) {
				on = false;
			} else if (value && !ParsePsqlBoolean(value, on)) {
				return fail(StringUtil::Format(
				    "unrecognized value \"%s\" for \"expanded\"; available values are: on, off, auto", value));
			}
			if (on) {
				mode = RenderMode::LINE;
			} else if (mode == RenderMode::LINE) {
				mode = RenderMode::DUCKBOX;
			}
			cMode = mode;
		}
		auto expanded = string(ExpandedName(*this));
		if (expanded == "auto") {
			return info("Expanded display is used automatically.");
		}
		return info("Expanded display is " + expanded + ".");
	}
	if (option == "tuples_only" || option == "t") {
		auto on = showHeader;
		if (value && !ParsePsqlBoolean(value, on)) {
			return boolean_expected();
		}
		if (mode == RenderMode::PSQL) {
			showHeader = !on;
		} else if (on) {
			mode = RenderMode::LIST;
			showHeader = false;
		} else {
			if (mode == RenderMode::LIST) {
				mode = RenderMode::DUCKBOX;
			}
			showHeader = true;
		}
		cMode = mode;
		ShellSetFlag(ShellFlags::SHFLG_HeaderSet);
		return info(on ? "Tuples only is on." : "Tuples only is off.");
	}
	if (option == "null") {
		if (value) {
			nullValue = value;
			null_value_set = true;
		}
		return info(StringUtil::Format("Null display is \"%s\".",
		                               mode == RenderMode::PSQL ? string(PsqlNullValue()) : nullValue));
	}
	if (option == "fieldsep" || option == "fieldsep_zero") {
		if (option == "fieldsep_zero") {
			colSeparator = string(1, '\0');
		} else if (value) {
			colSeparator = value;
		}
		if (IsZeroSeparator(colSeparator)) {
			return info("Field separator is zero byte.");
		}
		return info(StringUtil::Format("Field separator is \"%s\".", colSeparator));
	}
	if (option == "recordsep" || option == "recordsep_zero") {
		if (option == "recordsep_zero") {
			rowSeparator = string(1, '\0');
		} else if (value) {
			rowSeparator = value;
		}
		if (IsZeroSeparator(rowSeparator)) {
			return info("Record separator is zero byte.");
		}
		if (rowSeparator == "\n") {
			return info("Record separator is <newline>.");
		}
		return info(StringUtil::Format("Record separator is \"%s\".", rowSeparator));
	}
	if (option == "csv_fieldsep") {
		if (value) {
			if (strlen(value) != 1) {
				return fail("csv_fieldsep must be a single one-byte character");
			}
			if (value[0] == '"' || value[0] == '\n' || value[0] == '\r') {
				return fail("csv_fieldsep cannot be a double quote, a newline, or a carriage return");
			}
			psql_csv_separator = value[0];
		}
		return info(StringUtil::Format("Field separator for CSV is \"%s\".", string(1, psql_csv_separator)));
	}
	if (option == "border") {
		if (value) {
			psql_border = StringToInt(value);
		}
		return info(StringUtil::Format("Border style is %lld.", static_cast<long long>(psql_border)));
	}
	if (option == "footer") {
		auto on = !psql_footer;
		if (value && !ParsePsqlBoolean(value, on)) {
			return boolean_expected();
		}
		psql_footer = on;
		return info(on ? "Default footer is on." : "Default footer is off.");
	}
	if (option == "pager") {
		auto pager = pager_mode == PagerMode::PAGER_AUTOMATIC ? PagerMode::PAGER_OFF : PagerMode::PAGER_AUTOMATIC;
		bool on = false;
		if (value && StringUtil::CIEquals(value, "always")) {
			pager = PagerMode::PAGER_ON;
		} else if (value && ParsePsqlBoolean(value, on)) {
			pager = on ? PagerMode::PAGER_AUTOMATIC : PagerMode::PAGER_OFF;
		} else if (value) {
			return fail(StringUtil::Format(
			    "unrecognized value \"%s\" for \"pager\"; available values are: on, off, always", value));
		}
		pager_mode = pager;
		if (pager == PagerMode::PAGER_ON && pager_command.empty()) {
			pager_command = GetSystemPager();
		}
		if (pager == PagerMode::PAGER_ON) {
			return info("Pager is always used.");
		}
		return info(pager == PagerMode::PAGER_OFF ? "Pager usage is off." : "Pager is used for long output.");
	}
	return fail("unknown option: " + name);
}

void ShellState::PrintPrintOptions() {
	auto on_off = [](bool on) {
		return string(on ? "on" : "off");
	};
	string pager = pager_mode == PagerMode::PAGER_OFF ? "0" : pager_mode == PagerMode::PAGER_ON ? "2" : "1";
	auto fieldsep_zero = IsZeroSeparator(colSeparator);
	auto recordsep_zero = IsZeroSeparator(rowSeparator);
	string settings;
	auto add = [&](const char *setting, const string &value) {
		settings += StringUtil::Format("%-24s %s\n", setting, value);
	};
	add("border", to_string(psql_border));
	add("csv_fieldsep", QuoteSetting(string(1, psql_csv_separator)));
	add("expanded", ExpandedName(*this));
	add("fieldsep", QuoteSetting(fieldsep_zero ? string() : colSeparator));
	add("fieldsep_zero", on_off(fieldsep_zero));
	add("footer", on_off(psql_footer));
	add("format", PrintFormatName(*this));
	add("null", QuoteSetting(mode == RenderMode::PSQL ? string(PsqlNullValue()) : nullValue));
	add("pager", pager);
	add("recordsep", QuoteSetting(recordsep_zero ? string() : rowSeparator));
	add("recordsep_zero", on_off(recordsep_zero));
	add("tuples_only", on_off(!showHeader));
	PrintInfo(settings);
}

void ShellState::PrintBackslashHelp() {
	auto currently = [](const char *value) {
		return StringUtil::Format(" (currently %s)", value);
	};
	string help;
	help += "General\n";
	AddHelpLine(help, "\\q", "quit");
	help += "\nHelp\n";
	AddHelpLine(help, "\\?", "show help on backslash commands");
	help += "\nQuery Buffer\n";
	AddHelpLine(help, "\\g [FILE] or ;", "execute query (and send result to file or |pipe);");
	AddHelpLine(help, "", "\\g with no arguments is equivalent to a semicolon");
	AddHelpLine(help, "\\gx [FILE]", "as \\g, but forces expanded output mode");
	help += "\nInput/Output\n";
	AddHelpLine(help, "\\copy ...", "perform SQL COPY with data stream to the client host");
	AddHelpLine(help, "\\echo [-n] [STRING]", "write string to standard output (-n for no newline)");
	AddHelpLine(help, "\\i FILE", "execute commands from file");
	AddHelpLine(help, "\\o [FILE]", "send all query results to file or |pipe");
	help += "\nInformational\n";
	AddHelpLine(help, "\\l", "list databases");
	help += "\nFormatting\n";
	AddHelpLine(help, "\\a", "toggle between unaligned and aligned output mode");
	AddHelpLine(help, "\\pset [NAME [VALUE]]", "set table output option");
	AddHelpLine(help, "", "(border|csv_fieldsep|expanded|fieldsep|fieldsep_zero|");
	AddHelpLine(help, "", "footer|format|null|pager|recordsep|recordsep_zero|");
	AddHelpLine(help, "", "tuples_only)");
	AddHelpLine(help, "\\t [on|off]", "show only rows" + currently(showHeader ? "off" : "on"));
	AddHelpLine(help, "\\x [on|off|auto]", "toggle expanded output" + currently(ExpandedName(*this)));
	help += "\nOperating System\n";
	AddHelpLine(help, "\\timing [on|off]", "toggle timing of commands" + currently(TimerEnabled() ? "on" : "off"));
	help += "\nThe dot commands work as well: .help lists them.\n";
	PrintInfo(help);
}

SuccessState ShellState::CopyClientSide(const string &arguments) {
	ClientCopy copy;
	string error;
	if (!ParseClientCopy(arguments, copy, error)) {
		PrintF(PrintOutput::STDERR, "\\copy: %s\n", error);
		return SuccessState::FAILURE;
	}
	FILE *stream = nullptr;
	FILE *file = nullptr;
	if (copy.target == ClientCopyTarget::FILE) {
		if (safe_mode) {
			PrintF(PrintOutput::STDERR, "\\copy cannot access files in -safe mode\n");
			return SuccessState::FAILURE;
		}
		auto path = duckdb::FileSystem::ExpandPath(copy.file, nullptr);
		file = fopen(path.c_str(), copy.from ? "rb" : "wb");
		if (!file) {
			PrintF(PrintOutput::STDERR, "%s: %s\n", copy.file, strerror(errno));
			return SuccessState::FAILURE;
		}
		stream = file;
	} else if (copy.target == ClientCopyTarget::PROCESS) {
		stream = copy.from ? stdin : stdout;
	}
	auto sql = "COPY " + copy.before + (copy.from ? " FROM STDIN " : " TO STDOUT ") + copy.after;
	if (copy.from) {
		copy_in_file = stream;
	} else {
		copy_out_file = stream;
	}
	auto result = ExecuteSQL(sql);
	copy_in_file = nullptr;
	copy_out_file = nullptr;
	if (file && fclose(file) != 0 && result == SuccessState::SUCCESS) {
		PrintF(PrintOutput::STDERR, "%s: %s\n", copy.file, strerror(errno));
		result = SuccessState::FAILURE;
	}
	if (stream == stdout) {
		fflush(stdout);
	}
	return result;
}

} // namespace duckdb_shell
