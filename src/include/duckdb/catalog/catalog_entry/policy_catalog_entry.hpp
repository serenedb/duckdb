//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/policy_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/enums/policy_command.hpp"
#include "duckdb/parser/parsed_data/create_policy_info.hpp"

namespace duckdb {

class PolicyCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::POLICY_ENTRY;
	static constexpr const char *Name = "policy";

public:
	PolicyCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreatePolicyInfo &info);

	idx_t relation_oid;
	bool permissive;
	PolicyCommand command;
	vector<idx_t> roles;
	unique_ptr<ParsedExpression> using_expr;
	unique_ptr<ParsedExpression> check_expr;

public:
	bool AppliesTo(PolicyCommand cmd) const {
		return command == PolicyCommand::ALL || command == cmd;
	}
	bool ReferencesColumn(const Identifier &column, const IdentifierEquality &same) const;

	unique_ptr<CatalogEntry> AlterEntry(CatalogTransaction transaction, AlterInfo &info) override;
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
	unique_ptr<CreateInfo> GetInfo() const override;

	static vector<idx_t> ResolveRoles(const vector<idx_t> &roles, const vector<Identifier> &role_names);
};

} // namespace duckdb
