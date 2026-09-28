#include "shell_docs.hpp"

#include "terminal.hpp"
#include "shell_highlight.hpp"
#include "shell_state.hpp"

#include <algorithm>

namespace duckdb_shell {

namespace {

DocsBackend &Backend() {
	static DocsBackend backend;
	return backend;
}

constexpr duckdb::idx_t kMinWidth = 40;
constexpr duckdb::idx_t kMaxWidth = 100;

} // namespace

void RegisterDocsBackend(DocsBackend backend) {
	Backend() = std::move(backend);
}

void LoadDocsBackend(duckdb::ClientContext &context) {
	if (Backend().load) {
		Backend().load(context);
	}
}

MetadataResult ShowDocumentation(ShellState &state, const duckdb::vector<duckdb::string> &args) {
	if (!Backend().run) {
		state.Print(PrintOutput::STDERR, "Documentation is not available in this build.\n");
		return MetadataResult::FAIL;
	}

	DocsRequest request;
	for (duckdb::idx_t i = 1; i < args.size(); i++) {
		request.args.push_back(args[i]);
	}

	request.color = ShellHighlight::IsEnabled() && state.stdout_is_console;
	request.instance = state.db ? state.db->instance.get() : nullptr;
	if (state.max_width > 0) {
		request.width = state.max_width;
	} else if (state.stdout_is_console) {
		auto size = duckdb::Terminal::GetTerminalSize();
		auto columns = size.ws_col > 2 ? static_cast<duckdb::idx_t>(size.ws_col - 2) : kMinWidth;
		request.width = std::min(std::max(columns, kMinWidth), kMaxWidth);
	}

	duckdb::string rendered;
	if (!Backend().run(request, rendered)) {
		state.Print(PrintOutput::STDERR, rendered);
		return MetadataResult::FAIL;
	}

	duckdb::idx_t line_count = 0;
	duckdb::idx_t widest = 0;
	for (duckdb::idx_t begin = 0; begin < rendered.size();) {
		auto end = rendered.find('\n', begin);
		if (end == duckdb::string::npos) {
			end = rendered.size();
		}
		widest = std::max(widest, ShellState::RenderLength(rendered.c_str() + begin, end - begin));
		line_count++;
		begin = end + 1;
	}

	duckdb::unique_ptr<PagerState> pager;
	if (state.ShouldUsePagerForSize(line_count, widest)) {
		pager = state.SetupPager();
	}
	state.Print(PrintOutput::STDOUT, rendered);
	return MetadataResult::SUCCESS;
}

} // namespace duckdb_shell
