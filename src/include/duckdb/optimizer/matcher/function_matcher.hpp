//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/matcher/function_matcher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include <absl/container/flat_hash_set.h>
#include <algorithm>

namespace duckdb {

//! The FunctionMatcher class contains a set of matchers that can be used to pattern match specific functions
class FunctionMatcher {
public:
	virtual ~FunctionMatcher() {
	}

	virtual bool Match(const string &name) = 0;

	static bool Match(unique_ptr<FunctionMatcher> &matcher, const string &name) {
		if (!matcher) {
			return true;
		}
		return matcher->Match(name);
	}
};

//! The SpecificFunctionMatcher class matches a single specified function name
class SpecificFunctionMatcher : public FunctionMatcher {
public:
	explicit SpecificFunctionMatcher(string name_p) : name(std::move(name_p)) {
	}

	bool Match(const string &matched_name) override {
		return matched_name == this->name;
	}

private:
	string name;
};

//! The ManyFunctionMatcher class matches a set of functions
class ManyFunctionMatcher : public FunctionMatcher {
public:
	explicit ManyFunctionMatcher(absl::flat_hash_set<std::string_view> names_p) : names(std::move(names_p)) {
	}

	bool Match(const string &name) override {
		return names.find(name) != names.end();
	}

private:
	absl::flat_hash_set<std::string_view> names;
};

} // namespace duckdb
