#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/handle_file_writer.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/memory_file_system.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"

namespace duckdb {

namespace {

constexpr idx_t MEMORY_BLOCK = ExternalFileCache::DEFAULT_BLOCK_SIZE;

uint8_t ByteAt(idx_t offset) {
	return static_cast<uint8_t>((offset * 13 + offset / MEMORY_BLOCK) & 0xFF);
}

bool HoldsBytes(const_data_ptr_t data, idx_t offset, idx_t size) {
	for (idx_t i = 0; i < size; i++) {
		if (data[i] != ByteAt(offset + i)) {
			return false;
		}
	}
	return true;
}

//! Fills [writer] with the bytes up to [size] through its windows, committing a varying share of each
void FillWriter(FileWriter &writer, idx_t size) {
	for (idx_t written = 0; written < size;) {
		const auto window = writer.Window();
		REQUIRE(window.size != 0);
		const auto count = MinValue(window.size, MinValue(size - written, 1000 + written % 7000));
		for (idx_t i = 0; i < count; i++) {
			window.data[i] = ByteAt(written + i);
		}
		writer.Commit(count, 0);
		written += count;
	}
}

void WriteFile(FileSystem &fs, const string &path, idx_t size) {
	auto writer = fs.OpenFileWriter(path);
	FillWriter(*writer, size);
	writer->Close();
}

bool ReadsBack(FileSystem &fs, const string &path, idx_t size) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	if (NumericCast<idx_t>(handle->GetFileSize()) != size) {
		return false;
	}
	vector<uint8_t> data(size);
	handle->Read(data.data(), size, 0);
	if (!HoldsBytes(data.data(), 0, size)) {
		return false;
	}
	const auto offset = size / 3;
	const auto length = MinValue<idx_t>(3 * MEMORY_BLOCK, size - offset);
	vector<uint8_t> middle(length);
	handle->Read(middle.data(), length, offset);
	return HoldsBytes(middle.data(), offset, length);
}

} // namespace

TEST_CASE("Memory files read back what was written", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem blocks(*db.instance);
	MemoryFileSystem heap;
	for (auto fs : {&blocks, &heap}) {
		for (const idx_t size :
		     {idx_t(0), idx_t(1), MEMORY_BLOCK - 1, MEMORY_BLOCK, MEMORY_BLOCK + 1, 5 * MEMORY_BLOCK + 123}) {
			const auto path = string("memory://files/f") + to_string(size);
			WriteFile(*fs, path, size);
			REQUIRE(ReadsBack(*fs, path, size));
		}
	}
	auto &vfs = FileSystem::GetFileSystem(*db.instance);
	WriteFile(vfs, "memory://registered/data.bin", 3 * MEMORY_BLOCK + 7);
	REQUIRE(ReadsBack(vfs, "memory://registered/data.bin", 3 * MEMORY_BLOCK + 7));
}

TEST_CASE("Memory files are written through a writer only", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem fs(*db.instance);
	WriteFile(fs, "memory://once.bin", 100);
	REQUIRE_THROWS(fs.OpenFile("memory://once.bin", FileFlags::FILE_FLAGS_WRITE));
	REQUIRE_THROWS(fs.OpenFile("memory://other.bin", FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE));
	REQUIRE(!fs.FileExists("memory://other.bin"));
	ErrorData missing;
	try {
		fs.OpenFile("memory://missing.bin", FileFlags::FILE_FLAGS_READ);
	} catch (std::exception &ex) {
		missing = ErrorData(ex);
	}
	REQUIRE(missing.HasError());
	REQUIRE(missing.ExtraInfo().at("errno") == std::to_string(ENOENT));
	REQUIRE(
	    !fs.OpenFile("memory://missing.bin", FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS));
}

TEST_CASE("Memory file handles read, seek and report as a file handle does", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem fs(*db.instance);
	WriteFile(fs, "memory://handle/data.bin", 100);
	REQUIRE(fs.FileExists("memory://handle/data.bin"));
	REQUIRE(fs.DirectoryExists("memory://handle/"));
	auto handle = fs.OpenFile("memory://handle/data.bin", FileFlags::FILE_FLAGS_READ);
	REQUIRE(handle->CanSeek());
	REQUIRE(!handle->OnDiskFile());
	REQUIRE(fs.GetLastModifiedTime(*handle).IsFinite());
	handle->Seek(90);
	REQUIRE(handle->SeekPosition() == 90);
	uint8_t data[20];
	REQUIRE(handle->Read(data, 20) == 10);
	REQUIRE(HoldsBytes(data, 90, 10));
	REQUIRE(handle->Read(data, 20) == 0);
	REQUIRE_THROWS(handle->Read(data, 20, 90));
	REQUIRE_THROWS(handle->Truncate(0));
	REQUIRE_THROWS(fs.RemoveFile("memory://handle/missing.bin"));
}

TEST_CASE("Memory mappings are read-only views that take hints and close", "[memory_file_system]") {
	MemoryFileSystem heap;
	WriteFile(heap, "memory://mapped/heap.bin", 100);
	REQUIRE_THROWS(heap.MemoryMapFile(OpenFileInfo("memory://mapped/heap.bin"),
	                                  FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE, MMapOptions()));
	REQUIRE(!heap.MemoryMapFile(OpenFileInfo("memory://mapped/missing.bin"),
	                            FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS, MMapOptions()));
	auto mapped =
	    heap.MemoryMapFile(OpenFileInfo("memory://mapped/heap.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions());
	mapped->Prefetch(0, 100);
	mapped->Evict(0, 100);
	mapped->Sync();
	REQUIRE(!mapped->Trim(0, 100));
	REQUIRE(HoldsBytes(mapped->GetData(0, 100), 0, 100));
	mapped->Close();
	REQUIRE(mapped->Size() == 0);
}

TEST_CASE("Memory files are filled in place through a writer", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem blocks(*db.instance);
	MemoryFileSystem heap;
	const idx_t size = 3 * MEMORY_BLOCK + 123;
	for (auto fs : {&blocks, &heap}) {
		auto writer = fs->OpenFileWriter("memory://writer.bin");
		REQUIRE(writer);
		FillWriter(*writer, size);
		REQUIRE_THROWS(fs->Compact({"memory://writer.bin"}));
		writer->Close();
		writer->Close();
		REQUIRE(ReadsBack(*fs, "memory://writer.bin", size));
	}
	auto &vfs = FileSystem::GetFileSystem(*db.instance);
	auto forwarded = vfs.OpenFileWriter("memory://forwarded.bin");
	REQUIRE(forwarded);
	FillWriter(*forwarded, MEMORY_BLOCK + 1);
	forwarded->Close();
	REQUIRE(ReadsBack(vfs, "memory://forwarded.bin", MEMORY_BLOCK + 1));
}

TEST_CASE("A file system that writes through handles fills a file through a writer too", "[memory_file_system]") {
	LocalFileSystem local;
	const auto path = TestCreatePath("handle_writer.bin");
	const idx_t windows = 3 * HandleFileWriter::BUFFER_SIZE + 123;
	const idx_t direct = 2 * HandleFileWriter::BUFFER_SIZE + 7;
	auto writer = local.OpenFileWriter(path);
	REQUIRE(writer);
	FillWriter(*writer, windows);
	vector<uint8_t> owned(direct);
	for (idx_t i = 0; i < direct; i++) {
		owned[i] = ByteAt(windows + i);
	}
	REQUIRE(writer->Append(owned.data(), direct));
	writer->Close();
	REQUIRE(ReadsBack(local, path, windows + direct));
}

TEST_CASE("An aborted write of a memory file leaves no file behind", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem blocks(*db.instance);
	MemoryFileSystem heap;
	uint8_t byte = 1;
	for (auto fs : {&blocks, &heap}) {
		auto aborted = fs->OpenFileWriter("memory://aborted.bin");
		FillWriter(*aborted, MEMORY_BLOCK + 1);
		aborted->Abort();
		REQUIRE(!fs->FileExists("memory://aborted.bin"));

		{
			auto dropped = fs->OpenFileWriter("memory://dropped.bin");
			FillWriter(*dropped, 10);
		}
		REQUIRE(!fs->FileExists("memory://dropped.bin"));

		auto replaced = fs->OpenFileWriter("memory://replaced.bin");
		WriteFile(*fs, "memory://replaced.bin", 10);
		replaced->Abort();
		REQUIRE(ReadsBack(*fs, "memory://replaced.bin", 10));

		auto reader = fs->OpenFile("memory://replaced.bin", FileFlags::FILE_FLAGS_READ);
		REQUIRE_THROWS(reader->Write(&byte, 1));
	}
}

TEST_CASE("Heap memory files map in place, block files do not", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem blocks(*db.instance);
	MemoryFileSystem heap;

	WriteFile(blocks, "memory://small.bin", MEMORY_BLOCK - 10);
	REQUIRE(blocks.OpenBlockFile("memory://small.bin"));
	REQUIRE_THROWS(blocks.MemoryMapFile(OpenFileInfo("memory://small.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions()));

	WriteFile(heap, "memory://heap.bin", 3 * MEMORY_BLOCK);
	auto mapped = heap.MemoryMapFile(OpenFileInfo("memory://heap.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions());
	REQUIRE(mapped);
	REQUIRE(mapped->Size() == 3 * MEMORY_BLOCK);
	REQUIRE(HoldsBytes(mapped->GetData(0, mapped->Size()), 0, 3 * MEMORY_BLOCK));
	REQUIRE(!heap.OpenBlockFile("memory://heap.bin"));

	heap.RemoveFile("memory://heap.bin");
	REQUIRE(HoldsBytes(mapped->GetData(0, mapped->Size()), 0, 3 * MEMORY_BLOCK));

	WriteFile(heap, "memory://empty.bin", 0);
	auto empty = heap.MemoryMapFile(OpenFileInfo("memory://empty.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions());
	REQUIRE(empty);
	REQUIRE(empty->Size() == 0);

	auto writer = heap.OpenFileWriter("memory://growing.bin");
	FillWriter(*writer, 1);
	REQUIRE_THROWS(heap.MemoryMapFile(OpenFileInfo("memory://growing.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions()));
	auto reader = heap.OpenFile("memory://growing.bin", FileFlags::FILE_FLAGS_READ);
	uint8_t byte = 0;
	REQUIRE_THROWS(reader->Read(&byte, 1, 0));
	writer->Close();
	auto grown = heap.MemoryMapFile(OpenFileInfo("memory://growing.bin"), FileFlags::FILE_FLAGS_READ, MMapOptions());
	REQUIRE(grown->Size() == 1);
}

TEST_CASE("Memory files added over bytes they borrow read and map those bytes in place", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem blocks(*db.instance);
	MemoryFileSystem heap;
	vector<uint8_t> bytes(3 * MEMORY_BLOCK);
	for (idx_t i = 0; i < bytes.size(); i++) {
		bytes[i] = ByteAt(i);
	}
	const string path = "memory://borrowed/index.bin";
	for (auto &fs : vector<reference<MemoryFileSystem>> {blocks, heap}) {
		fs.get().AddFile(path, bytes.data(), bytes.size());
		REQUIRE(!fs.get().OpenBlockFile(path));
		REQUIRE(ReadsBack(fs.get(), path, bytes.size()));
		auto mapped = fs.get().MemoryMapFile(OpenFileInfo(path), FileFlags::FILE_FLAGS_READ, MMapOptions());
		REQUIRE(mapped->Size() == bytes.size());
		REQUIRE(mapped->GetData(0, mapped->Size()) == bytes.data());
		vector<string> listed;
		fs.get().ListFiles("memory://borrowed", [&](const string &name, bool) { listed.push_back(name); });
		REQUIRE(listed == vector<string> {"index.bin"});
		REQUIRE_THROWS(fs.get().OpenFile(path, FileFlags::FILE_FLAGS_WRITE));
		fs.get().RemoveFile(path);
		REQUIRE(!fs.get().FileExists(path));
	}
}

TEST_CASE("Compacted memory files map in place", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem fs(*db.instance);
	auto &cache = ExternalFileCache::Get(*db.instance);
	for (const idx_t size : {idx_t(0), idx_t(1), MEMORY_BLOCK, 5 * MEMORY_BLOCK + 123}) {
		const auto path = "memory://compact/f" + to_string(size);
		WriteFile(fs, path, size);
		auto blocks = fs.OpenBlockFile(path);
		REQUIRE(blocks);
		fs.Compact({path});
		REQUIRE(!fs.OpenBlockFile(path));
		REQUIRE(ReadsBack(fs, path, size));
		auto mapped = fs.MemoryMapFile(OpenFileInfo(path), FileFlags::FILE_FLAGS_READ, MMapOptions());
		REQUIRE(mapped);
		REQUIRE(mapped->Size() == size);
		if (size == 0) {
			continue;
		}
		REQUIRE(HoldsBytes(mapped->GetData(0, size), 0, size));
		auto pin = cache.Pin(*blocks, 0);
		REQUIRE(HoldsBytes(pin.Ptr(), 0, MinValue(size, MEMORY_BLOCK)));
		fs.Compact({path});
		fs.RemoveFile(path);
		REQUIRE(HoldsBytes(mapped->GetData(0, size), 0, size));
	}
	auto writer = fs.OpenFileWriter("memory://compact/open");
	REQUIRE_THROWS(fs.Compact({"memory://compact/open"}));
	REQUIRE_THROWS(fs.Compact({"memory://compact/missing"}));

	MemoryFileSystem heap;
	WriteFile(heap, "memory://heap.bin", MEMORY_BLOCK + 1);
	heap.Compact({"memory://heap.bin"});
	REQUIRE(ReadsBack(heap, "memory://heap.bin", MEMORY_BLOCK + 1));
}

TEST_CASE("Memory files compact together, all or none", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem fs(*db.instance);
	const string closed = "memory://together/closed";
	const string open = "memory://together/open";
	WriteFile(fs, closed, MEMORY_BLOCK + 1);
	auto writer = fs.OpenFileWriter(open);
	FillWriter(*writer, 2 * MEMORY_BLOCK);
	REQUIRE_THROWS(fs.Compact({closed, open}));
	REQUIRE_THROWS(fs.Compact({closed, "memory://together/missing"}));
	REQUIRE(fs.OpenBlockFile(closed));
	writer->Close();
	fs.Compact({closed, open});
	REQUIRE(!fs.OpenBlockFile(closed));
	REQUIRE(!fs.OpenBlockFile(open));
	REQUIRE(ReadsBack(fs, closed, MEMORY_BLOCK + 1));
	REQUIRE(ReadsBack(fs, open, 2 * MEMORY_BLOCK));
}

TEST_CASE("The virtual filesystem finds and compacts the block files of memory files", "[memory_file_system]") {
	DuckDB db(nullptr);
	auto &vfs = FileSystem::GetFileSystem(*db.instance);
	const string path = "memory://forwarded/file";
	WriteFile(vfs, path, MEMORY_BLOCK + 1);
	REQUIRE(vfs.OpenBlockFile(path));
	vfs.Compact({path});
	REQUIRE(!vfs.OpenBlockFile(path));
	REQUIRE(ReadsBack(vfs, path, MEMORY_BLOCK + 1));

	auto local = TestCreatePath("not_in_blocks.bin");
	WriteFile(vfs, local, 1);
	REQUIRE(!vfs.OpenBlockFile(local));
	vfs.Compact({local});
	REQUIRE(ReadsBack(vfs, local, 1));
}

TEST_CASE("A memory file counts as the external file cache's memory before and after its compaction",
          "[memory_file_system]") {
	DuckDB db(nullptr);
	auto &memory = FileSystem::GetFileSystem(*db.instance);
	auto &cache = ExternalFileCache::Get(*db.instance);
	const string path = "memory://tagged/file";
	constexpr idx_t size = 4 * MEMORY_BLOCK;
	const auto before = cache.GetStatistics().used;
	WriteFile(memory, path, size);
	REQUIRE(cache.GetStatistics().used - before >= size);
	memory.Compact({path});
	REQUIRE(cache.GetStatistics().used - before >= size);
	memory.RemoveFile(path);
	REQUIRE(cache.GetStatistics().used == before);
}

TEST_CASE("Compacted memory files spill while nothing maps them", "[memory_file_system]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='48MB'"));
	REQUIRE_NO_FAIL(con.Query("SET temp_directory='" + TestCreatePath("memory_file_compact_spill") + "'"));
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	constexpr idx_t size = 64 * MEMORY_BLOCK + 5;
	for (idx_t i = 0; i < 3; i++) {
		const auto path = "memory://compact_spill/f" + to_string(i);
		WriteFile(fs, path, size);
		fs.Compact({path});
	}
	for (idx_t i = 0; i < 3; i++) {
		const auto path = "memory://compact_spill/f" + to_string(i);
		auto mapped = fs.MemoryMapFile(OpenFileInfo(path), FileFlags::FILE_FLAGS_READ, MMapOptions());
		REQUIRE(HoldsBytes(mapped->GetData(0, size), 0, size));
	}
}

TEST_CASE("Memory directories are the prefixes of files, which list and remove them", "[memory_file_system]") {
	DuckDB db(nullptr);
	MemoryFileSystem fs(*db.instance);
	WriteFile(fs, "memory://index/a", 10);
	WriteFile(fs, "memory://index/b", 20);
	WriteFile(fs, "memory://index/sub/c", 30);
	fs.CreateDirectory("memory://index/empty");
	REQUIRE(fs.DirectoryExists("memory://index"));
	REQUIRE(fs.DirectoryExists("memory://index/sub/"));
	REQUIRE(!fs.DirectoryExists("memory://index/empty"));
	REQUIRE(!fs.DirectoryExists("memory://other"));

	vector<pair<string, bool>> entries;
	REQUIRE(
	    fs.ListFiles("memory://index", [&](const string &name, bool is_dir) { entries.emplace_back(name, is_dir); }));
	std::sort(entries.begin(), entries.end());
	REQUIRE(entries == vector<pair<string, bool>> {{"a", false}, {"b", false}, {"sub", true}});
	REQUIRE(!fs.ListFiles("memory://other", [&](const string &, bool) {}));
	REQUIRE_THROWS(fs.MoveFile("memory://index/a", "memory://index/z"));

	auto reader = fs.OpenFile("memory://index/a", FileFlags::FILE_FLAGS_READ);
	fs.RemoveFile("memory://index/a");
	REQUIRE(!fs.FileExists("memory://index/a"));
	vector<uint8_t> data(10);
	reader->Read(data.data(), 10, 0);
	REQUIRE(HoldsBytes(data.data(), 0, 10));

	fs.RemoveDirectory("memory://index");
	REQUIRE(!fs.DirectoryExists("memory://index"));
	REQUIRE(!fs.FileExists("memory://index/sub/c"));
}

TEST_CASE("Memory files larger than the memory limit spill and read back", "[memory_file_system]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET temp_directory='" + TestCreatePath("memory_file_spill") + "'"));
	REQUIRE_NO_FAIL(con.Query("SET memory_limit='1MB'"));
	auto &fs = FileSystem::GetFileSystem(*db.instance);
	WriteFile(fs, "memory://spill/big.bin", 16 * MEMORY_BLOCK + 99);
	auto &cache = ExternalFileCache::Get(*db.instance);
	REQUIRE(cache.GetStatistics().spilled > 0);
	REQUIRE(ReadsBack(fs, "memory://spill/big.bin", 16 * MEMORY_BLOCK + 99));

	fs.RemoveFile("memory://spill/big.bin");
	REQUIRE(cache.GetStatistics().used == 0);
	REQUIRE(BufferManager::GetBufferManager(*db.instance).GetUsedSwap() == 0);
}

} // namespace duckdb
