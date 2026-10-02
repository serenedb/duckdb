//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_policy_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/policy_command.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

struct AlterPolicyInfo : public AlterInfo {
	AlterPolicyInfo(AlterPolicyType alter_policy_type, AlterEntryData data, unique_ptr<BaseTableRef> base_table);
	~AlterPolicyInfo() override;

	AlterPolicyType alter_policy_type;
	unique_ptr<BaseTableRef> base_table;
	Identifier new_name;
	vector<Identifier> role_names;
	vector<idx_t> roles;
	unique_ptr<ParsedExpression> using_expr;
	unique_ptr<ParsedExpression> check_expr;

public:
	CatalogType GetCatalogType() const override;
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

private:
	AlterPolicyInfo();
};

} // namespace duckdb
