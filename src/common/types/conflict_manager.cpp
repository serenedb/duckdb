#include "duckdb/common/types/conflict_manager.hpp"

namespace duckdb {

ConflictManager::ConflictManager(const VerifyExistenceType verify_existence_type, const idx_t chunk_size)
    : verify_existence_type(verify_existence_type), chunk_size(chunk_size), mode(ConflictManagerMode::THROW) {
}

bool ConflictManager::IsConflict(LookupResultType type) {
	switch (type) {
	case LookupResultType::LOOKUP_NULL:
		if (IgnoreNulls()) {
			return false;
		}
		// If we ignore NULL, then we treat this as a hit.
		return IsConflict(LookupResultType::LOOKUP_HIT);
	case LookupResultType::LOOKUP_HIT:
		return true;
	case LookupResultType::LOOKUP_MISS:
		return false;
	default:
		throw NotImplementedException("Type not implemented for LookupResultType");
	}
}

bool ConflictManager::AddHit(const idx_t index_in_chunk) {
	D_ASSERT(index_in_chunk < chunk_size);
	if (mode == ConflictManagerMode::THROW) {
		return true;
	}
	if (conflict_count == 0) {
		validity.Initialize(chunk_size, false);
		first_index = index_in_chunk;
	}
	if (!validity.RowIsValid(index_in_chunk)) {
		validity.SetValid(index_in_chunk);
		conflict_count++;
	}
	return false;
}

bool ConflictManager::AddNull(const idx_t index_in_chunk) {
	D_ASSERT(index_in_chunk < chunk_size);
	if (!IsConflict(LookupResultType::LOOKUP_NULL)) {
		return false;
	}
	return AddHit(index_in_chunk);
}

optional_idx ConflictManager::GetFirstInvalidIndex(const idx_t count, const bool negate) {
	D_ASSERT(HasConflicts());
	for (idx_t i = 0; i < count; i++) {
		if (negate && !validity.RowIsValid(i)) {
			return i;
		} else if (!negate && validity.RowIsValid(i)) {
			return i;
		}
	}
	return optional_idx();
}

bool ConflictManager::IgnoreNulls() const {
	switch (verify_existence_type) {
	case VerifyExistenceType::APPEND:
		return true;
	case VerifyExistenceType::APPEND_FK:
		return false;
	case VerifyExistenceType::DELETE_FK:
		return true;
	default:
		throw InternalException("Type not implemented for VerifyExistenceType");
	}
}

} // namespace duckdb
