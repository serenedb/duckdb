//===----------------------------------------------------------------------===//
//                         DuckDB
//
// text_utf8.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace duckdb {
namespace text {

static constexpr uint32_t REPLACEMENT_CHARACTER = 0xFFFD;

static constexpr uint8_t UTF8_LEAD3_T1_BITS[16] = {0x20, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
                                                   0x30, 0x30, 0x30, 0x30, 0x30, 0x10, 0x30, 0x30};
static constexpr uint8_t UTF8_LEAD4_T1_BITS[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                   0x1E, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00, 0x00};

inline uint32_t DecodeUtf8(const uint8_t *data, size_t size, size_t &position) {
	uint32_t c = data[position++];
	if (c < 0x80) {
		return c;
	}
	uint8_t t;
	if (position != size) {
		if (c >= 0xE0) {
			if (c < 0xF0) {
				c &= 0xF;
				if ((UTF8_LEAD3_T1_BITS[c] & (1 << ((t = data[position]) >> 5))) != 0) {
					t &= 0x3F;
					c = (c << 6) | t;
					if (++position != size && (t = static_cast<uint8_t>(data[position] - 0x80)) <= 0x3F) {
						++position;
						return (c << 6) | t;
					}
				}
			} else {
				c -= 0xF0;
				if (c <= 4 && (UTF8_LEAD4_T1_BITS[(t = data[position]) >> 4] & (1 << c)) != 0) {
					c = (c << 6) | (t & 0x3F);
					if (++position != size && (t = static_cast<uint8_t>(data[position] - 0x80)) <= 0x3F) {
						c = (c << 6) | t;
						if (++position != size && (t = static_cast<uint8_t>(data[position] - 0x80)) <= 0x3F) {
							++position;
							return (c << 6) | t;
						}
					}
				}
			}
		} else if (c >= 0xC2) {
			c &= 0x1F;
			if ((t = static_cast<uint8_t>(data[position] - 0x80)) <= 0x3F) {
				++position;
				return (c << 6) | t;
			}
		}
	}
	return REPLACEMENT_CHARACTER;
}

inline uint32_t Utf8Length(uint32_t c) {
	return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
}

inline uint32_t Utf16Length(uint32_t c) {
	return c < 0x10000 ? 1 : 2;
}

inline uint32_t EncodeUtf8(uint32_t c, uint8_t *out) {
	if (c < 0x80) {
		out[0] = static_cast<uint8_t>(c);
		return 1;
	}
	if (c < 0x800) {
		out[0] = static_cast<uint8_t>(0xC0 | (c >> 6));
		out[1] = static_cast<uint8_t>(0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = static_cast<uint8_t>(0xE0 | (c >> 12));
		out[1] = static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F));
		out[2] = static_cast<uint8_t>(0x80 | (c & 0x3F));
		return 3;
	}
	out[0] = static_cast<uint8_t>(0xF0 | (c >> 18));
	out[1] = static_cast<uint8_t>(0x80 | ((c >> 12) & 0x3F));
	out[2] = static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F));
	out[3] = static_cast<uint8_t>(0x80 | (c & 0x3F));
	return 4;
}

inline void DecodeUtf8(std::string_view input, std::vector<uint32_t> &output) {
	output.clear();
	output.reserve(input.size());
	auto data = reinterpret_cast<const uint8_t *>(input.data());
	size_t position = 0;
	while (position < input.size()) {
		output.push_back(DecodeUtf8(data, input.size(), position));
	}
}

inline void AppendUtf8(const uint32_t *input, size_t size, std::string &output) {
	uint8_t buffer[4];
	for (size_t i = 0; i < size; i++) {
		auto length = EncodeUtf8(input[i], buffer);
		output.append(reinterpret_cast<const char *>(buffer), length);
	}
}

} // namespace text
} // namespace duckdb
