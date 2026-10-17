//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/map_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

#include <absl/container/flat_hash_set.h>

namespace duckdb {

enum class MapInvalidReason : uint8_t { VALID, NULL_KEY, DUPLICATE_KEY, NOT_ALIGNED, INVALID_PARAMS };

struct MapVector {
	DUCKDB_API static const Vector &GetKeys(const Vector &vector);
	DUCKDB_API static const Vector &GetValues(const Vector &vector);
	DUCKDB_API static Vector &GetKeys(Vector &vector);
	DUCKDB_API static Vector &GetValues(Vector &vector);
	DUCKDB_API static void CreateKeySortKeys(const Vector &keys, Vector &result);
	DUCKDB_API static MapInvalidReason
	CheckMapValidity(const Vector &map, idx_t count,
	                 const SelectionVector &sel = *FlatVector::IncrementalSelectionVector());
	DUCKDB_API static void EvalMapInvalidReason(MapInvalidReason reason);
	DUCKDB_API static void MapConversionVerify(const Vector &vector, idx_t count);
};

class MapValidityChecker {
public:
	DUCKDB_API MapValidityChecker(const Vector &lists, const Vector &keys);

	DUCKDB_API MapInvalidReason CheckRow(idx_t row);

private:
	VectorIterator<list_entry_t> entries;
	const Vector &keys;
	VectorValidityIterator key_validity;
	unique_ptr<Vector> sort_key_data;
	unique_ptr<VectorIterator<string_t>> sort_keys;
	absl::flat_hash_set<string_t, StringHash, StringEquality> seen;
};

} // namespace duckdb
