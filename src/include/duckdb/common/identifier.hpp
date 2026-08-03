//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/identifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/vector.hpp"

#include <iosfwd>

namespace duckdb {

//! An Identifier represents a SQL identifier (e.g. a column name, table name, alias, ...).
//! Unlike a regular string, identifiers compare case-insensitively (using StringUtil::CIEquals).
//! Internally the identifier is stored as-is (preserving the original casing), but all comparisons,
//! hashing and ordering are case-insensitive.
class Identifier {
public:
	Identifier() = default;
	//! Construction from a string literal is implicit: literals in the source are identifiers by intent.
	Identifier(const char *str) : value(str) { // NOLINT: implicit conversion from literals is intentional
	}
	//! Construction from a runtime string is explicit: an Identifier carries case-insensitive semantics, so
	//! promoting a runtime string must be a deliberate choice at the call site.
	explicit Identifier(const string &str) : value(str) {
	}
	explicit Identifier(string &&str) : value(std::move(str)) {
	}
	explicit Identifier(std::string_view str) : value(str) {
	}

	//! Named constructors for well-known identifiers
	static Identifier DefaultSchema() {
		return Identifier(DEFAULT_SCHEMA);
	}
	static Identifier InvalidSchema() {
		return Identifier(INVALID_SCHEMA);
	}
	static Identifier InvalidCatalog() {
		return Identifier(INVALID_CATALOG);
	}
	static Identifier SystemCatalog() {
		return Identifier(SYSTEM_CATALOG);
	}
	static Identifier TempCatalog() {
		return Identifier(TEMP_CATALOG);
	}

	//! Conversion back to a string is explicit: it discards the case-insensitive semantics, so callers must opt in
	//! (use GetIdentifierName() for the raw value). Keeping this explicit is what makes the Identifier type safe.
	explicit operator const string &() const {
		return value;
	}

	//! The raw underlying string (preserving original casing)
	const string &GetIdentifierName() const {
		return value;
	}

	bool empty() const { // NOLINT: match std::string interface
		return value.empty();
	}
	void clear() { // NOLINT: match std::string interface
		value.clear();
	}
	idx_t size() const { // NOLINT: match std::string interface
		return value.size();
	}
	const char *c_str() const { // NOLINT: match std::string interface
		return value.c_str();
	}

	//! Case-insensitive hash of the identifier
	DUCKDB_API hash_t Hash() const;

private:
	string value;
};

//! Equality (case-insensitive)
DUCKDB_API bool operator==(const Identifier &a, const Identifier &b);
DUCKDB_API bool operator==(const Identifier &a, const string &b);
DUCKDB_API bool operator==(const string &a, const Identifier &b);
DUCKDB_API bool operator==(const Identifier &a, const char *b);
DUCKDB_API bool operator==(const char *a, const Identifier &b);

inline bool operator!=(const Identifier &a, const Identifier &b) {
	return !(a == b);
}
inline bool operator!=(const Identifier &a, const string &b) {
	return !(a == b);
}
inline bool operator!=(const string &a, const Identifier &b) {
	return !(a == b);
}
inline bool operator!=(const Identifier &a, const char *b) {
	return !(a == b);
}
inline bool operator!=(const char *a, const Identifier &b) {
	return !(a == b);
}

//! Ordering (case-insensitive)
DUCKDB_API bool operator<(const Identifier &a, const Identifier &b);

//! Streaming an identifier writes its raw name (without quotes) - this mirrors writing the underlying string
DUCKDB_API std::ostream &operator<<(std::ostream &os, const Identifier &id);

//! String concatenation (std::operator+ is a template and cannot use the implicit conversion, so we provide our own)
inline string operator+(const Identifier &a, const string &b) {
	return a.GetIdentifierName() + b;
}
inline string operator+(const string &a, const Identifier &b) {
	return a + b.GetIdentifierName();
}
inline string operator+(const Identifier &a, const char *b) {
	return a.GetIdentifierName() + b;
}
inline string operator+(const char *a, const Identifier &b) {
	return a + b.GetIdentifierName();
}
inline string operator+(const Identifier &a, const Identifier &b) {
	return a.GetIdentifierName() + b.GetIdentifierName();
}
inline string operator+(const Identifier &a, char b) {
	return a.GetIdentifierName() + b;
}
inline string operator+(char a, const Identifier &b) {
	return a + b.GetIdentifierName();
}

//! Appending an identifier to a string appends the raw name
inline string &operator+=(string &a, const Identifier &b) {
	a += b.GetIdentifierName();
	return a;
}

//! Hashing and equality for identifier-keyed hash maps. Case-insensitive by default, which is duckdb's identifier
//! semantics; a catalog whose names are already folded by its own rules (postgres folds unquoted identifiers and then
//! matches exactly, so "A" and "a" are two columns) opts into the case-sensitive pair. Both halves carry the flag:
//! a case-insensitive hash under a case-sensitive comparison is correct but files every casing of one name in a
//! single bucket, and the reverse combination is simply wrong.
struct IdentifierHashFunction {
	IdentifierHashFunction() = default;
	explicit IdentifierHashFunction(bool case_sensitive_p) : case_sensitive(case_sensitive_p) {
	}

	DUCKDB_API uint64_t operator()(const Identifier &id) const;

	bool case_sensitive = false;
};

struct IdentifierEquality {
	IdentifierEquality() = default;
	explicit IdentifierEquality(bool case_sensitive_p) : case_sensitive(case_sensitive_p) {
	}

	bool operator()(const Identifier &a, const Identifier &b) const {
		if (case_sensitive) {
			return a.GetIdentifierName() == b.GetIdentifierName();
		}
		return a == b;
	}

	bool case_sensitive = false;
};

//! Ordering for identifier-keyed containers. Case-insensitive by default, which is duckdb's identifier
//! semantics; a catalog whose names are already folded by its own rules (postgres folds unquoted identifiers and
//! then matches exactly, so "Foo" and "foo" are two relations) opts into the case-sensitive ordering instead.
struct IdentifierCompare {
	IdentifierCompare() = default;
	explicit IdentifierCompare(bool case_sensitive_p) : case_sensitive(case_sensitive_p) {
	}

	bool operator()(const Identifier &a, const Identifier &b) const {
		if (case_sensitive) {
			return a.GetIdentifierName() < b.GetIdentifierName();
		}
		return a < b;
	}

	bool case_sensitive = false;
};

template <typename T>
using identifier_map_t = unordered_map<Identifier, T, IdentifierHashFunction, IdentifierEquality>;

using identifier_set_t = unordered_set<Identifier, IdentifierHashFunction, IdentifierEquality>;

template <typename T>
using identifier_tree_t = map<Identifier, T, IdentifierCompare>;

//! Helper to convert a vector of identifiers to a vector of (raw) strings (for interop with string-based APIs)
inline vector<string> IdentifiersToStrings(const vector<Identifier> &identifiers) {
	vector<string> result;
	result.reserve(identifiers.size());
	for (auto &identifier : identifiers) {
		result.push_back(identifier.GetIdentifierName());
	}
	return result;
}

//! Helper to convert a vector of (raw) strings to a vector of identifiers (to be removed at the end of the rework)
inline vector<Identifier> StringsToIdentifiers(const vector<string> &strings) {
	vector<Identifier> result;
	result.reserve(strings.size());
	for (auto &str : strings) {
		result.emplace_back(str);
	}
	return result;
}

//! Identifier-aware overloads of the invalid-catalog/schema checks. These live here (rather than next to the
//! string versions in constants.hpp) because constants.hpp is a dependency of this header.
inline bool IsInvalidCatalog(const Identifier &catalog) {
	return catalog.empty();
}
inline bool IsInvalidSchema(const Identifier &schema) {
	return schema.empty();
}

class IdentifierRef {
public:
	IdentifierRef() = default;
	explicit IdentifierRef(std::string_view value_p) : value(value_p) {
	}

	const auto &GetIdentifierName() const {
		return *this;
	}

	operator string() const { // NOLINT
		return string(value);
	}

	operator Identifier() const { // NOLINT
		return Identifier(value);
	}

	operator std::string_view() const { // NOLINT
		return value;
	}

	friend std::ostream &operator<<(std::ostream &o, const IdentifierRef &i) {
		return o << i.value;
	}

private:
	std::string_view value;
};

} // namespace duckdb
