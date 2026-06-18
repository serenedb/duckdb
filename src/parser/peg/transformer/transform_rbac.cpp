#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
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
unique_ptr<ParsedExpression> IntConst(int32_t i) {
	return make_uniq<ConstantExpression>(Value::INTEGER(i));
}

// An optional OptionBool ('TRUE'/'FALSE') child of a member option. Returns a
// tri-state: -1 unspecified, 0 FALSE, 1 TRUE. `bool_child` is the index of the
// OptionBool? optional within the option's LIST.
int32_t TransformOptionBool(ParseResult &option_list, idx_t bool_child) {
	auto &opt = option_list.Cast<ListParseResult>().Child<OptionalParseResult>(bool_child);
	if (!opt.HasResult()) {
		return -1;
	}
	auto &kw = opt.GetResult().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
	return StringUtil::Upper(kw.keyword) == "TRUE" ? 1 : 0;
}

// LIST(Grantee) -> CHOICE -> 'PUBLIC' keyword / LIST(ColId).
string TransformGrantee(PEGTransformer &transformer, ParseResult &grantee_list) {
	auto &chosen = grantee_list.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.type == ParseResultType::KEYWORD) {
		return "PUBLIC";
	}
	return transformer.Transform<string>(chosen);
}

// Optional DropBehavior? ('CASCADE' / 'RESTRICT') at `child`. True iff CASCADE.
// Absent or RESTRICT -> false (PG default is RESTRICT).
bool TransformCascade(ParseResult &list, idx_t child) {
	auto &opt = list.Cast<ListParseResult>().Child<OptionalParseResult>(child);
	if (!opt.HasResult()) {
		return false;
	}
	auto &kw = opt.GetResult().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
	return StringUtil::Upper(kw.keyword) == "CASCADE";
}

// Render a SET <guc> value expression to the plain text PG stores in setconfig
// (e.g. 'clickclack' -> clickclack, a -> a, 5000 -> 5000). A string/identifier
// keeps its raw text; anything else falls back to the expression's ToString.
string ConfigValueText(const ParsedExpression &expr) {
	if (expr.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		auto &val = expr.Cast<ConstantExpression>().GetValue();
		return val.IsNull() ? string() : val.ToString();
	}
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		return expr.Cast<ColumnRefExpression>().GetColumnName();
	}
	return expr.ToString();
}

// Optional GrantedBy? ('GRANTED' 'BY' ColId) at `child`. "" if absent.
string TransformGrantedBy(PEGTransformer &transformer, ParseResult &list, idx_t child) {
	auto &opt = list.Cast<ListParseResult>().Child<OptionalParseResult>(child);
	if (!opt.HasResult()) {
		return string();
	}
	// LIST(GrantedBy): 0:'GRANTED' 1:'BY' 2:ColId
	return TransformColIdName(transformer, opt.GetResult().Cast<ListParseResult>().GetChild(2));
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
	bool inherit = true; // PG default: new roles INHERIT
	bool has_password = false;
	string password;
	int32_t conn_limit = -1;   // PG default: unlimited
	string valid_until;        // empty -> NULL (no expiry)

	auto &opts_opt = list_pr.Child<OptionalParseResult>(3);
	if (opts_opt.HasResult()) {
		// LIST(RoleOptionList) -> child 0 is the RoleOption+ REPEAT.
		auto &repeat = opts_opt.GetResult().Cast<ListParseResult>().Child<RepeatParseResult>(0);
		for (auto &opt_ref : repeat.GetChildren()) {
			// LIST(RoleOption) -> CHOICE -> LIST(<one of the RoleOption forms>)
			auto &opt = opt_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
			if (opt.name == "LoginOption") {
				// LIST(LoginOption) -> CHOICE -> 'LOGIN' / 'NOLOGIN'
				auto &kw = opt.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
				login = StringUtil::Upper(kw.keyword) == "LOGIN";
			} else if (opt.name == "SuperuserOption") {
				auto &kw = opt.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
				superuser = StringUtil::Upper(kw.keyword) == "SUPERUSER";
			} else if (opt.name == "InheritOption") {
				// LIST(InheritOption) -> CHOICE -> 'INHERIT' / 'NOINHERIT'
				auto &kw = opt.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
				inherit = StringUtil::Upper(kw.keyword) == "INHERIT";
			} else if (opt.name == "ConnLimitOption") {
				// LIST(ConnLimitOption): 0:'CONNECTION' 1:'LIMIT' 2:NumberLiteral
				conn_limit = std::stoi(opt.Cast<ListParseResult>().Child<NumberParseResult>(2).number);
			} else if (opt.name == "ValidUntilOption") {
				// LIST(ValidUntilOption): 0:'VALID' 1:'UNTIL' 2:StringLiteral
				valid_until = opt.Cast<ListParseResult>().Child<StringLiteralParseResult>(2).result;
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
	result->info->parameters.push_back(BoolConst(inherit));
	result->info->parameters.push_back(IntConst(conn_limit));
	result->info->parameters.push_back(StrConst(valid_until));
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

// ALTER ROLE/USER name ( RENAME TO new | <option>+ )
//   RENAME TO new  -> PRAGMA serenedb_rename_role('name', 'new')
//   <option>+      -> PRAGMA serenedb_alter_role('name', login, super, createdb,
//                            createrole, inherit, has_password, password,
//                            password_null)
// All option flags are tri-state ints (-1 unspecified / 0 false / 1 true).
unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterRoleStatement(PEGTransformer &transformer,
                                                                            ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'ALTER' 1:RoleOrUser 2:ColId 3:(AlterRoleRename / AlterRoleConfig / AlterRoleOptionList)
	auto name = TransformColIdName(transformer, list_pr.GetChild(2));
	auto &chosen = list_pr.GetChild(3).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();

	if (chosen.name == "AlterRoleRename") {
		// LIST(AlterRoleRename): 0:'RENAME' 1:'TO' 2:ColId
		auto new_name = TransformColIdName(transformer, chosen.Cast<ListParseResult>().GetChild(2));
		auto result = make_uniq<PragmaStatement>();
		result->info->name = "serenedb_rename_role";
		result->info->parameters.push_back(StrConst(name));
		result->info->parameters.push_back(StrConst(new_name));
		return std::move(result);
	}

	if (chosen.name == "AlterRoleConfig") {
		// LIST(AlterRoleConfig) -> CHOICE(AlterRoleSet / AlterRoleReset).
		auto &cfg = chosen.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		auto result = make_uniq<PragmaStatement>();
		result->info->name = "serenedb_alter_role_config";
		result->info->parameters.push_back(StrConst(name));
		if (cfg.name == "AlterRoleSet") {
			// LIST(AlterRoleSet): 0:'SET' 1:SettingName 2:GROUP-LIST -> CHOICE(
			// AlterRoleSetTo / AlterRoleSetFromCurrent). SettingName is a
			// matcher-handled IdentifierParseResult; an inline (A / B) group is
			// wrapped in its own single-child LIST.
			auto setting = cfg.Cast<ListParseResult>().Child<IdentifierParseResult>(1).identifier;
			auto &val_choice =
			    cfg.Cast<ListParseResult>().Child<ListParseResult>(2).Child<ChoiceParseResult>(0).GetResult();
			string value;
			if (val_choice.name == "AlterRoleSetTo") {
				// LIST(AlterRoleSetTo): 0:GROUP-LIST('='|'TO') 1:List(Expression). Join
				// the rendered expression values with ", " (PG's setconfig rendering).
				for (auto &e : PEGTransformerFactory::ExtractParseResultsFromList(
				         val_choice.Cast<ListParseResult>().GetChild(1))) {
					auto expr = transformer.Transform<unique_ptr<ParsedExpression>>(e.get());
					if (!value.empty()) {
						value += ", ";
					}
					value += ConfigValueText(*expr);
				}
			}
			// AlterRoleSetFromCurrent records the GUC's current value; SereneDB has
			// no session GUC store, so it persists an empty value (the GUC name
			// alone), matching PG's "setting=" shape closely enough for surfacing.
			result->info->parameters.push_back(StrConst("SET"));
			result->info->parameters.push_back(StrConst(setting));
			result->info->parameters.push_back(StrConst(value));
		} else {
			// LIST(AlterRoleReset): 0:'RESET' 1:GROUP-LIST -> CHOICE(AlterRoleResetAll
			// / SettingName).
			auto &reset_choice =
			    cfg.Cast<ListParseResult>().Child<ListParseResult>(1).Child<ChoiceParseResult>(0).GetResult();
			if (reset_choice.name == "AlterRoleResetAll") {
				result->info->parameters.push_back(StrConst("RESET_ALL"));
				result->info->parameters.push_back(StrConst(string()));
			} else {
				// SettingName is a matcher-handled IdentifierParseResult.
				result->info->parameters.push_back(StrConst("RESET"));
				result->info->parameters.push_back(
				    StrConst(reset_choice.Cast<IdentifierParseResult>().identifier));
			}
			result->info->parameters.push_back(StrConst(string()));
		}
		return std::move(result);
	}

	int32_t login = -1, super = -1, createdb = -1, createrole = -1, inherit = -1;
	int32_t has_password = -1, password_null = 0;
	int32_t conn_limit = -2;  // -2 unspecified (-1 is "unlimited")
	string password;
	string valid_until;
	bool set_valid_until = false;
	// LIST(AlterRoleOptionList) -> child 0 is the AlterRoleOption+ REPEAT.
	auto &repeat = chosen.Cast<ListParseResult>().Child<RepeatParseResult>(0);
	for (auto &opt_ref : repeat.GetChildren()) {
		auto &opt = opt_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		if (opt.name == "PasswordNullOption") {
			// LIST(PasswordNullOption): 0:'PASSWORD' 1:'NULL'
			has_password = 1;
			password_null = 1;
			continue;
		}
		if (opt.name == "PasswordOption") {
			// LIST(PasswordOption): 0:'PASSWORD' 1:StringLiteral
			has_password = 1;
			password = opt.Cast<ListParseResult>().Child<StringLiteralParseResult>(1).result;
			continue;
		}
		if (opt.name == "ConnLimitOption") {
			// LIST(ConnLimitOption): 0:'CONNECTION' 1:'LIMIT' 2:NumberLiteral
			conn_limit = std::stoi(opt.Cast<ListParseResult>().Child<NumberParseResult>(2).number);
			continue;
		}
		if (opt.name == "ValidUntilOption") {
			// LIST(ValidUntilOption): 0:'VALID' 1:'UNTIL' 2:StringLiteral
			valid_until = opt.Cast<ListParseResult>().Child<StringLiteralParseResult>(2).result;
			set_valid_until = true;
			continue;
		}
		// Every other option is a rule wrapping a CHOICE of two keywords (the
		// positive form and its NO* negation), like LoginOption.
		auto &kw = opt.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
		const int32_t on = StringUtil::Upper(kw.keyword).rfind("NO", 0) == 0 ? 0 : 1;
		if (opt.name == "LoginOption") {
			login = on;
		} else if (opt.name == "SuperuserOption") {
			super = on;
		} else if (opt.name == "CreateDbOption") {
			createdb = on;
		} else if (opt.name == "CreateRoleOption") {
			createrole = on;
		} else if (opt.name == "InheritOption") {
			inherit = on;
		} else {
			throw ParserException("Unexpected role option in ALTER ROLE: %s", opt.name);
		}
	}

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_alter_role";
	result->info->parameters.push_back(StrConst(name));
	result->info->parameters.push_back(IntConst(login));
	result->info->parameters.push_back(IntConst(super));
	result->info->parameters.push_back(IntConst(createdb));
	result->info->parameters.push_back(IntConst(createrole));
	result->info->parameters.push_back(IntConst(inherit));
	result->info->parameters.push_back(IntConst(has_password));
	result->info->parameters.push_back(StrConst(password));
	result->info->parameters.push_back(IntConst(password_null));
	result->info->parameters.push_back(IntConst(conn_limit));
	result->info->parameters.push_back(BoolConst(set_valid_until));
	result->info->parameters.push_back(StrConst(valid_until));
	return std::move(result);
}

// ALTER <objtype> name OWNER TO role
//   -> PRAGMA serenedb_alter_owner('objtype', 'name', 'new_owner')
unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterOwnerStatement(PEGTransformer &transformer,
                                                                            ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'ALTER' 1:OwnerObjectType 2:QualifiedName 3:'OWNER' 4:'TO' 5:OwnerRoleSpec
	// OwnerObjectType is a rule wrapping a CHOICE of keywords; child 0 of its
	// LIST is the CHOICE (mirrors LoginOption handling above).
	auto &obj_kw = list_pr.GetChild(1).Cast<ListParseResult>()
	                   .Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
	auto objtype = StringUtil::Upper(obj_kw.keyword);
	auto name = QualifiedTableName(transformer, list_pr.GetChild(2));

	// OwnerRoleSpec: a rule wrapping CHOICE(CURRENT_USER / SESSION_USER /
	// CURRENT_ROLE keyword / ColId).
	auto &role_choice = list_pr.GetChild(5).Cast<ListParseResult>()
	                        .Child<ChoiceParseResult>(0).GetResult();
	string new_owner;
	if (role_choice.type == ParseResultType::KEYWORD) {
		new_owner = StringUtil::Upper(role_choice.Cast<KeywordParseResult>().keyword);
	} else {
		new_owner = transformer.Transform<string>(role_choice);
	}

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_alter_owner";
	result->info->parameters.push_back(StrConst(objtype));
	result->info->parameters.push_back(StrConst(name));
	result->info->parameters.push_back(StrConst(new_owner));
	return std::move(result);
}

// PrivilegeList -> comma-joined uppercase names, or "ALL" for ALL [PRIVILEGES].
// `privs_list` is the LIST(PrivilegeList) node: child 0 is the AllPrivileges /
// List(Privilege) choice. A Privilege element is either a known keyword or a
// bare ColId (an arbitrary identifier the command layer rejects with PG's
// "unrecognized privilege type" message).
static string TransformPrivilegeList(PEGTransformer &transformer, ParseResult &privs_list) {
	auto &chosen = privs_list.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.name == "AllPrivileges") {
		return "ALL";
	}
	// LIST(List): each element is a LIST(Privilege) -> CHOICE -> keyword / ColId.
	string out;
	for (auto &elem_ref : PEGTransformerFactory::ExtractParseResultsFromList(chosen)) {
		auto &priv = elem_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		if (!out.empty()) {
			out += ",";
		}
		if (priv.type == ParseResultType::KEYWORD) {
			out += StringUtil::Upper(priv.Cast<KeywordParseResult>().keyword);
		} else {
			// ColId: pass through (the matcher already downcased an unquoted
			// identifier); the command layer maps it to PG's message.
			out += transformer.Transform<string>(priv);
		}
	}
	return out;
}

// Map a GrantObjTypePlural keyword (TABLES/FUNCTIONS/...) to the singular bulk
// objtype string the pragma understands.
static string PluralObjType(const string &word) {
	auto w = StringUtil::Upper(word);
	if (w == "TABLES") {
		return "ALL_TABLES_IN_SCHEMA";
	}
	if (w == "SEQUENCES") {
		return "ALL_SEQUENCES_IN_SCHEMA";
	}
	// FUNCTIONS / ROUTINES / PROCEDURES
	return "ALL_FUNCTIONS_IN_SCHEMA";
}

// A GrantTarget -> (objtype, name). `name` is the dotted object name, or the
// schema name for the ALL ... IN SCHEMA bulk forms. The objtype for a function
// signature stays FUNCTION (SereneDB models functions by name, so the argtype
// list only disambiguates and is not needed for resolution).
struct GrantTargetInfo {
	string objtype;
	string name;
};
static GrantTargetInfo TransformGrantTarget(PEGTransformer &transformer, ParseResult &target_choice_holder) {
	auto &chosen = target_choice_holder.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.name == "GrantAllInSchema") {
		// 0:'ALL' 1:GrantObjTypePlural 2:'IN' 3:'SCHEMA' 4:QualifiedName
		auto &kw = chosen.Cast<ListParseResult>().GetChild(1).Cast<ListParseResult>()
		               .Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
		return {PluralObjType(kw.keyword), QualifiedTableName(transformer, chosen.Cast<ListParseResult>().GetChild(4))};
	}
	if (chosen.name == "GrantFunctionTarget") {
		// 0:GrantRoutineKind 1:QualifiedName 2:FuncArgSignature?
		return {"FUNCTION", QualifiedTableName(transformer, chosen.Cast<ListParseResult>().GetChild(1))};
	}
	// GrantNamedTarget: 0:GrantObjType? 1:QualifiedName
	auto &named = chosen.Cast<ListParseResult>();
	string objtype = "TABLE";
	auto &objtype_opt = named.Child<OptionalParseResult>(0);
	if (objtype_opt.HasResult()) {
		auto &kw = objtype_opt.GetResult().Cast<ListParseResult>()
		               .Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
		objtype = StringUtil::Upper(kw.keyword);
	}
	return {objtype, QualifiedTableName(transformer, named.GetChild(1))};
}

// Build a serenedb_grant_table pragma. `option_only` marks the REVOKE GRANT
// OPTION FOR downgrade (keep the privilege, drop only its grant option).
// Children from `privs_child`: 0:PrivilegeList 1:'ON' 2:GrantTarget 3:TO/FROM
// 4:Grantee 5:WithGrantOption?/DropBehavior? 6:GrantedBy?
static unique_ptr<SQLStatement> BuildGrantTable(PEGTransformer &transformer, ListParseResult &list_pr, bool revoke,
                                                idx_t privs_child, bool option_only) {
	auto privs = TransformPrivilegeList(transformer, list_pr.GetChild(privs_child));

	auto target = TransformGrantTarget(transformer, list_pr.GetChild(privs_child + 2));
	const string &objtype = target.objtype;
	const string &table = target.name;
	auto grantee = TransformGrantee(transformer, list_pr.GetChild(privs_child + 4));

	// GRANT: WithGrantOption? at +5, GrantedBy? at +6.
	// REVOKE table: DropBehavior? at +5 (cascade). Both never both present.
	bool with_grant_option = false;
	bool cascade = false;
	string granted_by;
	if (revoke) {
		// REVOKE [GRANT OPTION FOR] ...: DropBehavior? immediately after Grantee.
		cascade = TransformCascade(list_pr, privs_child + 5);
	} else {
		with_grant_option = list_pr.Child<OptionalParseResult>(privs_child + 5).HasResult();
		granted_by = TransformGrantedBy(transformer, list_pr, privs_child + 6);
	}

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_grant_table";
	result->info->parameters.push_back(StrConst(privs));
	result->info->parameters.push_back(StrConst(table));
	result->info->parameters.push_back(StrConst(grantee));
	result->info->parameters.push_back(BoolConst(revoke));
	result->info->parameters.push_back(BoolConst(with_grant_option));
	result->info->parameters.push_back(StrConst(objtype));
	result->info->parameters.push_back(BoolConst(option_only));
	result->info->parameters.push_back(BoolConst(cascade));
	result->info->parameters.push_back(StrConst(granted_by));
	return std::move(result);
}

// Build a serenedb_grant_role pragma. `option_only` marks REVOKE ADMIN OPTION
// FOR (keep the membership edge, drop only its admin option).
static unique_ptr<SQLStatement> BuildGrantRole(PEGTransformer &transformer, ListParseResult &list_pr, bool revoke,
                                               bool option_only, idx_t opts_child) {
	auto role = TransformColIdName(transformer, list_pr.GetChild(0));
	auto member = TransformColIdName(transformer, list_pr.GetChild(2));

	// Per-edge options, tri-state (-1 unspecified / 0 false / 1 true). REVOKE has
	// no option list. ADMIN OPTION FOR forces admin := 0.
	int32_t admin = option_only ? 0 : -1, inherit = -1, set_opt = -1;
	string granted_by;
	if (!revoke) {
		auto &opts_opt = list_pr.Child<OptionalParseResult>(opts_child);
		if (opts_opt.HasResult()) {
			// LIST(MemberOptionList): 0:'WITH' 1:List(MemberOption). Flatten the
			// List into its MemberOption elements (each a CHOICE of the 3 forms).
			auto &mol = opts_opt.GetResult().Cast<ListParseResult>();
			for (auto &opt_ref : PEGTransformerFactory::ExtractParseResultsFromList(mol.GetChild(1))) {
				auto &chosen = opt_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
				int32_t v = TransformOptionBool(chosen, 1);
				if (v == -1) {
					v = 1; // bare 'WITH ADMIN OPTION' / 'WITH INHERIT' means TRUE
				}
				if (chosen.name == "AdminOption") {
					admin = v;
				} else if (chosen.name == "InheritMemberOption") {
					inherit = v;
				} else {
					set_opt = v;
				}
			}
		}
		granted_by = TransformGrantedBy(transformer, list_pr, opts_child + 1);
	}

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_grant_role";
	result->info->parameters.push_back(StrConst(role));
	result->info->parameters.push_back(StrConst(member));
	result->info->parameters.push_back(BoolConst(revoke));
	result->info->parameters.push_back(IntConst(admin));
	result->info->parameters.push_back(IntConst(inherit));
	result->info->parameters.push_back(IntConst(set_opt));
	result->info->parameters.push_back(BoolConst(option_only));
	return std::move(result);
}

// Shared body for GRANT/REVOKE. `revoke` selects the direction; `inner` is the
// choice between the table-privilege, role-membership, and (REVOKE-only)
// OPTION-FOR downgrade forms.
static unique_ptr<SQLStatement> BuildGrant(PEGTransformer &transformer, ChoiceParseResult &inner, bool revoke) {
	auto &form = inner.GetResult();

	// GrantTablePrivilege / RevokeTablePrivilege: PrivilegeList at child 0.
	if (form.name == "GrantTablePrivilege" || form.name == "RevokeTablePrivilege") {
		return BuildGrantTable(transformer, form.Cast<ListParseResult>(), revoke, /*privs_child=*/0,
		                       /*option_only=*/false);
	}
	// REVOKE GRANT OPTION FOR <priv> ... : 0:'GRANT' 1:'OPTION' 2:'FOR', then
	// the same shape as RevokeTablePrivilege starting at child 3.
	if (form.name == "RevokeGrantOptionFor") {
		return BuildGrantTable(transformer, form.Cast<ListParseResult>(), /*revoke=*/true, /*privs_child=*/3,
		                       /*option_only=*/true);
	}
	// REVOKE ADMIN OPTION FOR <role> FROM <member>: 0:'ADMIN' 1:'OPTION' 2:'FOR'
	// 3:ColId(role) 4:'FROM' 5:ColId(member) 6:DropBehavior?. Reuse the role
	// builder by viewing children 3.. as role/from/member.
	if (form.name == "RevokeAdminOptionFor") {
		auto &list_pr = form.Cast<ListParseResult>();
		auto role = TransformColIdName(transformer, list_pr.GetChild(3));
		auto member = TransformColIdName(transformer, list_pr.GetChild(5));
		auto result = make_uniq<PragmaStatement>();
		result->info->name = "serenedb_grant_role";
		result->info->parameters.push_back(StrConst(role));
		result->info->parameters.push_back(StrConst(member));
		result->info->parameters.push_back(BoolConst(true)); // revoke
		result->info->parameters.push_back(IntConst(0));      // admin := 0
		result->info->parameters.push_back(IntConst(-1));     // inherit unspecified
		result->info->parameters.push_back(IntConst(-1));     // set unspecified
		result->info->parameters.push_back(BoolConst(true));  // option_only
		return std::move(result);
	}

	// GrantRoleMembership / RevokeRoleMembership: 0:role 2:member, options at 3.
	return BuildGrantRole(transformer, form.Cast<ListParseResult>(), revoke, /*option_only=*/false,
	                      /*opts_child=*/3);
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

namespace {

// DefaultPrivObjType keyword (TABLES/SEQUENCES/...) -> pg_default_acl objtype
// char (r/S/f/T/n). FUNCTIONS and ROUTINES both map to 'f'.
string DefaultPrivObjTypeChar(const string &word) {
	auto w = StringUtil::Upper(word);
	if (w == "SEQUENCES") {
		return "S";
	}
	if (w == "FUNCTIONS" || w == "ROUTINES") {
		return "f";
	}
	if (w == "TYPES") {
		return "T";
	}
	if (w == "SCHEMAS") {
		return "n";
	}
	return "r";  // TABLES
}

} // namespace

// ALTER DEFAULT PRIVILEGES [FOR ROLE r,...] [IN SCHEMA s,...] (GRANT|REVOKE) ...
//   -> PRAGMA serenedb_alter_default_privileges(privileges, objtype_char,
//        grantee, revoke, with_grant_option, for_role, in_schema,
//        grant_option_only, cascade)
// for_role/in_schema are empty when unspecified (defaults: current user / all
// schemas). SereneDB supports a single FOR ROLE / IN SCHEMA target (PG allows
// lists; the catalog write below uses the first of each).
unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterDefaultPrivilegesStatement(PEGTransformer &transformer,
                                                                                         ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'ALTER' 1:'DEFAULT' 2:'PRIVILEGES' 3:DefaultPrivForRole? 4:DefaultPrivInSchema?
	// 5:DefaultPrivAction. PG allows a list of roles / schemas; SereneDB writes a
	// pg_default_acl row per role/schema using the first of each.
	string for_role;
	string in_schema;
	if (auto &for_opt = list_pr.Child<OptionalParseResult>(3); for_opt.HasResult()) {
		// LIST(DefaultPrivForRole): 0:'FOR' 1:RoleOrUser 2:List(ColId)
		auto &for_role_pr = for_opt.GetResult().Cast<ListParseResult>();
		for_role = TransformColIdName(
		    transformer,
		    PEGTransformerFactory::ExtractParseResultsFromList(for_role_pr.GetChild(2))[0].get());
	}
	if (auto &schema_opt = list_pr.Child<OptionalParseResult>(4); schema_opt.HasResult()) {
		// LIST(DefaultPrivInSchema): 0:'IN' 1:'SCHEMA' 2:List(QualifiedName)
		auto &schema_pr = schema_opt.GetResult().Cast<ListParseResult>();
		in_schema = QualifiedTableName(
		    transformer,
		    PEGTransformerFactory::ExtractParseResultsFromList(schema_pr.GetChild(2))[0].get());
	}

	auto &action = list_pr.GetChild(5).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	const bool revoke = action.name == "DefaultPrivRevoke";

	string privs;
	string objtype_char;
	string grantee;
	bool with_grant_option = false;
	bool grant_option_only = false;
	bool cascade = false;
	if (!revoke) {
		// DefaultPrivGrant: 0:'GRANT' 1:PrivilegeList 2:'ON' 3:DefaultPrivObjType
		// 4:'TO' 5:Grantee 6:WithGrantOption?
		auto &g = action.Cast<ListParseResult>();
		privs = TransformPrivilegeList(transformer, g.GetChild(1));
		auto &kw = g.GetChild(3).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
		objtype_char = DefaultPrivObjTypeChar(kw.keyword);
		grantee = TransformGrantee(transformer, g.GetChild(5));
		with_grant_option = g.Child<OptionalParseResult>(6).HasResult();
	} else {
		// DefaultPrivRevoke: 0:'REVOKE' 1:DefaultPrivRevokeGrantOpt? 2:PrivilegeList
		// 3:'ON' 4:DefaultPrivObjType 5:'FROM' 6:Grantee 7:DropBehavior?
		auto &g = action.Cast<ListParseResult>();
		grant_option_only = g.Child<OptionalParseResult>(1).HasResult();
		privs = TransformPrivilegeList(transformer, g.GetChild(2));
		auto &kw = g.GetChild(4).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
		objtype_char = DefaultPrivObjTypeChar(kw.keyword);
		grantee = TransformGrantee(transformer, g.GetChild(6));
		cascade = TransformCascade(g, 7);
	}

	auto result = make_uniq<PragmaStatement>();
	result->info->name = "serenedb_alter_default_privileges";
	result->info->parameters.push_back(StrConst(privs));
	result->info->parameters.push_back(StrConst(objtype_char));
	result->info->parameters.push_back(StrConst(grantee));
	result->info->parameters.push_back(BoolConst(revoke));
	result->info->parameters.push_back(BoolConst(with_grant_option));
	result->info->parameters.push_back(StrConst(for_role));
	result->info->parameters.push_back(StrConst(in_schema));
	result->info->parameters.push_back(BoolConst(grant_option_only));
	result->info->parameters.push_back(BoolConst(cascade));
	return std::move(result);
}

} // namespace duckdb
