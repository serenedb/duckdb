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

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace duckdb {

template <bool UPPER>
inline uint64_t HexDigits8(uint32_t value) {
	uint64_t v = value;
	v = (v | (v << 16)) & 0x0000FFFF0000FFFFULL;
	v = (v | (v << 8)) & 0x00FF00FF00FF00FFULL;
	v = (v | (v << 4)) & 0x0F0F0F0F0F0F0F0FULL;
	const uint64_t letters = ((v + 0x0606060606060606ULL) >> 4) & 0x0101010101010101ULL;
	return std::byteswap(v + 0x3030303030303030ULL + letters * (UPPER ? 7 : 39));
}

template <bool UPPER>
inline void WriteHexDigits16(uint64_t value, char *output) {
	const uint64_t high = HexDigits8<UPPER>(static_cast<uint32_t>(value >> 32));
	const uint64_t low = HexDigits8<UPPER>(static_cast<uint32_t>(value));
	memcpy(output, &high, sizeof(high));
	memcpy(output + sizeof(high), &low, sizeof(low));
}

} // namespace duckdb
