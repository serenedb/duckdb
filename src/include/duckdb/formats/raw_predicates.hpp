//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/raw_predicates.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"

namespace duckdb_re2 {
class LiteralFinder;
} // namespace duckdb_re2

namespace duckdb {

class Expression;

enum class RawPredicateKind : uint8_t { COMPARE, IS_NULL, IS_NOT_NULL, LIKE, NOT_LIKE };

struct RawPredicate {
	RawPredicateKind kind;
	ExpressionType compare = ExpressionType::INVALID;
	vector<string> values;
	bool anchored_start = false;
	bool anchored_end = false;
};

bool CollectRawPattern(const Expression &expr, bool negated, vector<RawPredicate> &out);
bool CollectRawPredicates(const Expression &expr, bool string_column, vector<RawPredicate> &out);
bool RawPasses(const vector<RawPredicate> &predicates, const char *value, idx_t size, bool null);

class RawContains {
public:
	explicit RawContains(string needle);
	~RawContains();

	static shared_ptr<RawContains> TryCreate(const vector<RawPredicate> &predicates);

	idx_t Find(const char *base, const uint32_t *starts, const uint32_t *ends, idx_t count, uint32_t *matches) const;

private:
	const string needle;
	unique_ptr<duckdb_re2::LiteralFinder> finder;
};

} // namespace duckdb
