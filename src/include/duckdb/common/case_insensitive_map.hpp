//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/case_insensitive_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/map.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

namespace duckdb {

struct CaseInsensitiveStringHashFunction {
	using is_transparent = void;

	uint64_t operator()(std::string_view str) const {
		return StringUtil::CIHash(str);
	}
};

struct CaseInsensitiveStringEquality {
	using is_transparent = void;

	bool operator()(std::string_view a, std::string_view b) const {
		return StringUtil::CIEquals(a, b);
	}
};

template <typename T>
using case_insensitive_map_t =
    unordered_map<string, T, CaseInsensitiveStringHashFunction, CaseInsensitiveStringEquality>;

using case_insensitive_set_t = unordered_set<string, CaseInsensitiveStringHashFunction, CaseInsensitiveStringEquality>;

struct CaseInsensitiveStringCompare {
	using is_transparent = void;

	bool operator()(std::string_view s1, std::string_view s2) const {
		return StringUtil::CILessThan(s1, s2);
	}
};

template <typename T>
using case_insensitive_tree_t = map<string, T, CaseInsensitiveStringCompare>;

// For lookups over a set of STATIC string literals (compile-time arrays): a
// contiguous flat table of non-owning string_views -- no per-element allocation
// and no node indirection, so faster than the node_hash<string> variants above.
// The views must point at storage that outlives the container.
using case_insensitive_set_view_t =
    absl::flat_hash_set<std::string_view, CaseInsensitiveStringHashFunction, CaseInsensitiveStringEquality>;

template <typename T>
using case_insensitive_map_view_t =
    absl::flat_hash_map<std::string_view, T, CaseInsensitiveStringHashFunction, CaseInsensitiveStringEquality>;

} // namespace duckdb
