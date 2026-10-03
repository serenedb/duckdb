#include "text_unit.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "zstd.h"

namespace duckdb {
namespace text {

namespace {

class UnitCache {
public:
	static UnitCache &Get() {
		static UnitCache cache;
		return cache;
	}

	LoadedUnit Load(const TextUnit &unit) {
		lock_guard<mutex> guard(lock);
		auto &loaded = units[&unit];
		if (!loaded) {
			auto data = unique_ptr<uint8_t[]>(new uint8_t[unit.size]);
			auto size = duckdb_zstd::ZSTD_decompress(data.get(), unit.size, unit.data, unit.compressed_size);
			if (duckdb_zstd::ZSTD_isError(size) || size != unit.size) {
				throw InternalException("Failed to decompress the text data");
			}
			loaded = std::move(data);
		}
		return {loaded.get(), unit.size};
	}

private:
	mutex lock;
	unordered_map<const TextUnit *, unique_ptr<uint8_t[]>> units;
};

} // namespace

LoadedUnit LoadUnit(const TextUnit &unit) {
	return UnitCache::Get().Load(unit);
}

TextUnitReader::TextUnitReader(LoadedUnit unit, uint32_t array_count)
    : counts(reinterpret_cast<const uint32_t *>(unit.data)), position(unit.data), end(unit.data + unit.size), array(0) {
	auto header = array_count * sizeof(uint32_t);
	header += (8 - header % 8) % 8;
	position += header;
	Check();
}

void TextUnitReader::Check() const {
	if (position > end) {
		throw InternalException("Text data is corrupt");
	}
}

} // namespace text
} // namespace duckdb
