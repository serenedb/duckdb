//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_unit.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace duckdb {
namespace text {

struct TextUnit {
	const uint8_t *data;
	uint32_t compressed_size;
	uint32_t size;
};

struct LoadedUnit {
	const uint8_t *data;
	uint32_t size;
};

LoadedUnit LoadUnit(const TextUnit &unit);

class TextUnitReader {
public:
	TextUnitReader(LoadedUnit unit, uint32_t array_count);

	template <class T>
	const T *Read(uint32_t &count) {
		count = counts[array++];
		auto result = reinterpret_cast<const T *>(position);
		position += static_cast<uint64_t>(count) * sizeof(T);
		Check();
		return result;
	}
	template <class T>
	const T *Read() {
		uint32_t count;
		return Read<T>(count);
	}

private:
	void Check() const;

	const uint32_t *counts;
	const uint8_t *position;
	const uint8_t *end;
	uint32_t array;
};

struct CodePointTable {
	const uint16_t *stage1;
	const uint16_t *stage2;

	uint16_t Get(uint32_t c) const {
		return stage2[stage1[c >> 6] + (c & 0x3F)];
	}
};

} // namespace text
} // namespace duckdb
