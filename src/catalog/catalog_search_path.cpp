#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/default/default_schemas.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

#include "duckdb/common/exception/parser_exception.hpp"

namespace duckdb {

CatalogSearchEntry::CatalogSearchEntry(Identifier catalog_p, Identifier schema_p)
    : catalog(std::move(catalog_p)), schema(std::move(schema_p)) {
}

string CatalogSearchEntry::ToString() const {
	if (catalog.empty()) {
		return WriteOptionallyQuoted(schema);
	} else {
		return WriteOptionallyQuoted(catalog) + "." + WriteOptionallyQuoted(schema);
	}
}

string CatalogSearchEntry::WriteOptionallyQuoted(const Identifier &input_p) {
	auto &input = input_p.GetIdentifierName();
	// Postgres writes an entry unquoted only if it is a run of digits (a numeric SET value) or a
	// lower-case identifier [a-z_][a-z0-9_]* - uppercase, `$user`, `.`, `,`, `"`, spaces are quoted.
	bool all_digits = !input.empty();
	bool lower_identifier = !input.empty();
	for (idx_t i = 0; i < input.size(); i++) {
		const char c = input[i];
		const bool is_digit = c >= '0' && c <= '9';
		all_digits = all_digits && is_digit;
		lower_identifier = lower_identifier && ((c >= 'a' && c <= 'z') || c == '_' || (is_digit && i > 0));
	}
	if (!all_digits && !lower_identifier) {
		return "\"" + StringUtil::Replace(input, "\"", "\"\"") + "\"";
	}
	return input;
}

string CatalogSearchEntry::ListToString(const vector<CatalogSearchEntry> &input) {
	string result;
	for (auto &entry : input) {
		if (!result.empty()) {
			result += ", ";
		}
		result += entry.ToString();
	}
	return result;
}

CatalogSearchEntry CatalogSearchEntry::ParseInternal(const string &input, idx_t &idx) {
	string catalog;
	string schema;
	string entry;
	bool finished = false;
normal:
	// skip whitespace in front of an entry (e.g. after a comma in a list)
	while (idx < input.size() && entry.empty() && StringUtil::CharacterIsSpace(input[idx])) {
		idx++;
	}
	for (; idx < input.size(); idx++) {
		if (input[idx] == '"') {
			idx++;
			goto quoted;
		} else if (input[idx] == '.') {
			goto separator;
		} else if (input[idx] == ',') {
			finished = true;
			goto separator;
		}
		entry += input[idx];
	}
	finished = true;
	goto separator;
quoted:
	//! look for another quote
	for (; idx < input.size(); idx++) {
		if (input[idx] == '"') {
			//! unquote
			idx++;
			if (idx < input.size() && input[idx] == '"') {
				// escaped quote
				entry += input[idx];
				continue;
			}
			goto normal;
		}
		entry += input[idx];
	}
	throw ParserException("Unterminated quote in qualified name!");
separator:
	// trim whitespace between the entry and its separator
	while (!entry.empty() && StringUtil::CharacterIsSpace(entry.back())) {
		entry.pop_back();
	}
	if (entry.empty()) {
		throw ParserException("Unexpected dot - empty CatalogSearchEntry");
	}
	if (schema.empty()) {
		// if we parse one entry it is the schema
		schema = std::move(entry);
	} else if (catalog.empty()) {
		// if we parse two entries it is [catalog.schema]
		catalog = std::move(schema);
		schema = std::move(entry);
	} else {
		throw ParserException("Too many dots - expected [schema] or [catalog.schema] for CatalogSearchEntry");
	}
	entry = "";
	idx++;
	if (finished) {
		goto final;
	}
	goto normal;
final:
	if (schema.empty()) {
		throw ParserException("Unexpected end of entry - empty CatalogSearchEntry");
	}
	return CatalogSearchEntry(Identifier(std::move(catalog)), Identifier(std::move(schema)));
}

CatalogSearchEntry CatalogSearchEntry::Parse(const string &input) {
	idx_t pos = 0;
	auto result = ParseInternal(input, pos);
	if (pos < input.size()) {
		throw ParserException("Failed to convert entry \"%s\" to CatalogSearchEntry - expected a single entry", input);
	}
	return result;
}

vector<CatalogSearchEntry> CatalogSearchEntry::ParseList(const string &input) {
	idx_t pos = 0;
	vector<CatalogSearchEntry> result;
	while (pos < input.size()) {
		auto entry = ParseInternal(input, pos);
		result.push_back(entry);
	}
	return result;
}

CatalogSearchPath::CatalogSearchPath(ClientContext &context_p, vector<CatalogSearchEntry> entries)
    : context(context_p) {
	SetPathsInternal(std::move(entries));
}

CatalogSearchPath::CatalogSearchPath(ClientContext &context_p) : CatalogSearchPath(context_p, {}) {
}

void CatalogSearchPath::Reset() {
	SetPathsInternal(default_paths);
}

void CatalogSearchPath::SetDefaultPaths(vector<CatalogSearchEntry> new_defaults) {
	for (auto &entry : new_defaults) {
		if (entry.GetCatalog().empty() || entry.GetSchema().empty()) {
			throw InternalException("SetDefaultPaths requires fully qualified entries");
		}
	}
	default_paths = std::move(new_defaults);
}

void CatalogSearchPath::RefreshSetPaths() {
	SetPathsInternal(set_paths);
}

string CatalogSearchPath::GetSetName(CatalogSetPathType set_type) {
	switch (set_type) {
	case CatalogSetPathType::SET_SCHEMA:
		return "SET schema";
	case CatalogSetPathType::SET_SCHEMAS:
		return "SET search_path";
	default:
		throw InternalException("Unrecognized CatalogSetPathType");
	}
}

void CatalogSearchPath::Set(vector<CatalogSearchEntry> new_paths, CatalogSetPathType set_type) {
	if (set_type == CatalogSetPathType::SET_SCHEMA && new_paths.size() != 1) {
		throw CatalogException("%s can set only 1 schema. This has %d", GetSetName(set_type), new_paths.size());
	}
	for (auto &path : new_paths) {
		if (set_type == CatalogSetPathType::SET_DIRECTLY) {
			if (path.GetCatalog().empty() || path.GetSchema().empty()) {
				throw InternalException("SET_WITHOUT_VERIFICATION requires a fully qualified set path");
			}
			continue;
		}
		auto schema_entry =
		    Catalog::GetSchema(context, path.GetCatalog(), path.GetSchema(), OnEntryNotFound::RETURN_NULL);
		if (schema_entry) {
			// we are setting a schema - update the catalog and schema
			if (path.GetCatalog().empty()) {
				path.SetCatalog(GetDefault().GetCatalog());
			}
			continue;
		}
		// only schema supplied - check if this is a catalog instead
		if (path.GetCatalog().empty()) {
			auto catalog = Catalog::GetCatalogEntry(context, path.GetSchema());
			if (catalog) {
				auto schema =
				    catalog->GetSchema(context, Identifier(catalog->GetDefaultSchema()), OnEntryNotFound::RETURN_NULL);
				if (schema) {
					path.SetCatalog(path.GetSchema());
					path.SetSchema(schema->name);
					continue;
				}
			}
			// unknown schemas are accepted silently (as in Postgres) - lookups skip the missing entry.
			// SET schema / USE name a single target, which Postgres has no equivalent of, so those keep
			// duckdb's existence check
			if (set_type == CatalogSetPathType::SET_SCHEMA) {
				throw CatalogException("%s: No catalog + schema named \"%s\" found.", GetSetName(set_type),
				                       path.ToString());
			}
			path.SetCatalog(GetDefault().GetCatalog());
			continue;
		}
		// for an explicit catalog.schema only an unknown catalog is an error
		if (set_type != CatalogSetPathType::SET_SCHEMA && Catalog::GetCatalogEntry(context, path.GetCatalog())) {
			continue;
		}
		throw CatalogException("%s: No catalog + schema named \"%s\" found.", GetSetName(set_type), path.ToString());
	}
	if (set_type == CatalogSetPathType::SET_SCHEMA) {
		if (new_paths[0].GetCatalog() == TEMP_CATALOG || new_paths[0].GetCatalog() == SYSTEM_CATALOG) {
			throw CatalogException("%s cannot be set to internal schema \"%s\"", GetSetName(set_type),
			                       new_paths[0].GetCatalog().GetIdentifierName());
		}
	}
	SetPathsInternal(std::move(new_paths));
}

void CatalogSearchPath::Set(CatalogSearchEntry new_value, CatalogSetPathType set_type) {
	vector<CatalogSearchEntry> new_paths {std::move(new_value)};
	Set(std::move(new_paths), set_type);
}

// Resolves the "$user" placeholder to the session user. Returns empty when the entry has no schema,
// or when it is "$user" and no session user is set - callers skip such entries.
static string ResolveSchema(ClientContext &context, const CatalogSearchEntry &entry) {
	if (entry.GetSchema().empty()) {
		return {};
	}
	if (entry.GetSchema() == "$user") {
		return context.session_user;
	}
	return entry.GetSchema().GetIdentifierName();
}

static vector<CatalogSearchEntry> ResolveEntries(ClientContext &context, const vector<CatalogSearchEntry> &entries) {
	vector<CatalogSearchEntry> res;
	res.reserve(entries.size());
	for (auto &path : entries) {
		auto resolved = ResolveSchema(context, path);
		if (resolved.empty()) {
			continue;
		}
		res.emplace_back(path.GetCatalog(), Identifier(std::move(resolved)));
	}
	return res;
}

vector<CatalogSearchEntry> CatalogSearchPath::Get() const {
	return ResolveEntries(context, paths);
}

vector<CatalogSearchEntry> CatalogSearchPath::GetResolvedSetPaths() const {
	return ResolveEntries(context, set_paths);
}

Identifier CatalogSearchPath::GetDefaultSchema(const Identifier &catalog) const {
	return GetDefaultSchema(context, catalog);
}

Identifier CatalogSearchPath::GetDefaultSchema(ClientContext &context_p, const Identifier &catalog) const {
	for (auto &path : paths) {
		if (path.GetCatalog() == TEMP_CATALOG) {
			continue;
		}
		if (path.GetCatalog() == catalog) {
			auto resolved = ResolveSchema(context_p, path);
			if (resolved.empty()) {
				continue;
			}
			return Identifier(std::move(resolved));
		}
	}
	auto catalog_entry = Catalog::GetCatalogEntry(context_p, catalog);
	if (catalog_entry) {
		return Identifier(catalog_entry->GetDefaultSchema());
	}
	return DEFAULT_SCHEMA;
}

Identifier CatalogSearchPath::GetDefaultCatalog(const Identifier &schema) const {
	if (DefaultSchemaGenerator::IsDefaultSchema(schema)) {
		return GetCatalogsForSchema(schema).front();
	}
	for (auto &path : paths) {
		if (path.GetCatalog() == TEMP_CATALOG) {
			continue;
		}
		auto resolved = ResolveSchema(context, path);
		if (resolved.empty()) {
			continue;
		}
		if (schema == resolved) {
			return path.GetCatalog();
		}
	}
	return Identifier::InvalidCatalog();
}

vector<Identifier> CatalogSearchPath::GetCatalogsForSchema(const Identifier &schema) const {
	vector<Identifier> catalogs;
	if (DefaultSchemaGenerator::IsDefaultSchema(schema)) {
		// an attached catalog can serve a default schema itself - it takes precedence over the system
		// catalog, whose version stays reachable as system.<schema>
		for (auto &path : paths) {
			if (path.GetCatalog() == TEMP_CATALOG || path.GetCatalog() == SYSTEM_CATALOG || path.GetCatalog().empty()) {
				continue;
			}
			catalogs.emplace_back(path.GetCatalog());
		}
		catalogs.push_back(Identifier::SystemCatalog());
	} else {
		catalogs.reserve(paths.size());
		for (auto &path : paths) {
			if (path.GetSchema().empty()) {
				catalogs.push_back(path.GetCatalog());
				continue;
			}
			auto resolved = ResolveSchema(context, path);
			if (resolved.empty()) {
				continue;
			}
			if (schema == resolved) {
				catalogs.push_back(path.GetCatalog());
			}
		}
	}
	return catalogs;
}

vector<Identifier> CatalogSearchPath::GetSchemasForCatalog(const Identifier &catalog) const {
	vector<Identifier> schemas;
	schemas.reserve(paths.size());
	for (auto &path : paths) {
		if (path.GetCatalog() != catalog) {
			continue;
		}
		auto resolved = ResolveSchema(context, path);
		if (resolved.empty()) {
			continue;
		}
		schemas.emplace_back(std::move(resolved));
	}
	return schemas;
}

const CatalogSearchEntry &CatalogSearchPath::GetDefault() const {
	D_ASSERT(paths.size() >= 2);
	return paths[1];
}

CatalogSearchEntry CatalogSearchPath::GetResolvedDefault() const {
	// no user-set entries -> no default schema (in PG current_schema is NULL and a CREATE without a
	// schema prefix errors out with "no schema has been selected")
	if (set_paths.empty()) {
		return CatalogSearchEntry(Identifier(), Identifier());
	}
	// PG falls through to the next search_path entry when "$user" does not name an existing schema
	for (auto &path : set_paths) {
		auto resolved = ResolveSchema(context, path);
		if (resolved.empty()) {
			continue;
		}
		if (Catalog::GetSchema(context, path.GetCatalog(), Identifier(resolved), OnEntryNotFound::RETURN_NULL)) {
			return CatalogSearchEntry(path.GetCatalog(), Identifier(std::move(resolved)));
		}
	}
	return GetDefault();
}

void CatalogSearchPath::SetPathsInternal(vector<CatalogSearchEntry> new_paths) {
	this->set_paths = std::move(new_paths);

	paths.clear();
	paths.reserve(set_paths.size() + 5);
	paths.emplace_back(TEMP_CATALOG, DEFAULT_SCHEMA);
	for (auto &path : set_paths) {
		paths.push_back(path);
	}
	paths.emplace_back(INVALID_CATALOG, DEFAULT_SCHEMA);
	paths.emplace_back(SYSTEM_CATALOG, DEFAULT_SCHEMA);
	paths.emplace_back(INVALID_CATALOG, "pg_catalog");
	paths.emplace_back(SYSTEM_CATALOG, "pg_catalog");
	// set extension schemas on the search path, if any
	for (auto &schema : ExtensionCallbackManager::Get(context).GetExtensionSchemas()) {
		paths.emplace_back(Identifier(SYSTEM_CATALOG), Identifier(schema));
	}
}

bool CatalogSearchPath::SchemaInSearchPath(ClientContext &context, const Identifier &catalog_name,
                                           const Identifier &schema_name) const {
	for (auto &path : paths) {
		auto resolved = ResolveSchema(context, path);
		if (resolved.empty() || schema_name != resolved) {
			continue;
		}
		bool catalog_matches =
		    path.GetCatalog() == catalog_name ||
		    (IsInvalidCatalog(path.GetCatalog()) && catalog_name == DatabaseManager::GetDefaultDatabase(context));
		if (!catalog_matches) {
			continue;
		}
		// entries accepted for a schema that does not exist are ignored at lookup - they must not
		// report as being in the path either
		if (Catalog::GetSchema(context, catalog_name, schema_name, OnEntryNotFound::RETURN_NULL)) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
