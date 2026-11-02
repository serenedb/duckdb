//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/sequence_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/built_in_functions.hpp"

namespace duckdb {

struct NextvalBindData : public FunctionData {
	NextvalBindData(SequenceCatalogEntry &sequence, optional_idx role) : sequence(sequence), role(role) {
	}

	//! The sequence to use for the nextval computation; only if the sequence is a constant
	SequenceCatalogEntry &sequence;
	//! The role a function body that calls it runs as, when that is not the session's
	optional_idx role;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<NextvalBindData>(sequence, role);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<NextvalBindData>();
		return RefersToSameObject(sequence, other.sequence) && role == other.role;
	}
};

} // namespace duckdb
