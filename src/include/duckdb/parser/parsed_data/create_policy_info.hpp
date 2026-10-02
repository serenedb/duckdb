//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_policy_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/policy_command.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"

namespace duckdb {

struct CreatePolicyInfo : public CreateInfo {
	CreatePolicyInfo();

	const Identifier &GetPolicyName() const {
		return qualified_name.Name();
	}
	void SetPolicyName(Identifier name) {
		qualified_name = qualified_name.WithName(std::move(name));
	}

	unique_ptr<BaseTableRef> base_table;
	idx_t relation_oid = 0;
	bool permissive = true;
	PolicyCommand command = PolicyCommand::ALL;
	vector<Identifier> role_names;
	vector<idx_t> roles;
	unique_ptr<ParsedExpression> using_expr;
	unique_ptr<ParsedExpression> check_expr;

public:
	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;

	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	static string RolesToString(const vector<Identifier> &role_names);
};

} // namespace duckdb
