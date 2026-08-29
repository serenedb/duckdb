//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/macro_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/function/macro_function.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

//! A macro function in the catalog
class MacroCatalogEntry : public FunctionEntry {
public:
	MacroCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateMacroInfo &info);

	//! The macro function
	vector<unique_ptr<MacroFunction>> macros;
	//! Whether this is a procedure (CALL only, not usable in SELECT/FROM)
	bool is_procedure = false;

public:
	unique_ptr<CreateInfo> GetInfo() const override;
	unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info) override;

	string ToSQL() const override;

	//! The overload whose parameter types are exactly `parameters` (empty = the zero-argument overload),
	//! or null when the entry holds none
	DUCKDB_API optional_ptr<const MacroFunction> FindOverload(const vector<LogicalType> &parameters) const;
	//! The definition left after DROP FUNCTION name(parameters): the matching overload removed and the
	//! catalog type retagged to the survivors. Null when no overload matches; a definition holding no
	//! macros when the removed overload was the last one.
	DUCKDB_API unique_ptr<CreateMacroInfo> WithoutOverload(const vector<LogicalType> &parameters) const;
	//! The definition left after DROP FUNCTION / DROP PROCEDURE by bare name: every overload whose
	//! is_procedure equals `procedures` removed. Null when the entry holds no overload of that kind.
	DUCKDB_API unique_ptr<CreateMacroInfo> WithoutKind(bool procedures) const;
	//! The definition CREATE FUNCTION leaves over an existing entry: overloads with a new signature are
	//! appended, and a declared overload matching an existing signature replaces it when
	//! `replace_matching` -- or makes the merge answer null when it is not allowed to.
	DUCKDB_API unique_ptr<CreateMacroInfo> MergedWith(const CreateMacroInfo &declared, bool replace_matching) const;
};

} // namespace duckdb
