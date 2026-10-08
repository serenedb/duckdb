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

#include "duckdb/function/scalar/ilike_ascii_fold.hpp"

#include "duckdb/function/scalar/string_common.hpp"

#include <cstring>

namespace duckdb {

bool ILikeAsciiFold::IsAscii(const string_t &str) {
	return FirstNonAscii(str.GetData(), str.GetSize()) == str.GetSize();
}

static bool ContainsSequence(const char *data, idx_t size, unsigned char lead, const char *rest, idx_t rest_size) {
	const char *end = data + size;
	const char *pos = data;
	while (pos < end) {
		auto hit = static_cast<const char *>(memchr(pos, lead, static_cast<size_t>(end - pos)));
		if (!hit) {
			return false;
		}
		if (static_cast<idx_t>(end - hit - 1) >= rest_size && memcmp(hit + 1, rest, rest_size) == 0) {
			return true;
		}
		pos = hit + 1;
	}
	return false;
}

bool ILikeAsciiFold::NeedsSpecialCheck(const string_t &pattern) {
	auto data = pattern.GetData();
	auto size = pattern.GetSize();
	return memchr(data, 'i', size) || memchr(data, 'k', size);
}

bool ILikeAsciiFold::Fold(const string_t &str, char *target, bool special_check) {
	auto data = str.GetData();
	auto size = str.GetSize();
	if (special_check &&
	    (ContainsSequence(data, size, 0xC4, "\xB0", 1) || ContainsSequence(data, size, 0xE2, "\x84\xAA", 2))) {
		return false;
	}
	constexpr uint64_t ONES = 0x0101010101010101ULL;
	constexpr uint64_t HIGH = 0x8080808080808080ULL;
	idx_t i = 0;
	for (; i + 8 <= size; i += 8) {
		uint64_t word;
		memcpy(&word, data + i, 8);
		uint64_t low7 = word & ~HIGH;
		uint64_t ge_a = low7 + (0x80 - 'A') * ONES;
		uint64_t gt_z = low7 + (0x80 - 'Z' - 1) * ONES;
		uint64_t upper = ge_a & ~gt_z & ~word & HIGH;
		word |= upper >> 2;
		memcpy(target + i, &word, 8);
	}
	for (; i < size; i++) {
		auto c = static_cast<unsigned char>(data[i]);
		target[i] = static_cast<char>(c + (static_cast<unsigned char>(c - 'A') < 26 ? 32 : 0));
	}
	return true;
}

} // namespace duckdb
