#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <charconv>
#include <string_view>

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/permissions.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_role_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/statement/set_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

namespace {

// A role identifier is a ColId, which has a registered Transform<string>.
string TransformColIdName(PEGTransformer &transformer, ParseResult &col_id_pr) {
	return transformer.Transform<string>(col_id_pr);
}

// QualifiedName -> dotted "catalog.schema.name" (same as the TSDictionary path).
string QualifiedTableName(PEGTransformer &transformer, ParseResult &qname_pr) {
	auto name = transformer.Transform<QualifiedName>(qname_pr);
	string result;
	if (!name.Catalog().empty()) {
		result += name.Catalog();
		result += ".";
	}
	if (!name.Schema().empty()) {
		result += name.Schema();
		result += ".";
	}
	result += name.Name();
	return result;
}

// 'VALID' 'UNTIL' StringLiteral -> microseconds since the Unix epoch (UTC). The
// literal is parsed here so the value travels typed; a bad literal is a parse
// error (sqlstate diverges from PG's 22007, which is accepted).
int64_t TransformValidUntil(ParseResult &opt) {
	string literal(opt.Cast<ListParseResult>().GetChild(2).Cast<StringLiteralParseResult>().result);
	timestamp_t parsed;
	if (Timestamp::TryConvertTimestamp(literal.c_str(), literal.size(), parsed,
	                                   /*use_offset=*/true) != TimestampCastResult::SUCCESS) {
		throw InvalidInputException("invalid input syntax for type timestamp with time zone: \"%s\"", literal);
	}
	return parsed.value;
}

// 'CONNECTION' 'LIMIT' '-'? NumberLiteral -> the signed integer.
int32_t TransformConnLimit(ParseResult &opt) {
	auto &cl = opt.Cast<ListParseResult>();
	const bool negative = cl.Child<OptionalParseResult>(2).HasResult();
	int32_t value = 0;
	auto digits = cl.GetChild(3).Cast<NumberParseResult>().number;
	auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
	if (ec != std::errc() || ptr != digits.data() + digits.size() || (negative && value > 1)) {
		throw ParserException("invalid connection limit: %s%s", negative ? "-" : "", digits);
	}
	return negative ? -value : value;
}

// Walk down through Choice/List/Optional wrappers to the first KEYWORD leaf.
// Keyword-only rules (e.g. CascadeDropBehavior <- 'CASCADE') resolve to a bare
// KEYWORD node, so callers cannot assume a fixed List/Choice nesting.
KeywordParseResult &FirstKeyword(ParseResult &node) {
	ParseResult *cur = &node;
	for (;;) {
		switch (cur->type) {
		case ParseResultType::KEYWORD:
			return cur->Cast<KeywordParseResult>();
		case ParseResultType::CHOICE:
			cur = &cur->Cast<ChoiceParseResult>().GetResult();
			break;
		case ParseResultType::OPTIONAL:
			cur = &cur->Cast<OptionalParseResult>().GetResult();
			break;
		case ParseResultType::LIST:
			cur = &cur->Cast<ListParseResult>().GetChild(0);
			break;
		default:
			return cur->Cast<KeywordParseResult>();
		}
	}
}

// An optional OptionBool ('TRUE'/'FALSE') child of a member option. Returns a
// tri-state: -1 unspecified, 0 FALSE, 1 TRUE. `bool_child` is the index of the
// OptionBool? optional within the option's LIST.
int32_t TransformOptionBool(ParseResult &option_list, idx_t bool_child) {
	auto &opt = option_list.Cast<ListParseResult>().Child<OptionalParseResult>(bool_child);
	if (!opt.HasResult()) {
		return -1;
	}
	auto &kw = FirstKeyword(opt.GetResult());
	return StringUtil::Upper(kw.keyword) == "TRUE" ? 1 : 0;
}

// LIST(Grantee) -> CHOICE -> 'PUBLIC' keyword / LIST(ColId).
string TransformGrantee(PEGTransformer &transformer, ParseResult &grantee_list) {
	auto &chosen = grantee_list.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.type == ParseResultType::KEYWORD) {
		return "public";
	}
	return PEGTransformerFactory::OwnerRoleName(transformer, chosen);
}

// Optional DropBehavior? ('CASCADE' / 'RESTRICT') at `child`. True iff CASCADE.
// Absent or RESTRICT -> false (PG default is RESTRICT).
bool TransformCascade(ParseResult &list, idx_t child) {
	auto &opt = list.Cast<ListParseResult>().Child<OptionalParseResult>(child);
	if (!opt.HasResult()) {
		return false;
	}
	auto &kw = FirstKeyword(opt.GetResult());
	return StringUtil::Upper(kw.keyword) == "CASCADE";
}

// Render a SET <guc> value expression to the plain text PG stores in setconfig
// (e.g. 'clickclack' -> clickclack, a -> a, 5000 -> 5000). A string/identifier
// keeps its raw text; anything else falls back to the expression's ToString.
string ConfigValueText(const ParsedExpression &expr) {
	if (expr.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		auto &literal = expr.Cast<ConstantExpression>().GetLiteral();
		return literal.IsNull() ? string() : literal.ToValue().ToString();
	}
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		return expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
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
	return PEGTransformerFactory::OwnerRoleName(transformer, opt.GetResult().Cast<ListParseResult>().GetChild(2));
}

CatalogType GrantObjectType(const string &keyword) {
	if (keyword == "TABLE" || keyword == "VIEW" || keyword == "TABLES") {
		return CatalogType::TABLE_ENTRY;
	}
	if (keyword == "SEQUENCE" || keyword == "SEQUENCES") {
		return CatalogType::SEQUENCE_ENTRY;
	}
	if (keyword == "FUNCTION" || keyword == "PROCEDURE" || keyword == "ROUTINE" || keyword == "FUNCTIONS" ||
	    keyword == "PROCEDURES" || keyword == "ROUTINES") {
		return CatalogType::MACRO_ENTRY;
	}
	if (keyword == "TYPE" || keyword == "DOMAIN" || keyword == "TYPES") {
		return CatalogType::TYPE_ENTRY;
	}
	if (keyword == "SCHEMA" || keyword == "SCHEMAS") {
		return CatalogType::SCHEMA_ENTRY;
	}
	if (keyword == "DATABASE") {
		return CatalogType::DATABASE_ENTRY;
	}
	if (keyword == "JOB") {
		return CatalogType::JOB_ENTRY;
	}
	if (keyword == "FOREIGN SERVER" || keyword == "SERVER") {
		return CatalogType::FOREIGN_SERVER_ENTRY;
	}
	if (keyword == "FOREIGN DATA WRAPPER") {
		return CatalogType::FOREIGN_DATA_WRAPPER_ENTRY;
	}
	if (keyword == "TEXT SEARCH DICTIONARY") {
		return CatalogType::TOKENIZER_ENTRY;
	}
	throw ParserException("unrecognized privilege object type %s", keyword);
}

string ObjectTypeKeyword(ParseResult &objtype) {
	auto &chosen = objtype.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.type == ParseResultType::KEYWORD) {
		return StringUtil::Upper(chosen.Cast<KeywordParseResult>().keyword);
	}
	// A multi-keyword object type (e.g. FOREIGN SERVER) is a rule whose
	// body is a keyword sequence; join them into one space-separated word.
	vector<string> words;
	for (auto &child : chosen.Cast<ListParseResult>().GetChildren()) {
		words.push_back(StringUtil::Upper(child.get().Cast<KeywordParseResult>().keyword));
	}
	return StringUtil::Join(words, " ");
}

void SetObjectTarget(AlterPermissionsInfo &info, CatalogType type, QualifiedName name) {
	info.entry_catalog_type = type;
	if (type == CatalogType::DATABASE_ENTRY) {
		info.SetQualifiedName(Identifier(), Identifier(), name.Name());
	} else {
		info.SetQualifiedName(std::move(name));
	}
}

} // namespace

string PEGTransformerFactory::OwnerRoleName(PEGTransformer &transformer, ParseResult &spec) {
	auto &chosen = spec.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.type == ParseResultType::KEYWORD) {
		return StringUtil::Upper(chosen.Cast<KeywordParseResult>().keyword);
	}
	return transformer.Transform<string>(chosen);
}

void PEGTransformerFactory::CollectConfigClause(PEGTransformer &transformer, ParseResult &config,
                                                vector<string> &set_config, vector<string> &reset_config,
                                                bool &reset_all_config) {
	auto &clause = config.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<ListParseResult>();
	if (clause.Name() == "AlterRoleReset") {
		auto &target = clause.Child<ListParseResult>(1).Child<ChoiceParseResult>(0).GetResult();
		if (target.Name() == "AlterRoleResetAll") {
			reset_all_config = true;
		} else {
			reset_config.push_back(target.Cast<IdentifierParseResult>().identifier.GetIdentifierName());
		}
		return;
	}
	SettingInfo setting;
	setting.name = clause.Child<IdentifierParseResult>(1).identifier;
	const auto name = setting.name.GetIdentifierName();
	auto &value = clause.Child<ListParseResult>(2).Child<ChoiceParseResult>(0).GetResult();
	if (value.Name() == "AlterRoleSetFromCurrent") {
		set_config.push_back(name);
		return;
	}
	vector<unique_ptr<ParsedExpression>> values;
	for (auto &element : ExtractParseResultsFromList(value.Cast<ListParseResult>().GetChild(1))) {
		values.push_back(transformer.Transform<unique_ptr<ParsedExpression>>(element.get()));
	}
	if (values.size() == 1 && values[0]->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
		reset_config.push_back(name);
		return;
	}
	string text;
	if (setting.name == "search_path") {
		auto statement = TransformStandardAssignment(transformer, setting, std::move(values));
		auto &assigned = *statement->Cast<SetVariableStatement>().value;
		if (assigned.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
			throw ParserException("SET search_path: expected identifier or string literal");
		}
		text = CatalogSearchEntry::ListToString(CatalogSearchEntry::ParseList(ConfigValueText(assigned)));
	} else {
		for (auto &expr : values) {
			if (!text.empty()) {
				text += ", ";
			}
			text += ConfigValueText(*expr);
		}
	}
	set_config.push_back(name + "=" + text);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreateRoleStatement(PEGTransformer &transformer,
                                                                             ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'CREATE' 1:RoleOrUser 2:ColId 3:'WITH'? 4:RoleOptionList? 5:RoleMembershipClause*
	// A present 4 is a bare LIST(RoleOptionList); absent it is an empty OPTIONAL.
	// A present 5 is a REPEAT; absent it is an empty OPTIONAL.
	auto info = make_uniq<CreateRoleInfo>();
	info->SetName(Identifier(TransformColIdName(transformer, list_pr.GetChild(2))));

	// CREATE USER defaults to LOGIN; CREATE ROLE does not. An explicit
	// LOGIN/NOLOGIN option below overrides either way.
	auto &role_or_user =
	    list_pr.GetChild(1).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
	info->options = RoleOption::Inherit;
	if (StringUtil::Upper(role_or_user.keyword) == "USER") {
		info->options = info->options | RoleOption::Login;
	}

	// Attributes (RoleOption) and membership clauses (IN ROLE / ROLE / ADMIN) may
	// be freely interleaved, like PG. A repeated option category is an error.
	absl::flat_hash_set<std::string_view> seen;
	auto once = [&seen](std::string_view category) {
		if (!seen.insert(category).second) {
			throw ParserException("conflicting or redundant options");
		}
	};
	auto set_option = [&](ParseResult &node, std::string_view category, const char *positive, RoleOption option) {
		once(category);
		auto &kw = FirstKeyword(node);
		info->options = StringUtil::Upper(kw.keyword) == positive ? info->options | option : info->options & ~option;
	};
	auto collect_members = [&](ParseResult &clause, vector<Identifier> &target) {
		auto &cl = clause.Cast<ListParseResult>();
		auto &names = cl.GetChild(cl.GetChildren().size() - 1);
		for (auto &n : PEGTransformerFactory::ExtractParseResultsFromList(names)) {
			target.emplace_back(TransformColIdName(transformer, n.get()));
		}
	};

	auto &clauses_opt = list_pr.Child<OptionalParseResult>(4);
	std::span<reference<ParseResult>> clauses;
	if (clauses_opt.HasResult()) {
		clauses = clauses_opt.GetResult().Cast<RepeatParseResult>().GetChildren();
	}
	for (auto &clause_ref : clauses) {
		// CreateRoleClause -> CHOICE -> RoleOption|RoleMembershipClause, each of
		// which is itself a CHOICE wrapping the concrete option/clause.
		auto &wrapper = clause_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		auto &opt = wrapper.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		if (opt.Name() == "LoginOption") {
			set_option(opt, "login", "LOGIN", RoleOption::Login);
		} else if (opt.Name() == "SuperuserOption") {
			set_option(opt, "superuser", "SUPERUSER", RoleOption::Superuser);
		} else if (opt.Name() == "CreateDbOption") {
			set_option(opt, "createdb", "CREATEDB", RoleOption::CreateDb);
		} else if (opt.Name() == "CreateRoleOption") {
			set_option(opt, "createrole", "CREATEROLE", RoleOption::CreateRole);
		} else if (opt.Name() == "ReplicationOption") {
			set_option(opt, "replication", "REPLICATION", RoleOption::Replication);
		} else if (opt.Name() == "BypassRlsOption") {
			set_option(opt, "bypassrls", "BYPASSRLS", RoleOption::BypassRls);
		} else if (opt.Name() == "InheritOption") {
			set_option(opt, "inherit", "INHERIT", RoleOption::Inherit);
		} else if (opt.Name() == "ConnLimitOption") {
			once("connlimit");
			info->conn_limit = TransformConnLimit(opt);
		} else if (opt.Name() == "ValidUntilOption") {
			once("validuntil");
			info->valid_until = TransformValidUntil(opt);
		} else if (opt.Name() == "PasswordOption") {
			// PasswordOption <- 'ENCRYPTED'? 'PASSWORD' StringLiteral
			once("password");
			info->password = opt.Cast<ListParseResult>().GetChild(2).Cast<StringLiteralParseResult>().result;
		} else if (opt.Name() == "PasswordNullOption") {
			once("password");
		} else if (opt.Name() == "SysIdOption") {
			// SYSID is a legacy no-op accepted for compatibility.
		} else if (opt.Name() == "InRoleClause") {
			collect_members(opt, info->in_roles);
		} else if (opt.Name() == "RoleMembersClause") {
			collect_members(opt, info->role_members);
		} else if (opt.Name() == "AdminClause") {
			collect_members(opt, info->admin_members);
		} else {
			throw ParserException("Unexpected role option in CREATE ROLE: %s", opt.Name());
		}
	}

	auto result = make_uniq<CreateStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropRoleStatement(PEGTransformer &transformer,
                                                                           ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'DROP' 1:RoleOrUser 2:IfExists? 3:List(ColId)
	const auto if_not_found = list_pr.Child<OptionalParseResult>(2).HasResult() ? OnEntryNotFound::RETURN_NULL
	                                                                            : OnEntryNotFound::THROW_EXCEPTION;
	const auto drop = [&](ParseResult &name) {
		auto result = make_uniq<DropStatement>();
		result->info->type = CatalogType::ROLE_ENTRY;
		result->info->SetQualifiedName(Identifier(), Identifier(), Identifier(TransformColIdName(transformer, name)));
		result->info->if_not_found = if_not_found;
		return result;
	};
	auto names = PEGTransformerFactory::ExtractParseResultsFromList(list_pr.GetChild(3));
	if (names.size() == 1) {
		return drop(names[0].get());
	}
	auto result = make_uniq<MultiStatement>();
	for (auto &name : names) {
		result->statements.push_back(drop(name.get()));
	}
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformCreatePolicyStatement(PEGTransformer &transformer,
                                                                               ParseResult &parse_result) {
	throw NotImplementedException("row-level security is not supported");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterPolicyStatement(PEGTransformer &transformer,
                                                                              ParseResult &parse_result) {
	throw NotImplementedException("row-level security is not supported");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropPolicyStatement(PEGTransformer &transformer,
                                                                             ParseResult &parse_result) {
	throw NotImplementedException("row-level security is not supported");
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterTableRowSecurityStatement(PEGTransformer &transformer,
                                                                                        ParseResult &parse_result) {
	throw NotImplementedException("row-level security is not supported");
}

// ALTER ROLE/USER name ( RENAME TO new | <option>+ )
//   RENAME TO new  -> PRAGMA serenedb_rename_role('name', 'new')
//   <option>+      -> PRAGMA serenedb_alter_role('name', login, super, createdb,
//                            createrole, inherit, has_password, has_conn_limit,
//                            has_valid_until)
// Attribute flags are tri-state ints (-1 unspecified / 0 false / 1 true).
// PASSWORD / CONNECTION LIMIT / VALID UNTIL are parsed but unsupported; only a
// "was given" flag is forwarded so the command can reject them.
unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterRoleStatement(PEGTransformer &transformer,
                                                                            ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'ALTER' 1:RoleOrUser 2:ColId 3:(AlterRoleRename / AlterRoleConfig / AlterRoleOptionList)
	auto name = OwnerRoleName(transformer, list_pr.GetChild(2));
	auto &chosen = list_pr.GetChild(3).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();

	auto info = make_uniq<AlterRoleInfo>(Identifier(name));
	auto result = make_uniq<AlterStatement>();

	if (chosen.Name() == "AlterRoleRename") {
		// LIST(AlterRoleRename): 0:'RENAME' 1:'TO' 2:ColId
		info->new_name = Identifier(TransformColIdName(transformer, chosen.Cast<ListParseResult>().GetChild(2)));
		result->info = std::move(info);
		return std::move(result);
	}

	if (chosen.Name() == "AlterRoleConfig") {
		CollectConfigClause(transformer, chosen, info->set_config, info->reset_config, info->reset_all_config);
		result->info = std::move(info);
		return std::move(result);
	}

	absl::flat_hash_set<std::string_view> seen;
	auto once = [&seen](std::string_view category) {
		if (!seen.insert(category).second) {
			throw ParserException("conflicting or redundant options");
		}
	};
	// LIST(AlterRoleOptionList) -> 0:'WITH'? 1:the AlterRoleOption+ REPEAT.
	auto &repeat = chosen.Cast<ListParseResult>().Child<RepeatParseResult>(1);
	for (auto &opt_ref : repeat.GetChildren()) {
		auto &opt = opt_ref.get().Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
		if (opt.Name() == "PasswordOption") {
			// PasswordOption <- 'ENCRYPTED'? 'PASSWORD' StringLiteral
			once("password");
			info->set_password = true;
			info->password = opt.Cast<ListParseResult>().GetChild(2).Cast<StringLiteralParseResult>().result;
			continue;
		}
		if (opt.Name() == "PasswordNullOption") {
			once("password");
			info->set_password = true;
			info->null_password = true;
			continue;
		}
		if (opt.Name() == "ConnLimitOption") {
			once("connlimit");
			info->set_conn_limit = true;
			info->conn_limit = TransformConnLimit(opt);
			continue;
		}
		if (opt.Name() == "ValidUntilOption") {
			once("validuntil");
			info->set_valid_until = true;
			info->valid_until = TransformValidUntil(opt);
			continue;
		}
		// Every other option is a rule wrapping a CHOICE of two keywords (the
		// positive form and its NO* negation), like LoginOption.
		auto &kw = opt.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<KeywordParseResult>();
		const bool on = StringUtil::Upper(kw.keyword).rfind("NO", 0) != 0;
		once(opt.Name());
		RoleOption option;
		if (opt.Name() == "LoginOption") {
			option = RoleOption::Login;
		} else if (opt.Name() == "SuperuserOption") {
			option = RoleOption::Superuser;
		} else if (opt.Name() == "CreateDbOption") {
			option = RoleOption::CreateDb;
		} else if (opt.Name() == "CreateRoleOption") {
			option = RoleOption::CreateRole;
		} else if (opt.Name() == "ReplicationOption") {
			option = RoleOption::Replication;
		} else if (opt.Name() == "BypassRlsOption") {
			option = RoleOption::BypassRls;
		} else if (opt.Name() == "InheritOption") {
			option = RoleOption::Inherit;
		} else {
			throw ParserException("Unexpected role option in ALTER ROLE: %s", opt.Name());
		}
		if (on) {
			info->set_options = info->set_options | option;
		} else {
			info->clear_options = info->clear_options | option;
		}
	}

	result->info = std::move(info);
	return std::move(result);
}

// ALTER <objtype> name OWNER TO role
//   -> PRAGMA serenedb_alter_owner('objtype', 'name', 'new_owner')
unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterOwnerStatement(PEGTransformer &transformer,
                                                                             ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	auto &target =
	    list_pr.GetChild(1).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult().Cast<ListParseResult>();
	auto info = make_uniq<AlterPermissionsInfo>(CatalogType::INVALID, QualifiedName());
	if (target.Name() == "OwnerRoutineTarget") {
		SetObjectTarget(*info, CatalogType::MACRO_ENTRY,
		                transformer.Transform<QualifiedName>(target.Child<ListParseResult>(1).GetChild(0)));
	} else {
		SetObjectTarget(*info, GrantObjectType(ObjectTypeKeyword(target.GetChild(0))),
		                transformer.Transform<QualifiedName>(target.GetChild(1)));
	}

	info->new_owner = OwnerRoleName(transformer, list_pr.GetChild(4));

	auto result = make_uniq<AlterStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformReassignOwnedStatement(PEGTransformer &transformer,
                                                                                ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'REASSIGN' 1:'OWNED' 2:'BY' 3:List(OwnerRoleSpec) 4:'TO' 5:OwnerRoleSpec
	auto info = make_uniq<ReassignOwnedInfo>();
	for (auto &role : ExtractParseResultsFromList(list_pr.GetChild(3))) {
		info->roles.push_back(OwnerRoleName(transformer, role.get()));
	}
	info->new_owner = OwnerRoleName(transformer, list_pr.GetChild(5));
	auto result = make_uniq<AlterStatement>();
	result->info = std::move(info);
	return std::move(result);
}

unique_ptr<SQLStatement> PEGTransformerFactory::TransformDropOwnedStatement(PEGTransformer &transformer,
                                                                            ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'DROP' 1:'OWNED' 2:'BY' 3:List(OwnerRoleSpec) 4:DropBehavior?
	auto info = make_uniq<DropOwnedInfo>();
	for (auto &role : ExtractParseResultsFromList(list_pr.GetChild(3))) {
		info->roles.push_back(OwnerRoleName(transformer, role.get()));
	}
	transformer.TransformOptional<bool>(list_pr, 4, info->cascade);
	auto result = make_uniq<AlterStatement>();
	result->info = std::move(info);
	return std::move(result);
}

static void TransformPrivilegeList(PEGTransformer &transformer, ParseResult &privs_list, AlterPermissionsInfo &info) {
	const auto acl_class =
	    info.default_objtype != CatalogType::INVALID ? info.default_objtype : info.entry_catalog_type;
	auto add = [&](const string &keyword, optional_ptr<ParseResult> columns) {
		AclMode mode;
		if (!Permissions::TryParsePrivilege(keyword, acl_class, mode)) {
			throw ParserException("invalid privilege type %s", keyword);
		}
		if (!columns) {
			info.privileges |= mode;
			return;
		}
		if (info.default_objtype != CatalogType::INVALID) {
			throw ParserException("default privileges cannot be set for columns");
		}
		if (keyword != "ALL" && (mode & ~ACL_COLUMN_PRIVILEGES) != AclMode::NoRights) {
			throw ParserException("invalid privilege type %s for column", keyword);
		}
		auto &inner = PEGTransformerFactory::ExtractResultFromParens(columns->Cast<ListParseResult>().GetChild(0));
		for (auto &col_ref : PEGTransformerFactory::ExtractParseResultsFromList(inner)) {
			Identifier column(transformer.Transform<string>(col_ref.get()));
			auto existing = std::find_if(info.column_privileges.begin(), info.column_privileges.end(),
			                             [&](const ColumnPrivilege &entry) { return entry.column == column; });
			if (existing == info.column_privileges.end()) {
				info.column_privileges.push_back(ColumnPrivilege {std::move(column), mode});
			} else {
				existing->privileges |= mode;
			}
		}
	};
	auto &chosen = privs_list.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	if (chosen.Name() == "AllPrivileges") {
		auto &columns = chosen.Cast<ListParseResult>().Child<OptionalParseResult>(2);
		add("ALL", columns.HasResult() ? &columns.GetResult() : nullptr);
		return;
	}
	for (auto &elem_ref : PEGTransformerFactory::ExtractParseResultsFromList(chosen)) {
		auto &priv_list = elem_ref.get().Cast<ListParseResult>();
		auto &priv = priv_list.Child<ListParseResult>(0).Child<ChoiceParseResult>(0).GetResult();
		auto &columns = priv_list.Child<OptionalParseResult>(1);
		add(StringUtil::Upper(priv.Cast<KeywordParseResult>().keyword),
		    columns.HasResult() ? &columns.GetResult() : nullptr);
	}
}

static vector<QualifiedName> TransformGrantTarget(PEGTransformer &transformer, ParseResult &target_choice_holder,
                                                  AlterPermissionsInfo &info) {
	auto &chosen = target_choice_holder.Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	auto &target = chosen.Cast<ListParseResult>();
	vector<QualifiedName> names;
	if (chosen.Name() == "GrantAllInSchema") {
		auto kind = StringUtil::Upper(target.GetChild(1)
		                                  .Cast<ListParseResult>()
		                                  .Child<ChoiceParseResult>(0)
		                                  .GetResult()
		                                  .Cast<KeywordParseResult>()
		                                  .keyword);
		if (kind == "TABLES") {
			info.entry_catalog_type = CatalogType::TABLE_ENTRY;
		} else if (kind == "SEQUENCES") {
			info.entry_catalog_type = CatalogType::SEQUENCE_ENTRY;
		} else {
			info.entry_catalog_type = CatalogType::MACRO_ENTRY;
			info.include_functions = kind != "PROCEDURES";
			info.include_procedures = kind != "FUNCTIONS";
		}
		info.all_in_schema = true;
		for (auto &schema_ref : PEGTransformerFactory::ExtractParseResultsFromList(target.GetChild(4))) {
			auto schema = transformer.Transform<QualifiedName>(schema_ref.get());
			names.emplace_back(schema.Schema(), schema.Name(), Identifier());
		}
		return names;
	}
	if (chosen.Name() == "GrantFunctionTarget") {
		// 0:GrantRoutineKind 1:List(RoutineSignature)
		info.entry_catalog_type = CatalogType::MACRO_ENTRY;
		for (auto &routine : PEGTransformerFactory::ExtractParseResultsFromList(target.GetChild(1))) {
			names.push_back(transformer.Transform<QualifiedName>(routine.get().Cast<ListParseResult>().GetChild(0)));
		}
		return names;
	}
	// GrantNamedTarget: 0:GrantObjType? 1:List(QualifiedName)
	auto &objtype_opt = target.Child<OptionalParseResult>(0);
	info.entry_catalog_type =
	    GrantObjectType(objtype_opt.HasResult() ? ObjectTypeKeyword(objtype_opt.GetResult()) : "TABLE");
	for (auto &name : PEGTransformerFactory::ExtractParseResultsFromList(target.GetChild(1))) {
		names.push_back(transformer.Transform<QualifiedName>(name.get()));
	}
	return names;
}

static unique_ptr<SQLStatement> SingleOrMulti(unique_ptr<MultiStatement> statements) {
	if (statements->statements.size() == 1) {
		return std::move(statements->statements[0]);
	}
	return std::move(statements);
}

// Children from `privs_child`: 0:PrivilegeList 1:'ON' 2:GrantTarget 3:TO/FROM
// 4:List(Grantee) 5:WithGrantOption?/DropBehavior? 6:GrantedBy?
static unique_ptr<SQLStatement> BuildGrantTable(PEGTransformer &transformer, ListParseResult &list_pr, bool revoke,
                                                idx_t privs_child, bool option_only) {
	AlterPermissionsInfo info(CatalogType::INVALID, QualifiedName());
	auto targets = TransformGrantTarget(transformer, list_pr.GetChild(privs_child + 2), info);
	TransformPrivilegeList(transformer, list_pr.GetChild(privs_child), info);
	info.revoke = revoke;
	info.option_only = option_only;

	// GRANT: WithGrantOption? at +5, GrantedBy? at +6.
	// REVOKE table: GrantedBy? at +5, DropBehavior? at +6.
	if (revoke) {
		info.granted_by = TransformGrantedBy(transformer, list_pr, privs_child + 5);
		info.cascade = TransformCascade(list_pr, privs_child + 6);
	} else {
		info.with_grant_option = list_pr.Child<OptionalParseResult>(privs_child + 5).HasResult();
		info.granted_by = TransformGrantedBy(transformer, list_pr, privs_child + 6);
	}

	auto statements = make_uniq<MultiStatement>();
	for (auto &target : targets) {
		for (auto &grantee : PEGTransformerFactory::ExtractParseResultsFromList(list_pr.GetChild(privs_child + 4))) {
			auto alter = info.Copy();
			auto &permissions = alter->Cast<AlterPermissionsInfo>();
			SetObjectTarget(permissions, info.entry_catalog_type, target);
			permissions.grantee = TransformGrantee(transformer, grantee.get());
			PEGTransformerFactory::AddToMultiStatement(statements, std::move(alter));
		}
	}
	return SingleOrMulti(std::move(statements));
}

static unique_ptr<SQLStatement> BuildGrantRole(PEGTransformer &transformer, ListParseResult &list_pr, idx_t roles_child,
                                               bool revoke, const string &option) {
	AlterRoleInfo info {Identifier()};
	info.revoke = revoke;
	info.option_only = !option.empty();
	if (option == "ADMIN") {
		info.admin_option = 0;
	} else if (option == "INHERIT") {
		info.inherit_option = 0;
	} else if (option == "SET") {
		info.set_option = 0;
	}
	if (!revoke) {
		auto &opts_opt = list_pr.Child<OptionalParseResult>(roles_child + 3);
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
				if (chosen.Name() == "AdminOption") {
					info.admin_option = NumericCast<int8_t>(v);
				} else if (chosen.Name() == "InheritMemberOption") {
					info.inherit_option = NumericCast<int8_t>(v);
				} else {
					info.set_option = NumericCast<int8_t>(v);
				}
			}
		}
	}

	auto statements = make_uniq<MultiStatement>();
	for (auto &role : PEGTransformerFactory::ExtractParseResultsFromList(list_pr.GetChild(roles_child))) {
		for (auto &member : PEGTransformerFactory::ExtractParseResultsFromList(list_pr.GetChild(roles_child + 2))) {
			auto alter = info.Copy();
			auto &membership = alter->Cast<AlterRoleInfo>();
			membership.SetQualifiedName(Identifier(), Identifier(),
			                            Identifier(PEGTransformerFactory::OwnerRoleName(transformer, member.get())));
			membership.grant_role = TransformColIdName(transformer, role.get());
			PEGTransformerFactory::AddToMultiStatement(statements, std::move(alter));
		}
	}
	return SingleOrMulti(std::move(statements));
}

// Shared body for GRANT/REVOKE. `revoke` selects the direction; `inner` is the
// choice between the table-privilege, role-membership, and (REVOKE-only)
// OPTION-FOR downgrade forms.
static unique_ptr<SQLStatement> BuildGrant(PEGTransformer &transformer, ChoiceParseResult &inner, bool revoke) {
	auto &form = inner.GetResult();

	// GrantTablePrivilege / RevokeTablePrivilege: PrivilegeList at child 0.
	if (form.Name() == "GrantTablePrivilege" || form.Name() == "RevokeTablePrivilege") {
		return BuildGrantTable(transformer, form.Cast<ListParseResult>(), revoke, /*privs_child=*/0,
		                       /*option_only=*/false);
	}
	// REVOKE GRANT OPTION FOR <priv> ... : 0:'GRANT' 1:'OPTION' 2:'FOR', then
	// the same shape as RevokeTablePrivilege starting at child 3.
	if (form.Name() == "RevokeGrantOptionFor") {
		return BuildGrantTable(transformer, form.Cast<ListParseResult>(), /*revoke=*/true, /*privs_child=*/3,
		                       /*option_only=*/true);
	}
	if (form.Name() == "RevokeAdminOptionFor") {
		auto &option = form.Cast<ListParseResult>().Child<ListParseResult>(0).Child<ChoiceParseResult>(0).GetResult();
		return BuildGrantRole(transformer, form.Cast<ListParseResult>(), /*roles_child=*/3, /*revoke=*/true,
		                      StringUtil::Upper(option.Cast<KeywordParseResult>().keyword));
	}

	// GrantRoleMembership / RevokeRoleMembership: 0:roles 2:members, options at 3.
	return BuildGrantRole(transformer, form.Cast<ListParseResult>(), /*roles_child=*/0, revoke, string());
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

unique_ptr<SQLStatement> PEGTransformerFactory::TransformAlterDefaultPrivilegesStatement(PEGTransformer &transformer,
                                                                                         ParseResult &parse_result) {
	auto &list_pr = parse_result.Cast<ListParseResult>();
	// 0:'ALTER' 1:'DEFAULT' 2:'PRIVILEGES' 3:DefaultPrivForRole? 4:DefaultPrivInSchema?
	// 5:DefaultPrivAction
	AlterPermissionsInfo info(CatalogType::DATABASE_ENTRY, QualifiedName());
	vector<string> for_roles {string()};
	if (auto &for_opt = list_pr.Child<OptionalParseResult>(3); for_opt.HasResult()) {
		// LIST(DefaultPrivForRole): 0:'FOR' 1:RoleOrUser 2:List(ColId)
		for_roles.clear();
		for (auto &role : PEGTransformerFactory::ExtractParseResultsFromList(
		         for_opt.GetResult().Cast<ListParseResult>().GetChild(2))) {
			for_roles.push_back(OwnerRoleName(transformer, role.get()));
		}
	}
	vector<string> schemas {string()};
	if (auto &schema_opt = list_pr.Child<OptionalParseResult>(4); schema_opt.HasResult()) {
		// LIST(DefaultPrivInSchema): 0:'IN' 1:'SCHEMA' 2:List(QualifiedName)
		schemas.clear();
		for (auto &schema : PEGTransformerFactory::ExtractParseResultsFromList(
		         schema_opt.GetResult().Cast<ListParseResult>().GetChild(2))) {
			schemas.push_back(QualifiedTableName(transformer, schema.get()));
		}
	}

	auto &action = list_pr.GetChild(5).Cast<ListParseResult>().Child<ChoiceParseResult>(0).GetResult();
	info.revoke = action.Name() == "DefaultPrivRevoke";
	auto &g = action.Cast<ListParseResult>();
	const auto objtype_of = [&](idx_t child) {
		auto &kw = g.GetChild(child)
		               .Cast<ListParseResult>()
		               .Child<ChoiceParseResult>(0)
		               .GetResult()
		               .Cast<KeywordParseResult>();
		return GrantObjectType(StringUtil::Upper(kw.keyword));
	};
	idx_t grantees_child;
	if (!info.revoke) {
		// DefaultPrivGrant: 0:'GRANT' 1:PrivilegeList 2:'ON' 3:DefaultPrivObjType
		// 4:'TO' 5:List(Grantee) 6:WithGrantOption?
		info.default_objtype = objtype_of(3);
		TransformPrivilegeList(transformer, g.GetChild(1), info);
		grantees_child = 5;
		info.with_grant_option = g.Child<OptionalParseResult>(6).HasResult();
	} else {
		// DefaultPrivRevoke: 0:'REVOKE' 1:DefaultPrivRevokeGrantOpt? 2:PrivilegeList
		// 3:'ON' 4:DefaultPrivObjType 5:'FROM' 6:List(Grantee) 7:DropBehavior?
		info.option_only = g.Child<OptionalParseResult>(1).HasResult();
		info.default_objtype = objtype_of(4);
		TransformPrivilegeList(transformer, g.GetChild(2), info);
		grantees_child = 6;
		info.cascade = TransformCascade(g, 7);
	}

	auto statements = make_uniq<MultiStatement>();
	for (auto &for_role : for_roles) {
		for (auto &schema : schemas) {
			for (auto &grantee : PEGTransformerFactory::ExtractParseResultsFromList(g.GetChild(grantees_child))) {
				auto alter = info.Copy();
				auto &defaults = alter->Cast<AlterPermissionsInfo>();
				defaults.for_role = for_role;
				defaults.default_schema = schema;
				defaults.grantee = TransformGrantee(transformer, grantee.get());
				PEGTransformerFactory::AddToMultiStatement(statements, std::move(alter));
			}
		}
	}
	return SingleOrMulti(std::move(statements));
}

} // namespace duckdb
