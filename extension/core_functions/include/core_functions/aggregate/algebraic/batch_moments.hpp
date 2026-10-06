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

#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/validity_mask.hpp"

namespace duckdb {

//! Count, means, squared deviations and co-moment of the rows of a chunk where both inputs are valid, computed in
//! two passes so that they can be merged into running states with the parallel combine formulas.
struct BatchMoments {
	uint64_t count = 0;
	double mean_a = 0;
	double mean_b = 0;
	double m2_a = 0;
	double m2_b = 0;
	double co_moment = 0;

	template <class A_TYPE, class B_TYPE>
	void Compute(const A_TYPE *a, const B_TYPE *b, const SelectionVector &asel, const SelectionVector &bsel,
	             const ValidityMask &avalidity, const ValidityMask &bvalidity, idx_t n) {
		const bool all_valid = avalidity.CannotHaveNull() && bvalidity.CannotHaveNull();
		auto valid = [&](idx_t aidx, idx_t bidx) {
			return all_valid || (avalidity.RowIsValid(aidx) && bvalidity.RowIsValid(bidx));
		};
		double sum_a = 0;
		double sum_b = 0;
		for (idx_t i = 0; i < n; i++) {
			auto aidx = asel.get_index(i);
			auto bidx = bsel.get_index(i);
			if (valid(aidx, bidx)) {
				sum_a += static_cast<double>(a[aidx]);
				sum_b += static_cast<double>(b[bidx]);
				count++;
			}
		}
		if (count == 0) {
			return;
		}
		mean_a = sum_a / static_cast<double>(count);
		mean_b = sum_b / static_cast<double>(count);
		for (idx_t i = 0; i < n; i++) {
			auto aidx = asel.get_index(i);
			auto bidx = bsel.get_index(i);
			if (valid(aidx, bidx)) {
				const double da = static_cast<double>(a[aidx]) - mean_a;
				const double db = static_cast<double>(b[bidx]) - mean_b;
				m2_a += da * da;
				m2_b += db * db;
				co_moment += da * db;
			}
		}
	}
};

} // namespace duckdb
