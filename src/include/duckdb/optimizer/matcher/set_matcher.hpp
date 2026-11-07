//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/matcher/set_matcher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <absl/cleanup/cleanup.h>

namespace duckdb {

class SetMatcher {
public:
	//! The policy used by the SetMatcher
	enum class Policy {
		//! All entries have to be matched, and the matches have to be ordered
		ORDERED,
		//! All entries have to be matched, but the order of the matches does not matter
		UNORDERED,
		//! Only some entries have to be matched, the order of the matches does not matter
		SOME,
		//! Only some entries have to be matched. The order of the matches does matter.
		SOME_ORDERED,
		//! Not initialized
		INVALID
	};

	template <class T, class MATCHER>
	static bool MatchRecursive(vector<unique_ptr<MATCHER>> &matchers, vector<unique_ptr<T>> &entries,
	                           vector<reference<T>> &bindings, idx_t m_idx) {
		if (m_idx == matchers.size()) {
			return true;
		}
		auto previous_binding_count = NumericCast<int64_t>(bindings.size());
		for (auto &entry : entries) {
			if (!entry) {
				continue;
			}
			if (matchers[m_idx]->Match(*entry, bindings)) {
				auto matched = std::move(entry);
				absl::Cleanup restore = [&] {
					entry = std::move(matched);
				};
				if (MatchRecursive(matchers, entries, bindings, m_idx + 1)) {
					return true;
				}
			}
			bindings.erase(bindings.begin() + previous_binding_count, bindings.end());
		}
		return false;
	}

	template <class T, class MATCHER>
	static idx_t MatchFirst(MATCHER &matcher, const vector<unique_ptr<T>> &entries, vector<reference<T>> &bindings,
	                        idx_t from, idx_t skip = DConstants::INVALID_INDEX) {
		auto previous_binding_count = NumericCast<int64_t>(bindings.size());
		for (auto e_idx = from; e_idx < entries.size(); e_idx++) {
			if (e_idx == skip) {
				continue;
			}
			if (matcher.Match(*entries[e_idx], bindings)) {
				return e_idx;
			}
			bindings.erase(bindings.begin() + previous_binding_count, bindings.end());
		}
		return DConstants::INVALID_INDEX;
	}

	template <class T, class MATCHER>
	static bool MatchTwo(vector<unique_ptr<MATCHER>> &matchers, const vector<unique_ptr<T>> &entries,
	                     vector<reference<T>> &bindings) {
		auto previous_binding_count = NumericCast<int64_t>(bindings.size());
		auto first = MatchFirst(*matchers[0], entries, bindings, 0);
		if (first == DConstants::INVALID_INDEX) {
			return false;
		}
		if (MatchFirst(*matchers[1], entries, bindings, 0, first) != DConstants::INVALID_INDEX) {
			return true;
		}
		bindings.erase(bindings.begin() + previous_binding_count, bindings.end());
		if (MatchFirst(*matchers[0], entries, bindings, first + 1) != DConstants::INVALID_INDEX &&
		    matchers[1]->Match(*entries[first], bindings)) {
			return true;
		}
		bindings.erase(bindings.begin() + previous_binding_count, bindings.end());
		return false;
	}

	template <class T, class MATCHER>
	static bool Match(vector<unique_ptr<MATCHER>> &matchers, vector<unique_ptr<T>> &entries,
	                  vector<reference<T>> &bindings, Policy policy) {
		if (policy == Policy::ORDERED) {
			// ordered policy, count has to match
			if (matchers.size() != entries.size()) {
				return false;
			}
			// now entries have to match in order
			for (idx_t i = 0; i < matchers.size(); i++) {
				if (!matchers[i]->Match(*entries[i], bindings)) {
					return false;
				}
			}
			return true;
		} else if (policy == Policy::SOME_ORDERED) {
			if (entries.size() < matchers.size()) {
				return false;
			}
			// now provided entries have to match in order
			for (idx_t i = 0; i < matchers.size(); i++) {
				if (!matchers[i]->Match(*entries[i], bindings)) {
					return false;
				}
			}
			return true;
		} else {
			if (policy == Policy::UNORDERED && matchers.size() != entries.size()) {
				// unordered policy, count does not match: no match
				return false;
			} else if (policy == Policy::SOME && matchers.size() > entries.size()) {
				// some policy, every matcher has to match a unique entry
				// this is not possible if there are more matchers than entries
				return false;
			}
			// now perform the actual matching
			// every matcher has to match a UNIQUE entry
			switch (matchers.size()) {
			case 0:
				return true;
			case 1:
				return MatchFirst(*matchers[0], entries, bindings, 0) != DConstants::INVALID_INDEX;
			case 2:
				return MatchTwo(matchers, entries, bindings);
			default:
				D_ASSERT(false);
				return MatchRecursive(matchers, entries, bindings, 0);
			}
		}
	}
};

} // namespace duckdb
