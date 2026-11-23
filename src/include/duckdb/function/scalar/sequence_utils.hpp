//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/sequence_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/built_in_functions.hpp"

namespace duckdb {

struct NextvalBindData : public FunctionData {
	NextvalBindData(SequenceCatalogEntry &sequence, optional_idx caller_role)
	    : sequence(sequence), caller_role(caller_role) {
	}

	//! The sequence to use for the nextval computation; only if the sequence is a constant
	SequenceCatalogEntry &sequence;
	optional_idx caller_role;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<NextvalBindData>(sequence, caller_role);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<NextvalBindData>();
		return RefersToSameObject(sequence, other.sequence) && caller_role == other.caller_role;
	}
};

} // namespace duckdb
