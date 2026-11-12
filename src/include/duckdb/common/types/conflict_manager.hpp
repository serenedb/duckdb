//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/conflict_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/execution/index/art/art.hpp"

namespace duckdb {

enum class ConflictManagerMode : uint8_t {
	SCAN, // Gather conflicts without throwing a constraint exception.
	THROW // Throw on the first conflict.
};

enum class LookupResultType : uint8_t { LOOKUP_MISS, LOOKUP_HIT, LOOKUP_NULL };

//! The conflict manager tracks conflicts during constraint verification of a chunk: it either throws on the first
//! conflict, or records which rows of the chunk have one.
class ConflictManager {
public:
	ConflictManager(const VerifyExistenceType lookup_type, const idx_t chunk_size);

public:
	//! Returns true, if we need to throw, otherwise, records the conflict and returns false.
	bool AddHit(const idx_t index_in_chunk);
	//! Returns true, if we need to throw, otherwise, records the NULL and returns false.
	bool AddNull(const idx_t index_in_chunk);
	//! Returns the index of the first (in)valid row, if any.
	optional_idx GetFirstInvalidIndex(const idx_t count, const bool negate = false);

	//! Sets the mode of the conflict manager.
	void SetMode(const ConflictManagerMode mode_p) {
		mode = mode_p;
	}

	//! Returns the existence verification type.
	VerifyExistenceType GetVerifyExistenceType() const {
		return verify_existence_type;
	}
	//! Returns true, if there are any conflicts, else false.
	bool HasConflicts() const {
		return conflict_count != 0;
	}
	//! Returns the number of conflicts.
	idx_t ConflictCount() const {
		return conflict_count;
	}
	//! Returns the first index in a chunk with a conflict.
	idx_t GetFirstIndex() const {
		D_ASSERT(HasConflicts());
		return first_index;
	}
	//! Returns the validity array marking the rows with a conflict.
	ValidityArray &GetValidity() {
		D_ASSERT(HasConflicts());
		return validity;
	}

private:
	//! The type of constraint verification for which we're using the conflict manager.
	VerifyExistenceType verify_existence_type;
	//! The count of the data chunk for which we've created the conflict manager.
	idx_t chunk_size;
	//! The mode of the conflict manager.
	ConflictManagerMode mode;
	//! The number of rows with a conflict.
	idx_t conflict_count = 0;
	//! The index in the chunk of the first conflict.
	idx_t first_index = 0;
	//! True (valid) marks a row with a conflict.
	ValidityArray validity;

private:
	//! Returns true, if we register a conflict for the lookup type.
	bool IsConflict(LookupResultType type);
	//! Returns true, if we ignore NULLs, else false.
	bool IgnoreNulls() const;
};

} // namespace duckdb
