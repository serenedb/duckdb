#include "duckdb/storage/block_copy_store.hpp"

#include "duckdb/common/checksum.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/temporary_file_manager.hpp"

#include <algorithm>

namespace duckdb {

namespace {

//! What a copy starts with; the buffer's allocation follows, and the checksum covers it
struct CopyHeader {
	static constexpr uint64_t MAGIC = 0x3159504F434B4C42ULL;
	uint64_t magic;
	uint64_t alloc_size;
	uint64_t header_size;
	uint64_t checksum;
};

} // namespace

BlockCopyStore::BlockCopyStore(DatabaseInstance &db_p, const string &temp_directory_p, TemporaryFileManager &files_p,
                               string staging_prefix_p)
    : db(db_p), files(files_p), temp_directory(temp_directory_p),
      directory(FileSystem::GetFileSystem(db_p).JoinPath(temp_directory_p, DIRECTORY)),
      staging_prefix(std::move(staging_prefix_p)) {
}

string BlockCopyStore::PathOf(const BlockCopyName &name) const {
	return FileSystem::GetFileSystem(db).JoinPath(directory, name.ToString());
}

void BlockCopyStore::Load(idx_t limit) {
	auto &fs = FileSystem::GetFileSystem(db);
	if (!fs.DirectoryExists(directory)) {
		return;
	}
	struct Found {
		BlockCopyName name;
		idx_t size;
		timestamp_t modified;
	};
	vector<Found> found;
	vector<string> strays;
	fs.ListFiles(directory, [&](const string &file_name, bool is_dir) {
		if (is_dir) {
			return;
		}
		const auto path = fs.JoinPath(directory, file_name);
		BlockCopyName name;
		const auto stats =
		    BlockCopyName::TryParse(file_name, name) ? fs.GetStatsIfExists(path) : optional<FileMetadata>();
		if (!stats || stats->file_size < int64_t(sizeof(CopyHeader))) {
			strays.push_back(path);
			return;
		}
		found.push_back({name, NumericCast<idx_t>(stats->file_size), stats->last_modification_time});
	});
	std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.modified < b.modified; });
	idx_t total = 0;
	for (auto &copy : found) {
		total += copy.size;
	}
	const auto room = files.GetMaxSwapSpace().GetIndex() -
	                  MinValue(files.GetMaxSwapSpace().GetIndex(), files.GetTotalUsedSpaceInBytes());
	idx_t kept = 0;
	for (; kept < found.size() && total > MinValue(limit, room); kept++) {
		total -= found[kept].size;
		strays.push_back(PathOf(found[kept].name));
	}
	for (auto &path : strays) {
		fs.TryRemoveFile(path);
	}
	files.IncreaseSizeOnDisk(total);
	lock_guard<mutex> guard(lock);
	has_directory = true;
	for (; kept < found.size(); kept++) {
		auto &copy = found[kept];
		entries.emplace(copy.name, Entry {copy.size, 0, 0, unheld.insert(unheld.end(), copy.name)});
	}
	bytes = total;
}

void BlockCopyStore::HoldLocked(Entries::iterator entry) {
	if (entry->second.holds++ == 0 && entry->second.unheld != unheld.end()) {
		unheld.erase(entry->second.unheld);
		entry->second.unheld = unheld.end();
	}
}

void BlockCopyStore::EraseLocked(Entries::iterator entry) {
	if (entry->second.unheld != unheld.end()) {
		unheld.erase(entry->second.unheld);
	}
	bytes -= entry->second.size;
	files.DecreaseSizeOnDisk(entry->second.size);
	entries.erase(entry);
}

template <class FITS>
void BlockCopyStore::MakeRoomLocked(FITS &&fits) {
	auto &fs = FileSystem::GetFileSystem(db);
	while (!fits() && !unheld.empty()) {
		const auto name = unheld.front();
		EraseLocked(entries.find(name));
		fs.TryRemoveFile(PathOf(name));
	}
}

bool BlockCopyStore::Hold(const BlockCopyName &name) {
	lock_guard<mutex> guard(lock);
	const auto entry = entries.find(name);
	if (entry == entries.end() || entry->second.write != 0) {
		return false;
	}
	HoldLocked(entry);
	return true;
}

void BlockCopyStore::Release(const BlockCopyName &name) {
	lock_guard<mutex> guard(lock);
	const auto entry = entries.find(name);
	if (entry == entries.end() || entry->second.holds == 0) {
		return;
	}
	if (--entry->second.holds == 0) {
		entry->second.unheld = unheld.insert(unheld.end(), name);
	}
}

bool BlockCopyStore::TryWrite(QueryContext context, const BlockCopyName &name, FileBuffer &buffer, idx_t limit) {
	auto &fs = FileSystem::GetFileSystem(db);
	const auto size = sizeof(CopyHeader) + buffer.AllocSize();
	const auto max_swap = files.GetMaxSwapSpace().GetIndex();
	idx_t write;
	{
		lock_guard<mutex> guard(lock);
		const auto entry = entries.find(name);
		if (entry != entries.end()) {
			// another block of the same bytes wrote them, or is writing them
			HoldLocked(entry);
			return true;
		}
		const auto fits = [&] {
			return bytes + size <= limit && files.GetTotalUsedSpaceInBytes() + size <= max_swap;
		};
		MakeRoomLocked(fits);
		if (!fits()) {
			return false;
		}
		if (!has_directory) {
			fs.CreateDirectory(directory);
			has_directory = true;
		}
		files.IncreaseSizeOnDisk(size);
		bytes += size;
		write = ++writes;
		entries.emplace(name, Entry {size, 1, write, unheld.end()});
	}
	const auto path = PathOf(name);
	const auto staging = fs.JoinPath(temp_directory, staging_prefix + "copy-" + to_string(write));
	const auto finish = [&](bool written) {
		lock_guard<mutex> guard(lock);
		const auto entry = entries.find(name);
		const bool mine = entry != entries.end() && entry->second.write == write;
		if (written && mine) {
			entry->second.write = 0;
			return;
		}
		if (mine) {
			EraseLocked(entry);
		}
		if (entry == entries.end() || mine) {
			// nothing names the file: the copy went while it was written, or the write failed
			fs.TryRemoveFile(path);
		}
	};
	try {
		const CopyHeader header {CopyHeader::MAGIC, buffer.AllocSize(), buffer.GetHeaderSize(),
		                         Checksum(buffer.InternalBuffer(), buffer.AllocSize())};
		auto handle = fs.OpenFile(staging, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		handle->Write(context, const_cast<CopyHeader *>(&header), sizeof(header), 0); // NOLINT: written, not changed
		buffer.Write(context, *handle, sizeof(header));
		handle->Close();
		fs.MoveFile(staging, path);
	} catch (...) {
		fs.TryRemoveFile(staging);
		finish(false);
		throw;
	}
	finish(true);
	return true;
}

unique_ptr<FileBuffer> BlockCopyStore::Read(QueryContext context, const BlockCopyName &name, FileBufferType type,
                                            idx_t alloc_size, unique_ptr<FileBuffer> reusable_buffer) {
	auto &fs = FileSystem::GetFileSystem(db);
	unique_ptr<FileBuffer> result;
	try {
		auto handle = fs.OpenFile(PathOf(name), FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
		CopyHeader header;
		if (handle && handle->GetFileSize() == sizeof(header) + alloc_size) {
			handle->Read(context, &header, sizeof(header), 0);
			if (header.magic == CopyHeader::MAGIC && header.alloc_size == alloc_size &&
			    header.header_size <= alloc_size) {
				auto buffer = BufferManager::GetBufferManager(db).ConstructManagedBuffer(
				    alloc_size - header.header_size, header.header_size, std::move(reusable_buffer), type);
				if (buffer->AllocSize() == alloc_size) {
					buffer->Read(context, *handle, sizeof(header));
					if (Checksum(buffer->InternalBuffer(), alloc_size) == header.checksum) {
						result = std::move(buffer);
					}
				}
			}
		}
	} catch (IOException &) { // NOLINT: a copy that cannot be read is fetched again
	}
	if (!result) {
		Remove(name);
	}
	return result;
}

void BlockCopyStore::Remove(const BlockCopyName &name) {
	lock_guard<mutex> guard(lock);
	const auto entry = entries.find(name);
	if (entry != entries.end()) {
		EraseLocked(entry);
	}
	FileSystem::GetFileSystem(db).TryRemoveFile(PathOf(name));
}

idx_t BlockCopyStore::Bytes() const {
	lock_guard<mutex> guard(lock);
	return bytes;
}

} // namespace duckdb
