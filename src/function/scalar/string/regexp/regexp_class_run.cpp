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

#include "duckdb/function/scalar/regexp_class_run.hpp"

#include "re2/regexp.h"

namespace duckdb {

static void AddAscii(bool member[], int lo, int hi) {
	for (int c = lo; c <= hi; c++) {
		member[c] = true;
	}
}

bool RegexpClassRun::Init(const duckdb_re2::RE2 &re) {
	auto regexp = re.Regexp();
	if (!regexp || regexp->op() != duckdb_re2::kRegexpPlus || (regexp->parse_flags() & duckdb_re2::Regexp::NonGreedy)) {
		return false;
	}
	auto sub = regexp->sub()[0];
	if (sub->op() == duckdb_re2::kRegexpLiteral) {
		auto rune = sub->rune();
		if (rune >= 0x80) {
			return false;
		}
		member[rune] = true;
		if (sub->parse_flags() & duckdb_re2::Regexp::FoldCase) {
			if (rune >= 'a' && rune <= 'z') {
				member[rune - 'a' + 'A'] = true;
			} else if (rune >= 'A' && rune <= 'Z') {
				member[rune - 'A' + 'a'] = true;
			} else if (rune == 'k' || rune == 'K' || rune == 's' || rune == 'S') {
				return false;
			}
		}
		return true;
	}
	if (sub->op() != duckdb_re2::kRegexpCharClass) {
		return false;
	}
	auto cc = sub->cc();
	for (auto range = cc->begin(); range != cc->end(); ++range) {
		if (range->hi >= 0x80) {
			return false;
		}
		AddAscii(member, range->lo, range->hi);
	}
	return !cc->empty();
}

bool RegexpClassRun::Next(const char *data, idx_t size, idx_t &pos, idx_t &begin, idx_t &end) const {
	auto bytes = reinterpret_cast<const unsigned char *>(data);
	while (pos < size && !member[bytes[pos]]) {
		pos++;
	}
	if (pos == size) {
		return false;
	}
	begin = pos;
	while (pos < size && member[bytes[pos]]) {
		pos++;
	}
	end = pos;
	return true;
}

} // namespace duckdb
