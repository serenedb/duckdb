#include "shell_docs.hpp"

#include "terminal.hpp"
#include "shell_highlight.hpp"
#include "shell_state.hpp"
#ifdef HAVE_LINENOISE
#include "linenoise.hpp"
#include <unistd.h>
#endif

#include <algorithm>

namespace duckdb_shell {

namespace {

DocsBackend &Backend() {
	static DocsBackend backend;
	return backend;
}

constexpr duckdb::idx_t kMinWidth = 40;
constexpr duckdb::idx_t kMaxWidth = 100;

bool CanOfferList(ShellState &state) {
#ifdef HAVE_LINENOISE
	return state.rl_version == ReadLineVersion::LINENOISE && state.stdin_is_interactive && state.stdout_is_console &&
	       state.out == stdout && state.outfile.empty() && !duckdb::Terminal::IsUnsupportedTerm();
#else
	return false;
#endif
}

void OfferList() {
#ifdef HAVE_LINENOISE
	if (duckdb::Terminal::HasMoreData(STDIN_FILENO) > 0) {
		return;
	}
	for (const char c : duckdb::string(".docs ")) {
		duckdb::KeyPress key;
		key.action = c;
		duckdb::BufferedKeyPresses::BufferKeyPress(key);
	}
	duckdb::BufferedKeyPresses::BufferKeyPress(duckdb::TAB);
#endif
}

} // namespace

void RegisterDocsBackend(DocsBackend backend) {
	Backend() = std::move(backend);
}

void LoadDocsBackend(duckdb::ClientContext &context) {
	if (Backend().load) {
		Backend().load(context);
	}
}

bool DocsCompletions(const char *line, duckdb::idx_t length, duckdb::idx_t &argument_start,
                     duckdb::vector<DocsCompletion> &completions) {
	if (!Backend().run || !Backend().complete) {
		return false;
	}
	const duckdb::string text(line, length);
	static constexpr const char *kPrefixes[] = {".docs ", ".doc "};
	for (auto *prefix : kPrefixes) {
		const duckdb::string marker(prefix);
		if (text.size() < marker.size() || text.compare(0, marker.size(), marker) != 0) {
			continue;
		}
		argument_start = marker.size();
		auto &state = ShellState::Get();
		completions = Backend().complete(state.db ? state.db->instance.get() : nullptr, text.substr(marker.size()));
		return true;
	}
	return false;
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
	request.interactive = CanOfferList(state);
	request.instance = state.db ? state.db->instance.get() : nullptr;
	if (state.max_width > 0) {
		request.width = state.max_width;
	} else if (state.stdout_is_console) {
		auto size = duckdb::Terminal::GetTerminalSize();
		auto columns = size.ws_col > 2 ? static_cast<duckdb::idx_t>(size.ws_col - 2) : kMinWidth;
		request.width = std::min(std::max(columns, kMinWidth), kMaxWidth);
	}

	duckdb::string rendered;
	const bool ok = Backend().run(request, rendered);
	if (request.interactive && Backend().offered && Backend().offered()) {
		OfferList();
	}
	if (!ok) {
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
