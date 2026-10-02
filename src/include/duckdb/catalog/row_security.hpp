//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/row_security.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/policy_command.hpp"
#include "duckdb/common/identifier.hpp"

#include <functional>

namespace duckdb {
class Catalog;
class CatalogEntry;
class CatalogSet;
class PolicyCatalogEntry;
class SchemaCatalogEntry;
class StandardEntry;
struct AlterInfo;
struct CreatePolicyInfo;

struct RowSecurity {
	explicit RowSecurity(Catalog &catalog);

	bool enabled = false;
	bool forced = false;
	shared_ptr<CatalogSet> policies;

public:
	static optional_ptr<RowSecurity> Get(CatalogEntry &relation);
	static StandardEntry &GetRelation(CatalogTransaction transaction, SchemaCatalogEntry &schema, idx_t relation_oid);

	void Apply(RowSecurityAction action);

	optional_ptr<CatalogEntry> CreatePolicy(CatalogTransaction transaction, StandardEntry &relation,
	                                        CreatePolicyInfo &info);
	bool DropPolicy(CatalogTransaction transaction, const Identifier &name);
	bool AlterPolicy(CatalogTransaction transaction, const Identifier &name, AlterInfo &info);
	optional_ptr<PolicyCatalogEntry> GetPolicy(CatalogTransaction transaction, const Identifier &name) const;
	void ScanPolicies(CatalogTransaction transaction, const std::function<void(PolicyCatalogEntry &)> &callback) const;
	void ScanPolicies(const std::function<void(PolicyCatalogEntry &)> &callback) const;
};

} // namespace duckdb
