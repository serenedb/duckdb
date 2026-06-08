#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

// RBAC DDL rewritten into serenedb_* pragmas (handlers in
// server/connector/duckdb_rbac_function.cpp). Mirrors the TSDictionary transformer.

namespace {

// A role identifier is a ColId, which has a registered Transform<string>.
string TransformColIdName(PEGTransformer &transformer, ParseResult &col_id_pr) {
	return transformer.Transform<string>(col_id_pr);
}

// QualifiedName -> dotted "catalog.schema.name" (same as the TSDictionary path).
string QualifiedTableName(PEGTransformer &transformer, ParseResult &qname_pr) {
	auto name = transformer.Transform<QualifiedName>(qname_pr);
	string result;
	if (!name.catalog.empty() && name.catalog != INVALID_CATALOG) {
		result += name.catalog + ".";
	}
	if (!name.schema.empty() && name.schema != INVALID_SCHEMA) {
		result += name.schema + ".";
	}
	result += name.name;
	return result;
}

unique_ptr<ParsedExpression> StrConst(const string &s) {
	return make_uniq<ConstantExpression>(Value(s));
}
unique_ptr<ParsedExpression> BoolConst(bool b) {
	return make_uniq<ConstantExpression>(Value::BOOLEAN(b));
}

// LIST(Grantee) -> CHOICE -> 'PUBLIC' keyword / LIST(ColId).
string TransformGrantee(PEGTransformer &transformer, ParseResult &grantee_list) {
	auto &chosen = grantee_list.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.type == ParseResultType::KEYWORD) {
		return "PUBLIC";
	}
	return transformer.Transform<string>(chosen);
}

} // namespace

// CREATE ROLE/USER name [LOGIN] [SUPERUSER] [PASSWORD '...']
//   -> PRAGMA serenedb_create_role('name', login, superuser, 'password')
unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateRoleStatement(PEGTransformer &transformer,
                                                                             ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'CREATE' 1:RoleOrUser 2:ColId 3:RoleOptionList?
	auto name = TransformColIdName(transformer, list_pr.GetChild(2));

	bool login = false;
	bool superuser = false;
	bool has_password = false;
	string password;

	auto &opts_opt = list_pr.Child<OptionalParseResult>(3);
	if (opts_opt.HasResult()) {
		// LIST(RoleOptionList) -> child 0 is the RoleOption+ REPEAT.
		auto &repeat = opts_opt.GetResult().Cast<ListParseResult>().Child<RepeatParseResult>(0);
		for (auto &opt_ref : repeat.GetChildren()) {
			// LIST(RoleOption) -> CHOICE -> LIST(<LoginOption|SuperuserOption|PasswordOption>)
			auto &opt = opt_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
			if (opt.name == "LoginOption") {
				// LIST(LoginOption) -> CHOICE -> 'LOGIN' / 'NOLOGIN'
				auto &kw = opt.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
				login = StringUtil::Upper(kw.keyword) == "LOGIN";
			} else if (opt.name == "SuperuserOption") {
				auto &kw = opt.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
				superuser = StringUtil::Upper(kw.keyword) == "SUPERUSER";
			} else if (opt.name == "PasswordOption") {
				// LIST(PasswordOption) -> [ 'PASSWORD', StringLiteral ]
				password = opt.Cast<ListParseResult>().Child<StringLiteralParseResult>(1).result;
				has_password = true;
			} else {
				throw ParserException("Unexpected role option in CREATE ROLE: %s", opt.name);
			}
		}
	}

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_create_role";
	result->info->parameters.push_back(StrConst(name));
	result->info->parameters.push_back(BoolConst(login));
	result->info->parameters.push_back(BoolConst(superuser));
	// The pragma treats an empty password as "no password"; only forward a
	// password when PASSWORD was given.
	result->info->parameters.push_back(StrConst(has_password ? password : string()));
	return std::move(result);
}

// DROP ROLE/USER [IF EXISTS] name -> PRAGMA serenedb_drop_role('name', if_exists)
unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropRoleStatement(PEGTransformer &transformer,
                                                                           ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'DROP' 1:RoleOrUser 2:IfExists? 3:ColId
	bool if_exists = list_pr.Child<OptionalParseResult>(2).HasResult();
	auto name = TransformColIdName(transformer, list_pr.GetChild(3));

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_drop_role";
	result->info->parameters.push_back(StrConst(name));
	result->info->parameters.push_back(BoolConst(if_exists));
	return std::move(result);
}

// ALTER ROLE/USER name PASSWORD '...' -> PRAGMA serenedb_alter_role_password('name', 'password')
unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterRoleStatement(PEGTransformer &transformer,
                                                                            ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'ALTER' 1:RoleOrUser 2:ColId 3:'PASSWORD' 4:StringLiteral
	auto name = TransformColIdName(transformer, list_pr.GetChild(2));
	auto password = list_pr.Child<StringLiteralParseResult>(4).result;

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_alter_role_password";
	result->info->parameters.push_back(StrConst(name));
	result->info->parameters.push_back(StrConst(password));
	return std::move(result);
}

// PrivilegeList -> comma-joined uppercase names, or "ALL" for ALL [PRIVILEGES].
// `privs_list` is the LIST(PrivilegeList) node: child 0 is the AllPrivileges /
// List(Privilege) choice.
static string TransformPrivilegeList(PEGTransformer &transformer, ParseResult &privs_list) {
	auto &chosen = privs_list.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.name == "AllPrivileges") {
		return "ALL";
	}
	// LIST(List): each element is a LIST(Privilege) -> CHOICE -> keyword.
	string out;
	for (auto &elem_ref : PEGTransformerFactory::ExtractParseResultsFromList(chosen)) {
		auto &kw =
		    elem_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
		if (!out.empty()) {
			out += ",";
		}
		out += StringUtil::Upper(kw.keyword);
	}
	return out;
}

// Shared body for GRANT/REVOKE. `revoke` selects the direction; `inner` is the
// choice between the table-privilege and role-membership forms.
static unique_ptr<SQLStatement> BuildGrant(PEGTransformer &transformer, ChoiceParseResult &inner, bool revoke) {
	auto &form = inner.GetResult();
	auto result = make_uniq<PragmaStatement>();

	if (form.name == "GrantTablePrivilege" || form.name == "RevokeTablePrivilege") {
		// LIST(GrantTablePrivilege):
		//   0:PrivilegeList 1:'ON' 2:TableKeyword? 3:QualifiedName 4:'TO'/'FROM' 5:Grantee
		//   6:WithGrantOption?   (GRANT form only; REVOKE has no child 6)
		auto &list_pr = form.Cast<ListParseResult>();
		auto privs = TransformPrivilegeList(transformer, list_pr.GetChild(0));
		auto table = QualifiedTableName(transformer, list_pr.GetChild(3));
		auto grantee = TransformGrantee(transformer, list_pr.GetChild(5));

		// WITH GRANT OPTION is only valid on GRANT (the GrantTablePrivilege rule);
		// the RevokeTablePrivilege rule has no such child.
		bool with_grant_option = false;
		if (!revoke) {
			with_grant_option = list_pr.Child<OptionalParseResult>(6).HasResult();
		}

		result->info->name = "serenedb_grant_table";
		result->info->parameters.push_back(StrConst(privs));
		result->info->parameters.push_back(StrConst(table));
		result->info->parameters.push_back(StrConst(grantee));
		result->info->parameters.push_back(BoolConst(revoke));
		result->info->parameters.push_back(BoolConst(with_grant_option));
		return std::move(result);
	}

	// Role membership: 0:ColId(role) 1:'TO'/'FROM' 2:ColId(member)
	auto &list_pr = form.Cast<ListParseResult>();
	auto role = TransformColIdName(transformer, list_pr.GetChild(0));
	auto member = TransformColIdName(transformer, list_pr.GetChild(2));

	result->info->name = "serenedb_grant_role";
	result->info->parameters.push_back(StrConst(role));
	result->info->parameters.push_back(StrConst(member));
	result->info->parameters.push_back(BoolConst(revoke));
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformGrantStatement(PEGTransformer &transformer,
                                                                        ParseResult &parse_result) {
	// 0:'GRANT' 1:LIST[ CHOICE(GrantTablePrivilege / GrantRoleMembership) ]
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &inner = list_pr.Child<ListParseResult>(1).Child<ChoiceParseResult>(0);
	return BuildGrant(transformer, inner, /*revoke=*/false);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformRevokeStatement(PEGTransformer &transformer,
                                                                         ParseResult &parse_result) {
	// 0:'REVOKE' 1:LIST[ CHOICE(RevokeTablePrivilege / RevokeRoleMembership) ]
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &inner = list_pr.Child<ListParseResult>(1).Child<ChoiceParseResult>(0);
	return BuildGrant(transformer, inner, /*revoke=*/true);
}

} // namespace duckdb
