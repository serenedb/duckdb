#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

constexpr const char *SNIFF_FAULT_PATH = "sniff-fault://rows.csv";

struct SniffFault {
	idx_t fail_at = NumericLimits<idx_t>::Maximum();
	atomic<idx_t> failures {0};
};

class SniffFaultFileSystem : public LocalFileSystem {
public:
	SniffFaultFileSystem(string local_path_p, SniffFault &fault_p)
	    : local_path(std::move(local_path_p)), fault(fault_p) {
	}

	string GetName() const override {
		return "SniffFaultFileSystem";
	}

	bool CanHandleFile(const string &path) override {
		return path == SNIFF_FAULT_PATH;
	}

	bool CanSeek() override {
		return false;
	}

	FileExistence TryFileExists(const string &path, optional_ptr<FileOpener> opener,
	                            optional_ptr<ErrorData> error) override {
		return LocalFileSystem::TryFileExists(local_path, opener, error);
	}

	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override {
		return {OpenFileInfo(path)};
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override {
		auto handle = LocalFileSystem::OpenFile(local_path, flags, opener);
		lock_guard<mutex> guard(lock);
		positions[handle.get()] = 0;
		return handle;
	}

	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		lock_guard<mutex> guard(lock);
		auto &position = positions[&handle];
		if (position + NumericCast<idx_t>(nr_bytes) > fault.fail_at && fault.failures++ == 0) {
			throw IOException("injected sniffer read failure");
		}
		const auto read = LocalFileSystem::Read(handle, buffer, nr_bytes);
		position += NumericCast<idx_t>(read);
		return read;
	}

private:
	const string local_path;
	SniffFault &fault;
	mutex lock;
	unordered_map<const FileHandle *, idx_t> positions;
};

} // namespace

TEST_CASE("A read failure while the sniffer parses its dialect candidates fails the query", "[csv]") {
	DuckDB db(nullptr);
	Connection con(db);
	const auto local_path = TestCreatePath("sniff_fault.csv");
	REQUIRE_NO_FAIL(
	    con.Query(StringUtil::Format("COPY (SELECT lpad(i::VARCHAR, 6, '0') AS a, lpad((2 * i)::VARCHAR, 6, "
	                                 "'0') AS b FROM range(10000) t(i)) TO '%s' (HEADER false)",
	                                 local_path)));
	SniffFault fault;
	FileSystem::GetFileSystem(*con.context).RegisterSubSystem(make_uniq<SniffFaultFileSystem>(local_path, fault));
	const auto query = StringUtil::Format(
	    "SELECT count(*), sum(column0::BIGINT) FROM read_csv('%s', header = false, buffer_size = 1024)",
	    SNIFF_FAULT_PATH);
	for (const idx_t threads : {1, 4}) {
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("SET threads = %llu", threads)));
		for (const idx_t fail_at : {8192, 40960}) {
			INFO("threads " << threads << ", failing read past byte " << fail_at);
			fault.fail_at = fail_at;
			fault.failures = 0;
			auto result = con.Query(query);
			INFO(result->ToString());
			REQUIRE(fault.failures > 0);
			REQUIRE(result->HasError());
			REQUIRE(StringUtil::Contains(result->GetError(), "injected sniffer read failure"));
		}
	}
	fault.fail_at = NumericLimits<idx_t>::Maximum();
	auto result = con.Query(query);
	REQUIRE(CHECK_COLUMN(result, 0, {10000}));
	REQUIRE(CHECK_COLUMN(result, 1, {49995000}));
}
