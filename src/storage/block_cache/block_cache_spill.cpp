#include "duckdb/storage/block_cache/block_cache_spill.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"

namespace duckdb {

namespace {

constexpr idx_t GROW_SLOTS = 64;

} // namespace

BlockCacheSpill::~BlockCacheSpill() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	try {
		Close();
	} catch (...) { // NOLINT
	}
}

void BlockCacheSpill::Close() {
	if (!handle) {
		return;
	}
	auto path = handle->GetPath();
	auto &fs = handle->file_system;
	handle.reset();
	if (temporary_files) {
		temporary_files->DecreaseSizeOnDisk(extent * BlockCache::BLOCK_SIZE);
		temporary_files = nullptr;
	}
	extent = 0;
	fs.TryRemoveFile(path);
}

void BlockCacheSpill::Configure(FileSystem &fs, const string &path, idx_t backed_limit_p) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (path.empty() && (!handle || is_default)) {
		return;
	}
	backed_limit = backed_limit_p / BlockCache::BLOCK_SIZE;
	if (handle && handle->GetPath() == path) {
		is_default = false;
		while (backed_order.size() > backed_limit && DropOldestBacked()) {
		}
		return;
	}
	for (auto &slot : slots) {
		if (slot.second.keep) {
			throw InvalidInputException(
			    "Cannot move the block cache spill file while it holds blocks of in-memory files");
		}
	}
	Close();
	slots.clear();
	free_slots.clear();
	backed_order.clear();
	is_default = false;
	if (path.empty()) {
		return;
	}
	Open(fs, path);
}

void BlockCacheSpill::ConfigureDefault(FileSystem &fs, TemporaryFileManager &temporary_files_p) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (handle) {
		return;
	}
	backed_limit = 0;
	Open(fs, temporary_files_p.TemporaryFilePath("block_cache.spill"));
	is_default = true;
	temporary_files = &temporary_files_p;
}

void BlockCacheSpill::Open(FileSystem &fs, const string &path) {
	const auto flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
	try {
		handle = fs.OpenFile(path, flags | FileFlags::FILE_FLAGS_DIRECT_IO);
	} catch (IOException &) {
		handle = fs.OpenFile(path, flags);
	}
}

bool BlockCacheSpill::IsConfigured() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return handle != nullptr;
}

uint32_t BlockCacheSpill::AcquireSlot() {
	if (free_slots.empty()) {
		const auto used = slots.size();
		for (idx_t i = GROW_SLOTS; i > 0; i--) {
			free_slots.push_back(NumericCast<uint32_t>(used + i - 1));
		}
	}
	const auto index = free_slots.back();
	free_slots.pop_back();
	return index;
}

void BlockCacheSpill::Release(Slots::iterator entry) {
	if (!entry->second.keep) {
		backed_order.erase(entry->second.order);
	}
	free_slots.push_back(entry->second.index);
	slots.erase(entry);
}

bool BlockCacheSpill::DropOldestBacked() {
	if (backed_order.empty()) {
		return false;
	}
	auto victim = slots.find(backed_order.front());
	D_ASSERT(victim != slots.end() && !victim->second.keep);
	Release(victim);
	return true;
}

bool BlockCacheSpill::Reserve(uint64_t key, bool keep, Reservation &result) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	result = Reservation {key, NO_SLOT};
	if (!handle || (!keep && backed_limit == 0)) {
		return false;
	}
	auto existing = slots.find(key);
	if (existing != slots.end()) {
		if (keep && !existing->second.keep) {
			backed_order.erase(existing->second.order);
			existing->second.keep = true;
		}
		return true;
	}
	while (!keep && backed_order.size() >= backed_limit && DropOldestBacked()) {
	}
	const auto index = AcquireSlot();
	if (index >= extent) {
		try {
			if (temporary_files) {
				temporary_files->IncreaseSizeOnDisk((index + 1 - extent) * BlockCache::BLOCK_SIZE);
			}
		} catch (...) {
			free_slots.push_back(index);
			return false;
		}
		extent = index + 1;
	}
	auto order = backed_order.end();
	if (!keep) {
		order = backed_order.insert(order, key);
	}
	slots.emplace(key, Slot {index, keep, false, order});
	result.slot = index;
	return true;
}

bool BlockCacheSpill::Write(const Reservation &reservation, data_ptr_t data) {
	if (reservation.slot == NO_SLOT) {
		return true;
	}
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = slots.find(reservation.key);
	if (entry == slots.end() || entry->second.index != reservation.slot) {
		return true;
	}
	try {
		handle->Write(QueryContext(), data, BlockCache::BLOCK_SIZE, entry->second.index * BlockCache::BLOCK_SIZE);
	} catch (...) {
		Release(entry);
		return false;
	}
	entry->second.written = true;
	return true;
}

void BlockCacheSpill::Cancel(const Reservation &reservation) {
	if (reservation.slot == NO_SLOT) {
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = slots.find(reservation.key);
	if (entry != slots.end() && entry->second.index == reservation.slot) {
		Release(entry);
	}
}

bool BlockCacheSpill::Read(uint64_t key, data_ptr_t data) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = slots.find(key);
	if (entry != slots.end() && !entry->second.written) {
		auto settled = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
			auto reserved = slots.find(key);
			return reserved == slots.end() || reserved->second.written;
		};
		lock.Await(absl::Condition(&settled));
		entry = slots.find(key);
	}
	if (entry == slots.end()) {
		return false;
	}
	handle->Read(data, BlockCache::BLOCK_SIZE, entry->second.index * BlockCache::BLOCK_SIZE);
	return true;
}

bool BlockCacheSpill::Contains(uint64_t key) const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return slots.find(key) != slots.end();
}

vector<uint64_t> BlockCacheSpill::Keys() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	vector<uint64_t> result;
	result.reserve(slots.size());
	for (auto &slot : slots) {
		result.push_back(slot.first);
	}
	return result;
}

void BlockCacheSpill::Erase(uint64_t key) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = slots.find(key);
	if (entry != slots.end()) {
		Release(entry);
	}
}

void BlockCacheSpill::EraseFile(uint32_t file) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	for (auto entry = slots.begin(); entry != slots.end();) {
		if (BlockCache::FileOf(entry->first) == file) {
			Release(entry++);
		} else {
			++entry;
		}
	}
}

void BlockCacheSpill::EraseBacked() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	for (auto entry = slots.begin(); entry != slots.end();) {
		if (entry->second.keep) {
			++entry;
		} else {
			Release(entry++);
		}
	}
}

} // namespace duckdb
