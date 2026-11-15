//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/entry_use.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

enum class EntryUse : uint8_t { REFERENCE, OWN, RENAME, DROP };

} // namespace duckdb
