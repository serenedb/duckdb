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

#include "duckdb/common/helper.hpp"
#include "duckdb/function/scalar/string_common.hpp"

#include <cstring>

namespace duckdb {

idx_t Utf8CharacterCount(const char *input, idx_t n) {
	constexpr uint64_t HIGH_BITS = 0x8080808080808080ULL;
	idx_t continuation = 0;
	idx_t i = 0;
	for (; i + sizeof(uint64_t) <= n; i += sizeof(uint64_t)) {
		uint64_t word;
		memcpy(&word, input + i, sizeof(word));
		continuation += static_cast<idx_t>(__builtin_popcountll(word & ~(word << 1) & HIGH_BITS));
	}
	for (; i < n; i++) {
		continuation += (static_cast<uint8_t>(input[i]) & 0xC0) == 0x80;
	}
	return n - continuation;
}

} // namespace duckdb
