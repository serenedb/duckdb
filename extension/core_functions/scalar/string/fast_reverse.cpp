////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "core_functions/scalar/fast_reverse.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "utf8proc_wrapper.hpp"

#include <cstring>

namespace duckdb {

static void ReverseBytes(const char *input, idx_t begin, idx_t end, idx_t size, char *output) {
	idx_t i = begin;
	for (; i + 8 <= end; i += 8) {
		uint64_t word;
		memcpy(&word, input + i, 8);
		word = __builtin_bswap64(word);
		memcpy(output + size - i - 8, &word, 8);
	}
	for (; i < end; i++) {
		output[size - i - 1] = input[i];
	}
}

static bool IsCrLf(const char *input, idx_t pos) {
	return input[pos] == '\r' && input[pos + 1] == '\n';
}

static bool IsSimpleTwoByte(const char *input, idx_t pos, idx_t size) {
	auto lead = static_cast<unsigned char>(input[pos]);
	if ((lead & 0xE0) != 0xC0 || pos + 1 >= size) {
		return false;
	}
	auto codepoint = (static_cast<uint32_t>(lead & 0x1F) << 6) | (static_cast<unsigned char>(input[pos + 1]) & 0x3F);
	return codepoint < 0x300 || (codepoint >= 0x370 && codepoint < 0x483) || (codepoint >= 0x48A && codepoint < 0x530);
}

static idx_t CharacterSize(const char *input, idx_t pos, idx_t size) {
	auto lead = static_cast<unsigned char>(input[pos]);
	idx_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	return MinValue(length, size - pos);
}

static void ReverseSimple(const char *input, idx_t begin, idx_t end, idx_t size, char *output) {
	if (begin == end) {
		return;
	}
	if (FirstNonAscii(input + begin, end - begin) == end - begin && !memchr(input + begin, '\r', end - begin)) {
		ReverseBytes(input, begin, end, size, output);
		return;
	}
	for (idx_t i = begin; i < end;) {
		auto c = static_cast<unsigned char>(input[i]);
		if (c >= 0x80) {
			output[size - i - 2] = input[i];
			output[size - i - 1] = input[i + 1];
			i += 2;
		} else if (i + 1 < end && IsCrLf(input, i)) {
			output[size - i - 2] = '\r';
			output[size - i - 1] = '\n';
			i += 2;
		} else {
			output[size - i - 1] = input[i];
			i++;
		}
	}
}

static void ReverseClusters(const char *input, idx_t begin, idx_t end, idx_t size, char *output) {
	for (auto cluster : Utf8Proc::GraphemeClusters(input + begin, end - begin)) {
		memcpy(output + size - begin - cluster.end, input + begin + cluster.start, cluster.end - cluster.start);
	}
}

static idx_t NextComplex(const char *input, idx_t pos, idx_t size) {
	while (pos < size) {
		pos += FirstNonAscii(input + pos, size - pos);
		if (pos < size && IsSimpleTwoByte(input, pos, size)) {
			pos += 2;
			continue;
		}
		return pos;
	}
	return size;
}

void FastReverse::Reverse(const char *input, idx_t size, char *output) {
	auto first = FirstNonAscii(input, size);
	if (first == size) {
		ReverseBytes(input, 0, size, size, output);
		return;
	}
	idx_t done = 0;
	idx_t window_begin = 0;
	idx_t window_end = 0;
	bool has_window = false;
	idx_t pos = NextComplex(input, first, size);
	while (pos < size) {
		auto island_end = pos;
		while (island_end < size && static_cast<unsigned char>(input[island_end]) >= 0x80 &&
		       !IsSimpleTwoByte(input, island_end, size)) {
			island_end += CharacterSize(input, island_end, size);
		}
		idx_t begin = pos;
		if (begin > 0) {
			begin -= static_cast<unsigned char>(input[begin - 1]) >= 0x80 ? 2 : 1;
		}
		if (begin > 0 && IsCrLf(input, begin - 1)) {
			begin--;
		}
		auto end = island_end < size ? island_end + CharacterSize(input, island_end, size) : size;
		if (end < size && IsCrLf(input, end - 1)) {
			end++;
		}
		if (has_window && begin <= window_end) {
			window_end = end;
		} else {
			if (has_window) {
				ReverseClusters(input, window_begin, window_end, size, output);
				done = window_end;
			}
			ReverseSimple(input, done, begin, size, output);
			window_begin = begin;
			window_end = end;
			has_window = true;
		}
		if (end >= size) {
			break;
		}
		pos = NextComplex(input, end, size);
	}
	if (has_window) {
		ReverseClusters(input, window_begin, window_end, size, output);
		done = window_end;
	}
	ReverseSimple(input, done, size, size, output);
}

} // namespace duckdb
