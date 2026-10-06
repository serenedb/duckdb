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

#include "core_functions/scalar/bit_parallel_levenshtein.hpp"

#include "duckdb/common/assert.hpp"

namespace duckdb {

idx_t BitParallelLevenshtein::Distance(const char *pattern, idx_t pattern_size, const char *text, idx_t text_size) {
	D_ASSERT(pattern_size > 0 && pattern_size <= MAX_PATTERN_SIZE);
	uint64_t peq[256] = {};
	for (idx_t i = 0; i < pattern_size; i++) {
		peq[static_cast<unsigned char>(pattern[i])] |= uint64_t(1) << i;
	}
	const uint64_t last = uint64_t(1) << (pattern_size - 1);
	uint64_t pv = ~uint64_t(0);
	uint64_t mv = 0;
	idx_t score = pattern_size;
	for (idx_t j = 0; j < text_size; j++) {
		const uint64_t eq = peq[static_cast<unsigned char>(text[j])];
		const uint64_t xv = eq | mv;
		const uint64_t xh = (((eq & pv) + pv) ^ pv) | eq;
		uint64_t ph = mv | ~(xh | pv);
		uint64_t mh = pv & xh;
		if (ph & last) {
			score++;
		} else if (mh & last) {
			score--;
		}
		ph = (ph << 1) | 1;
		mh <<= 1;
		pv = mh | ~(xv | ph);
		mv = ph & xv;
	}
	return score;
}

} // namespace duckdb
