#include "duckdb/storage/block_cache/block_cache_spill.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"

namespace duckdb {

namespace {

constexpr idx_t GROW_SLOTS = 64;

} // namespace

BlockCacheSpill::BlockCacheSpill(idx_t slot_size_p) : slot_size(slot_size_p) {
}

BlockCacheSpill::~BlockCacheSpill() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (!handle) {
		return;
	}
	auto path = handle->GetPath();
	auto &fs = handle->file_system;
	handle.reset();
	fs.TryRemoveFile(path);
}

void BlockCacheSpill::Configure(FileSystem &fs, const string &path, idx_t backed_limit_p) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	backed_limit = backed_limit_p / slot_size;
	if (handle && handle->GetPath() == path) {
		while (backed_count > backed_limit && DropOldestBacked()) {
		}
		return;
	}
	for (auto &slot : slots) {
		if (slot.second.keep) {
			throw InvalidInputException(
			    "Cannot move the block cache spill file while it holds blocks of in-memory files");
		}
	}
	if (handle) {
		auto old_path = handle->GetPath();
		handle.reset();
		fs.TryRemoveFile(old_path);
	}
	slots.clear();
	free_slots.clear();
	backed_order.clear();
	backed_count = 0;
	if (path.empty()) {
		return;
	}
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
		backed_count--;
	}
	free_slots.push_back(entry->second.index);
	slots.erase(entry);
}

bool BlockCacheSpill::DropOldestBacked() {
	while (!backed_order.empty()) {
		auto victim = slots.find(backed_order.front());
		backed_order.pop_front();
		if (victim != slots.end() && !victim->second.keep) {
			Release(victim);
			return true;
		}
	}
	return false;
}

bool BlockCacheSpill::Write(uint64_t key, data_ptr_t data, bool keep) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (!handle || (!keep && backed_limit == 0)) {
		return false;
	}
	auto existing = slots.find(key);
	if (existing != slots.end()) {
		if (keep && !existing->second.keep) {
			existing->second.keep = true;
			backed_count--;
		}
		return true;
	}
	if (!keep && backed_count >= backed_limit) {
		DropOldestBacked();
		if (backed_count >= backed_limit) {
			return false;
		}
	}
	const auto index = AcquireSlot();
	try {
		handle->Write(QueryContext(), data, slot_size, index * slot_size);
	} catch (...) {
		free_slots.push_back(index);
		return false;
	}
	slots[key] = Slot {index, keep};
	if (!keep) {
		backed_count++;
		backed_order.push_back(key);
	}
	return true;
}

bool BlockCacheSpill::Read(uint64_t key, data_ptr_t data) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto entry = slots.find(key);
	if (entry == slots.end()) {
		return false;
	}
	handle->Read(data, slot_size, entry->second.index * slot_size);
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
	backed_order.clear();
}

} // namespace duckdb
