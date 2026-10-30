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

#include "duckdb/storage/compression/dict_fsst/decompression.hpp"

namespace duckdb {

struct TableFilterState;

namespace dict_fsst {

//! Filter over a dictionary segment whose dictionary is deferred, decoding and testing only the entries the
//! still-selected rows reference, with verdicts cached per entry. Used while few rows survive the preceding
//! filters; past half the dictionary in on-demand decodes the segment falls back to the dictionary filter.
struct DictFSSTSparse {
	static bool TryFilter(CompressedStringScanState &scan_state, idx_t start, idx_t vector_count, Vector &result,
	                      SelectionVector &sel, idx_t &sel_count, TableFilterState &filter_state);
};

} // namespace dict_fsst

} // namespace duckdb
