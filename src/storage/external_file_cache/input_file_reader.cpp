#include "duckdb/storage/external_file_cache/input_file_reader.hpp"

#include "duckdb/common/limits.hpp"

namespace duckdb {

InputFileReader::Block &InputFileReader::GetBlock() {
	if (!block) {
		block = make_uniq<Block>();
		// a reader that names no end may read up to the end of the file
		ahead.Arm(Position(), NumericLimits<idx_t>::Maximum(), 0);
	}
	return *block;
}

void InputFileReader::NameEnd(idx_t until, ReadAheadSteps steps) {
	auto &pinned = GetBlock();
	pinned.next = NumericLimits<idx_t>::Maximum();
	pinned.scans = steps == ReadAheadSteps::SCANNING;
	// the steps of a user start with what they ask of a mapped file, a reader with the blocks it asks for
	ahead.Arm(Position(), until, steps == ReadAheadSteps::NONE ? 0 : InputFile::HINT_CHUNK);
}

void InputFileReader::ReadAhead(Block &pinned, idx_t number, bool exact) {
	const auto &blocks = file->Blocks();
	const auto block_size = blocks.BlockSize();
	const auto behind = blocks.BlockStart(number + 1);
	const bool named = ahead.Stop() != NumericLimits<idx_t>::Maximum();
	// what is asked for with the block
	FileRange range;
	if (pinned.next == NumericLimits<idx_t>::Maximum()) {
		// a reader that named its end reads past its first block: a second request for the next one costs more
		// than its bytes in the first
		if (named && behind < ahead.Stop()) {
			range = {behind, block_size};
		}
	} else if (number != pinned.next) {
		// a step back into the block before the one the reader left goes on in order from there: the start of a
		// group a reader rereads may lie there
		if (number + 2 != pinned.next) {
			ahead.Restart(behind, 2 * block_size);
		}
	} else if (read_end != begin ? named || !exact : pinned.scans && !exact) {
		range = ahead.Extend(behind, ExternalFileCache::MAX_READ_AHEAD, 2 * block_size);
	}
	pinned.next = number + 1;
	if (range.size != 0) {
		// one request holds the block and the ones behind it, up to two blocks of the range whatever the bytes in
		// flight; what those leave out is asked for again
		const auto from = blocks.BlockStart(number);
		const auto end = range.offset + range.size;
		const auto needed = MinValue(end, range.offset + 2 * block_size) - from;
		if (const auto asked = file->ReadAhead(from, end - from, needed); asked < end) {
			ahead.Rewind(asked);
		}
	}
}

bool InputFileReader::Refill(bool exact) {
	if (!InBlocks()) {
		return false;
	}
	const auto position = Position();
	if (position >= file->GetSize()) {
		return false;
	}
	auto &pinned = GetBlock();
	const auto &blocks = file->Blocks();
	const auto number = blocks.BlockOf(position);
	if (!file->IsUnbacked()) {
		ReadAhead(pinned, number, exact);
	}
	pinned.pin = file->GetCache().Pin(*file, number);
	start = blocks.BlockStart(number);
	begin = pinned.pin.Ptr();
	SetReadWindow(begin + (position - start), pinned.pin.Size() - (position - start));
	D_ASSERT(read_pos != read_end);
	return true;
}

void InputFileReader::Underflow(QueryContext context, data_ptr_t buffer, idx_t read_size) {
	const bool exact = read_size > sizeof(uint64_t);
	const auto end = Position() + read_size;
	// the bytes of the read that are asked for end here
	auto needed = Position() + ReadWindowRoom();
	while (true) {
		const auto chunk = MinValue(read_size, ReadWindowRoom());
		if (chunk != 0) {
			memcpy(buffer, read_pos, chunk);
			read_pos += chunk;
			buffer += chunk;
			read_size -= chunk;
		}
		if (read_size == 0) {
			return;
		}
		if (InBlocks() && needed < end && Position() + ExternalFileCache::MAX_FILE_IN_FLIGHT / 2 >= needed) {
			// a long read fetches its blocks together, a window of them at a time
			const auto &blocks = file->Blocks();
			const auto until = MinValue(end, Position() + ExternalFileCache::MAX_FILE_IN_FLIGHT);
			if (blocks.BlockOf(needed) != blocks.BlockOf(until - 1)) {
				file->ReadAhead(needed, until - needed, until - needed);
			}
			needed = until;
		}
		if (!Refill(exact)) {
			InputFile::ThrowPastEnd(file.get(), Position(), static_cast<idx_t>(read_end - begin));
		}
	}
}

const_data_ptr_t InputFileReader::PeekSlow(idx_t size) {
	if (read_pos == read_end && Refill(false) && size <= ReadWindowRoom()) {
		return read_pos;
	}
	return nullptr;
}

void InputFileReader::SeekOutside(idx_t position) {
	if (!InBlocks()) {
		// a read at a position past the bytes fails
		read_pos = read_end;
		return;
	}
	if (block) {
		block->pin.Reset();
	}
	DropWindow();
	start = position;
}

FilePin InputFileReader::PinSlow(idx_t offset, idx_t size, bool shared) const {
	if (!file) {
		InputFile::ThrowPastEnd(nullptr, offset, static_cast<idx_t>(read_end - begin));
	}
	if (block && offset >= start) {
		// the bytes may lie in the block the reader has pinned
		const auto at = offset - start;
		const auto window = static_cast<idx_t>(read_end - begin);
		if (at <= window && size != 0 && size <= window - at) {
			auto pinned = block->pin.Share(at, size);
			if (pinned.IsValid()) {
				return pinned;
			}
		}
	}
	return shared ? file->PinShared(offset, size) : file->Pin(offset, size);
}

void InputFileReader::ReadOnce(data_ptr_t buffer, idx_t size) {
	if (!file) {
		ReadData(buffer, size);
		return;
	}
	file->ReadOnce(Position(), buffer, size);
	Skip(size);
}

} // namespace duckdb
