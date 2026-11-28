#include "shell_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb_shell {

namespace {

constexpr const char *COPY_STDIN_PATH = "/dev/stdin";
constexpr const char *COPY_STDOUT_PATH = "/dev/stdout";

class CopyStreamHandle : public duckdb::FileHandle {
public:
	CopyStreamHandle(duckdb::FileSystem &file_system, const string &path, duckdb::FileOpenFlags flags)
	    : FileHandle(file_system, path, flags), input(path == COPY_STDIN_PATH) {
	}

	void Close() override {
	}

	bool input;
};

ShellState &CopyShell() {
	auto state = ShellState::GetReference();
	if (!state) {
		throw duckdb::IOException("COPY STDIN and STDOUT need a running shell");
	}
	return *state;
}

class ShellCopyFileSystem : public duckdb::FileSystem {
public:
	string GetName() const override {
		return "ShellCopyFileSystem";
	}

	bool CanHandleFile(const string &path) override {
		if (path == COPY_STDOUT_PATH) {
			return true;
		}
		if (path != COPY_STDIN_PATH) {
			return false;
		}
		auto state = ShellState::GetReference();
		return !state || !state->CopyInputIsFile();
	}

	bool IsPipe(const string &path, optional_ptr<duckdb::FileOpener> opener) override {
		return CanHandleFile(path);
	}

	unique_ptr<duckdb::FileHandle> OpenFile(const string &path, duckdb::FileOpenFlags flags,
	                                        optional_ptr<duckdb::FileOpener> opener) override {
		if (!CanHandleFile(path)) {
			throw duckdb::IOException("Cannot open \"%s\"", path);
		}
		return make_uniq<CopyStreamHandle>(*this, path, flags);
	}

	int64_t Read(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		if (!handle.Cast<CopyStreamHandle>().input) {
			throw duckdb::IOException("Cannot read from \"%s\"", handle.path);
		}
		auto read = CopyShell().ReadCopyData(static_cast<char *>(buffer), duckdb::NumericCast<idx_t>(nr_bytes));
		return duckdb::NumericCast<int64_t>(read);
	}

	void Read(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		throw duckdb::IOException("Cannot read \"%s\" at a position", handle.path);
	}

	int64_t Write(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		if (handle.Cast<CopyStreamHandle>().input) {
			throw duckdb::IOException("Cannot write to \"%s\"", handle.path);
		}
		CopyShell().WriteCopyData(static_cast<const char *>(buffer), duckdb::NumericCast<idx_t>(nr_bytes));
		return nr_bytes;
	}

	void Write(duckdb::FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		throw duckdb::IOException("Cannot write \"%s\" at a position", handle.path);
	}

	duckdb::FileExistence TryFileExists(const string &path, optional_ptr<duckdb::FileOpener> opener,
	                                    optional_ptr<duckdb::ErrorData> error) override {
		return CanHandleFile(path) ? duckdb::FileExistence::EXISTS : duckdb::FileExistence::MISSING;
	}

	bool TryRemoveFile(const string &path, optional_ptr<duckdb::FileOpener> opener) override {
		return false;
	}

	vector<duckdb::OpenFileInfo> Glob(const string &path, duckdb::FileOpener *opener) override {
		vector<duckdb::OpenFileInfo> result;
		if (CanHandleFile(path)) {
			result.emplace_back(path);
		}
		return result;
	}

	int64_t GetFileSize(duckdb::FileHandle &handle) override {
		return 0;
	}

	duckdb::timestamp_t GetLastModifiedTime(duckdb::FileHandle &handle) override {
		return duckdb::timestamp_t(0);
	}

	duckdb::FileType GetFileType(duckdb::FileHandle &handle) override {
		return duckdb::FileType::FILE_TYPE_FIFO;
	}

	bool CanSeek() override {
		return false;
	}

	bool OnDiskFile(duckdb::FileHandle &handle) override {
		return false;
	}

	void FileSync(duckdb::FileHandle &handle) override {
	}

	void Seek(duckdb::FileHandle &handle, idx_t location) override {
		throw duckdb::IOException("Cannot seek in \"%s\"", handle.path);
	}

	void Reset(duckdb::FileHandle &handle) override {
		throw duckdb::IOException("Cannot reset \"%s\"", handle.path);
	}

	idx_t SeekPosition(duckdb::FileHandle &handle) override {
		return 0;
	}
};

} // namespace

void RegisterCopyFileSystem(duckdb::DatabaseInstance &db) {
	duckdb::FileSystem::GetFileSystem(db).RegisterSubSystem(make_uniq<ShellCopyFileSystem>());
}

} // namespace duckdb_shell
