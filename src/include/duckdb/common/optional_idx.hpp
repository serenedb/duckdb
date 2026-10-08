//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/optional_idx.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/likely.hpp"

namespace duckdb {

class optional_idx {
	static constexpr const idx_t INVALID_INDEX = idx_t(-1);

public:
	optional_idx() : index(INVALID_INDEX) {
	}
	[[gnu::always_inline]] optional_idx(idx_t index) : index(index) { // NOLINT: allow implicit conversion from idx_t
		if (DUCKDB_UNLIKELY(index == INVALID_INDEX)) {
			ThrowInvalidInitialization();
		}
	}

	static optional_idx Invalid() {
		return optional_idx();
	}

	bool IsValid() const {
		return index != INVALID_INDEX;
	}

	void SetInvalid() {
		index = INVALID_INDEX;
	}

	[[gnu::always_inline]] idx_t GetIndex() const {
		if (DUCKDB_UNLIKELY(index == INVALID_INDEX)) {
			ThrowNotSet();
		}
		return index;
	}

	inline bool operator==(const optional_idx &rhs) const {
		return index == rhs.index;
	}

	inline bool operator!=(const optional_idx &rhs) const {
		return index != rhs.index;
	}

private:
	//! Kept out-of-line so that the throwing paths do not block inlining of the accessors
	[[noreturn]] DUCKDB_API static void ThrowInvalidInitialization();
	[[noreturn]] DUCKDB_API static void ThrowNotSet();

private:
	idx_t index;
};

} // namespace duckdb
