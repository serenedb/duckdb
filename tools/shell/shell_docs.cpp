#include "shell_docs.hpp"

#include "terminal.hpp"
#include "shell_highlight.hpp"
#include "shell_state.hpp"

#include <algorithm>

namespace duckdb_shell {

namespace {

DocsBackend &BackendStorage() {
	static DocsBackend backend;
	return backend;
}

constexpr duckdb::idx_t kMinWidth = 40;
constexpr duckdb::idx_t kMaxWidth = 100;

} // namespace

void RegisterDocsBackend(DocsBackend backend) {
	BackendStorage() = std::move(backend);
}

bool HasDocsBackend() {
	return static_cast<bool>(BackendStorage().run);
}

const DocsBackend &GetDocsBackend() {
	return BackendStorage();
}

bool DocsCompletions(const char *line, duckdb::idx_t length, duckdb::idx_t &argument_start,
                     duckdb::vector<duckdb::string> &completions) {
	if (!HasDocsBackend() || !GetDocsBackend().complete) {
		return false;
	}
	const duckdb::string text(line, length);
	static constexpr const char *kPrefixes[] = {".docs ", ".doc "};
	for (auto *prefix : kPrefixes) {
		const duckdb::string marker(prefix);
		if (text.size() < marker.size() || text.compare(0, marker.size(), marker) != 0) {
			continue;
		}
		auto argument = text.substr(marker.size());
		if (argument.find(' ') != duckdb::string::npos) {
			return false;
		}
		argument_start = marker.size();
		completions = GetDocsBackend().complete(argument);
		return true;
	}
	return false;
}

MetadataResult ShowDocumentation(ShellState &state, const duckdb::vector<duckdb::string> &args) {
	if (!HasDocsBackend()) {
		state.Print(PrintOutput::STDERR, "Documentation is not available in this build.\n");
		return MetadataResult::FAIL;
	}

	DocsRequest request;
	for (duckdb::idx_t i = 1; i < args.size(); i++) {
		request.args.push_back(args[i]);
	}

	request.color = ShellHighlight::IsEnabled() && state.stdout_is_console;
	if (!state.psql_dbname.empty()) {
		request.query = [&state](const duckdb::string &sql, duckdb::string &out) {
			return state.ExecuteSQLSingleValue(sql, out) == ExecuteSQLSingleValueResult::SUCCESS;
		};
	}
	if (state.max_width > 0) {
		request.width = state.max_width;
	} else if (state.stdout_is_console) {
		auto size = duckdb::Terminal::GetTerminalSize();
		auto columns = size.ws_col > 2 ? static_cast<duckdb::idx_t>(size.ws_col - 2) : kMinWidth;
		request.width = std::min(std::max(columns, kMinWidth), kMaxWidth);
	} else {
		request.width = 80;
	}

	duckdb::string rendered;
	const bool ok = GetDocsBackend().run(request, rendered);
	if (!ok) {
		state.Print(PrintOutput::STDERR, rendered);
		return MetadataResult::FAIL;
	}

	duckdb::idx_t line_count = 0;
	duckdb::idx_t widest = 0;
	duckdb::idx_t current = 0;
	for (auto c : rendered) {
		if (c == '\n') {
			line_count++;
			widest = std::max(widest, current);
			current = 0;
		} else {
			current++;
		}
	}
	widest = std::max(widest, current);

	duckdb::unique_ptr<PagerState> pager;
	if (state.ShouldUsePagerForSize(line_count, widest)) {
		pager = state.SetupPager();
	}
	state.Print(PrintOutput::STDOUT, rendered);
	return MetadataResult::SUCCESS;
}

} // namespace duckdb_shell
