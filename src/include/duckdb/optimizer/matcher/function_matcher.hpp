//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/matcher/function_matcher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include <algorithm>

namespace duckdb {

//! The FunctionMatcher class contains a set of matchers that can be used to pattern match specific functions
class FunctionMatcher {
public:
	virtual ~FunctionMatcher() {
	}

	virtual bool Match(const Identifier &name) = 0;

	static bool Match(unique_ptr<FunctionMatcher> &matcher, const Identifier &name) {
		if (!matcher) {
			return true;
		}
		return matcher->Match(name);
	}
};

//! The SpecificFunctionMatcher class matches a single specified function name
class SpecificFunctionMatcher : public FunctionMatcher {
public:
	explicit SpecificFunctionMatcher(Identifier name_p) : name(std::move(name_p)) {
	}

	bool Match(const Identifier &matched_name) override {
		return matched_name == this->name;
	}

private:
	Identifier name;
};

//! The ManyFunctionMatcher class matches a set of functions.
//! It only references the set - pass storage that outlives the matcher (e.g. a function-local static).
class ManyFunctionMatcher : public FunctionMatcher {
public:
	explicit ManyFunctionMatcher(const case_insensitive_set_view_t *names_p) : names(*names_p) {
	}

	bool Match(const Identifier &name) override {
		return names.find(name.GetIdentifierName()) != names.end();
	}

private:
	const case_insensitive_set_view_t &names;
};

} // namespace duckdb
