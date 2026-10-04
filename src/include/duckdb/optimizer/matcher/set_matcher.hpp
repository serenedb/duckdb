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

#include <algorithm>

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
	static bool MatchRecursive(vector<unique_ptr<MATCHER>> &matchers, vector<reference<T>> &entries,
	                           vector<reference<T>> &bindings, const vector<vector<idx_t>> &candidates,
	                           vector<idx_t> &assigned) {
		auto m_idx = assigned.size();
		if (m_idx == matchers.size()) {
			return true;
		}
		auto previous_binding_count = NumericCast<int64_t>(bindings.size());
		for (auto e_idx : candidates[m_idx]) {
			if (std::find(assigned.begin(), assigned.end(), e_idx) != assigned.end()) {
				continue;
			}
			if (matchers[m_idx]->Match(entries[e_idx], bindings)) {
				assigned.push_back(e_idx);
				if (MatchRecursive(matchers, entries, bindings, candidates, assigned)) {
					return true;
				}
				assigned.pop_back();
			}
			bindings.erase(bindings.begin() + previous_binding_count, bindings.end());
		}
		return false;
	}

	template <class T, class MATCHER>
	static bool MatchUnordered(vector<unique_ptr<MATCHER>> &matchers, vector<reference<T>> &entries,
	                           vector<reference<T>> &bindings) {
		auto previous_binding_count = NumericCast<int64_t>(bindings.size());
		if (matchers.size() == 1) {
			for (auto &entry : entries) {
				if (matchers[0]->Match(entry, bindings)) {
					return true;
				}
				bindings.erase(bindings.begin() + previous_binding_count, bindings.end());
			}
			return false;
		}
		vector<vector<idx_t>> candidates(matchers.size());
		for (idx_t m_idx = 0; m_idx < matchers.size(); m_idx++) {
			for (idx_t e_idx = 0; e_idx < entries.size(); e_idx++) {
				if (matchers[m_idx]->Match(entries[e_idx], bindings)) {
					candidates[m_idx].push_back(e_idx);
				}
				bindings.erase(bindings.begin() + previous_binding_count, bindings.end());
			}
			if (candidates[m_idx].empty()) {
				return false;
			}
		}
		vector<idx_t> assigned;
		assigned.reserve(matchers.size());
		return MatchRecursive(matchers, entries, bindings, candidates, assigned);
	}

	template <class T, class MATCHER>
	static bool Match(vector<unique_ptr<MATCHER>> &matchers, vector<reference<T>> &entries,
	                  vector<reference<T>> &bindings, Policy policy) {
		if (policy == Policy::ORDERED) {
			// ordered policy, count has to match
			if (matchers.size() != entries.size()) {
				return false;
			}
			// now entries have to match in order
			for (idx_t i = 0; i < matchers.size(); i++) {
				if (!matchers[i]->Match(entries[i], bindings)) {
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
				if (!matchers[i]->Match(entries[i], bindings)) {
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
			return MatchUnordered(matchers, entries, bindings);
		}
	}

	template <class T, class MATCHER>
	static bool Match(vector<unique_ptr<MATCHER>> &matchers, vector<unique_ptr<T>> &entries,
	                  vector<reference<T>> &bindings, Policy policy) {
		// convert vector of unique_ptr to vector of normal pointers
		vector<reference<T>> ptr_entries;
		for (auto &entry : entries) {
			ptr_entries.push_back(*entry);
		}
		// then just call the normal match function
		return Match(matchers, ptr_entries, bindings, policy);
	}
};

} // namespace duckdb
