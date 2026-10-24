#include "duckdb/storage/block_cache/block_cache_spill.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/storage/block_cache/block_cache.hpp"

#include <absl/cleanup/cleanup.h>

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
	closing = true;
	auto drained = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
		return in_flight == 0;
	};
	lock.Await(absl::Condition(&drained));
	closing = false;
	auto path = handle->GetPath();
	auto &fs = handle->file_system;
	handle.reset();
	if (temporary_files) {
		temporary_files->DecreaseSizeOnDisk(extent * BlockCache::BLOCK_SIZE);
		temporary_files = nullptr;
	}
	extent = 0;
	copies.clear();
	free_slots.clear();
	slot_uses.clear();
	backed_order.clear();
	fs.TryRemoveFile(path);
}

void BlockCacheSpill::AwaitClose() {
	auto closed = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
		return !closing;
	};
	lock.Await(absl::Condition(&closed));
}

void BlockCacheSpill::Configure(FileSystem &fs, const string &path, idx_t backed_limit_p) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	AwaitClose();
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
	for (auto &copy : copies) {
		if (copy.second.keep) {
			throw InvalidInputException(
			    "Cannot move the block cache spill file while it holds blocks of in-memory files");
		}
	}
	Close();
	is_default = false;
	if (path.empty()) {
		return;
	}
	Open(fs, path);
}

void BlockCacheSpill::ConfigureDefault(FileSystem &fs, TemporaryFileManager &temporary_files_p) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	AwaitClose();
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
		const auto count = slot_uses.size();
		slot_uses.resize(count + GROW_SLOTS);
		for (idx_t i = GROW_SLOTS; i > 0; i--) {
			free_slots.push_back(NumericCast<uint32_t>(count + i - 1));
		}
	}
	const auto slot = free_slots.back();
	free_slots.pop_back();
	return slot;
}

BlockCacheSpill::Copies::iterator BlockCacheSpill::Release(Copies::iterator copy) {
	if (!copy->second.keep) {
		backed_order.erase(copy->second.order);
	}
	auto &use = slot_uses[copy->second.slot];
	use.held = false;
	if (use.pins == 0) {
		free_slots.push_back(copy->second.slot);
	}
	return copies.erase(copy);
}

bool BlockCacheSpill::DropOldestBacked() {
	if (backed_order.empty()) {
		return false;
	}
	auto victim = copies.find(backed_order.front());
	D_ASSERT(victim != copies.end() && !victim->second.keep);
	Release(victim);
	return true;
}

void BlockCacheSpill::Pin(uint32_t slot) {
	slot_uses[slot].pins++;
	in_flight++;
}

void BlockCacheSpill::Unpin(uint32_t slot) {
	auto &use = slot_uses[slot];
	D_ASSERT(use.pins > 0 && in_flight > 0);
	in_flight--;
	if (--use.pins == 0 && !use.held) {
		free_slots.push_back(slot);
	}
}

bool BlockCacheSpill::Reserve(uint64_t key, bool keep, Reservation &result) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	result = Reservation {key, NO_SLOT, nullptr};
	if (!handle || closing || (!keep && backed_limit == 0)) {
		return false;
	}
	auto existing = copies.find(key);
	if (existing != copies.end()) {
		if (keep && !existing->second.keep) {
			backed_order.erase(existing->second.order);
			existing->second.keep = true;
		}
		return true;
	}
	while (!keep && backed_order.size() >= backed_limit && DropOldestBacked()) {
	}
	const auto slot = AcquireSlot();
	if (slot >= extent) {
		try {
			if (temporary_files) {
				temporary_files->IncreaseSizeOnDisk((slot + 1 - extent) * BlockCache::BLOCK_SIZE);
			}
		} catch (...) {
			free_slots.push_back(slot);
			return false;
		}
		extent = slot + 1;
	}
	auto order = backed_order.end();
	if (!keep) {
		order = backed_order.insert(order, key);
	}
	copies.emplace(key, Copy {slot, keep, false, order});
	slot_uses[slot].held = true;
	Pin(slot);
	result.slot = slot;
	result.file = handle.get();
	return true;
}

bool BlockCacheSpill::Write(const Reservation &reservation, data_ptr_t data) {
	if (reservation.slot == NO_SLOT) {
		return true;
	}
	auto file = reservation.file;
	bool written = true;
	try {
		file->Write(QueryContext(), data, BlockCache::BLOCK_SIZE, reservation.slot * BlockCache::BLOCK_SIZE);
	} catch (...) {
		written = false;
	}
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto copy = copies.find(reservation.key);
	if (copy != copies.end() && copy->second.slot == reservation.slot) {
		if (written) {
			copy->second.written = true;
		} else {
			Release(copy);
		}
	}
	Unpin(reservation.slot);
	return written;
}

void BlockCacheSpill::Cancel(const Reservation &reservation) {
	if (reservation.slot == NO_SLOT) {
		return;
	}
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto copy = copies.find(reservation.key);
	if (copy != copies.end() && copy->second.slot == reservation.slot) {
		Release(copy);
	}
	Unpin(reservation.slot);
}

bool BlockCacheSpill::Read(uint64_t key, data_ptr_t data) {
	optional_ptr<FileHandle> file;
	uint32_t slot = NO_SLOT;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		auto copy = copies.find(key);
		if (copy != copies.end() && !copy->second.written) {
			auto settled = [&]() DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
				auto reserved = copies.find(key);
				return closing || reserved == copies.end() || reserved->second.written;
			};
			lock.Await(absl::Condition(&settled));
			copy = copies.find(key);
		}
		if (closing || copy == copies.end()) {
			return false;
		}
		slot = copy->second.slot;
		Pin(slot);
		file = handle.get();
	}
	absl::Cleanup unpin = [&]() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		Unpin(slot);
	};
	file->Read(data, BlockCache::BLOCK_SIZE, slot * BlockCache::BLOCK_SIZE);
	return true;
}

bool BlockCacheSpill::Contains(uint64_t key) const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return copies.contains(key);
}

vector<uint64_t> BlockCacheSpill::Keys() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	vector<uint64_t> result;
	result.reserve(copies.size());
	for (auto &copy : copies) {
		result.push_back(copy.first);
	}
	return result;
}

void BlockCacheSpill::Erase(uint64_t key) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto copy = copies.find(key);
	if (copy != copies.end()) {
		Release(copy);
	}
}

void BlockCacheSpill::EraseFile(uint32_t file) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto copy = copies.lower_bound(BlockCache::MakeKey(file, 0));
	while (copy != copies.end() && BlockCache::FileOf(copy->first) == file) {
		copy = Release(copy);
	}
}

void BlockCacheSpill::EraseBacked() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	while (DropOldestBacked()) {
	}
}

} // namespace duckdb
