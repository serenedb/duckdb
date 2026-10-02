#include "duckdb/parser/parsed_data/create_policy_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/create_statement.hpp"

namespace duckdb {

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreatePolicyStmt(
    PEGTransformer &transformer, const Identifier &policy_name, unique_ptr<BaseTableRef> base_table_name,
    const optional<bool> &policy_permissive, const optional<PolicyCommand> &policy_for_cmd,
    const optional<vector<Identifier>> &policy_to_roles, optional<unique_ptr<ParsedExpression>> policy_using,
    optional<unique_ptr<ParsedExpression>> policy_check) {
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreatePolicyInfo>();
	info->SetPolicyName(policy_name);
	info->base_table = std::move(base_table_name);
	info->permissive = !policy_permissive || *policy_permissive;
	if (policy_for_cmd) {
		info->command = *policy_for_cmd;
	}
	if (policy_to_roles) {
		info->role_names = *policy_to_roles;
	} else {
		info->role_names.emplace_back("PUBLIC");
	}
	if (policy_using) {
		info->using_expr = std::move(*policy_using);
	}
	if (policy_check) {
		info->check_expr = std::move(*policy_check);
	}
	result->info = std::move(info);
	return result;
}

bool PEGTransformerFactory::TransformPolicyPermissiveKeyword(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformPolicyRestrictiveKeyword(PEGTransformer &transformer) {
	return false;
}

PolicyCommand PEGTransformerFactory::TransformPolicyCommandAll(PEGTransformer &transformer) {
	return PolicyCommand::ALL;
}

PolicyCommand PEGTransformerFactory::TransformPolicyCommandSelect(PEGTransformer &transformer) {
	return PolicyCommand::SELECT;
}

PolicyCommand PEGTransformerFactory::TransformPolicyCommandInsert(PEGTransformer &transformer) {
	return PolicyCommand::INSERT;
}

PolicyCommand PEGTransformerFactory::TransformPolicyCommandUpdate(PEGTransformer &transformer) {
	return PolicyCommand::UPDATE;
}

PolicyCommand PEGTransformerFactory::TransformPolicyCommandDelete(PEGTransformer &transformer) {
	return PolicyCommand::DELETE;
}

Identifier PEGTransformerFactory::TransformPolicyCurrentRole(PEGTransformer &transformer) {
	return Identifier("CURRENT_ROLE");
}

Identifier PEGTransformerFactory::TransformPolicyCurrentUser(PEGTransformer &transformer) {
	return Identifier("CURRENT_USER");
}

Identifier PEGTransformerFactory::TransformPolicySessionUser(PEGTransformer &transformer) {
	return Identifier("SESSION_USER");
}

} // namespace duckdb
