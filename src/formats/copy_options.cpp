#include "duckdb/formats/copy_options.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "fast_float/fast_float.h"

namespace duckdb {

namespace {

constexpr const char *SQLSTATE_FEATURE_NOT_SUPPORTED = "0A000";
constexpr const char *SQLSTATE_SYNTAX_ERROR = "42601";
constexpr const char *SQLSTATE_INVALID_PARAMETER_VALUE = "22023";
constexpr const char *SQLSTATE_INVALID_TEXT_REPRESENTATION = "22P02";
constexpr const char *SQLSTATE_INVALID_COLUMN_REFERENCE = "42P10";

constexpr std::string_view TEXT_RESERVED_DELIMITERS = "\\.abcdefghijklmnopqrstuvwxyz0123456789";

constexpr std::string_view COPY_FORMAT_OPTIONS[] = {
    "format",      "delimiter",     "delim",   "sep",           "separator",      "null",
    "nullstr",     "header",        "default", "ignore_errors", "rejects_limit",  "reject_limit",
    "compression", "quote",         "escape",  "force_quote",   "force_not_null", "force_null",
    "on_error",    "log_verbosity", "freeze",  "encoding",      "rendering",
};

constexpr std::string_view UNSUPPORTED_POSTGRES_ENCODINGS[] = {
    "big5",         "euccn",        "eucjis2004", "eucjp",    "euckr",   "euctw",   "gb18030", "gbk",
    "iso88595",     "iso88596",     "iso88597",   "iso88598", "johab",   "koi8r",   "koi8u",   "latin2",
    "latin3",       "latin4",       "latin5",     "latin6",   "latin7",  "latin8",  "latin9",  "latin10",
    "muleinternal", "shiftjis2004", "sjis",       "uhc",      "win1250", "win1251", "win1252", "win1253",
    "win1254",      "win1255",      "win1256",    "win1257",  "win1258", "win866",  "win874",
};

[[noreturn]] void ThrowRequiresCsv(const string &option) {
	ThrowCopyError(SQLSTATE_FEATURE_NOT_SUPPORTED, "COPY " + option + " requires CSV mode");
}

[[noreturn]] void ThrowNotRecognized(const string &option) {
	ThrowCopyError(SQLSTATE_SYNTAX_ERROR, "COPY option \"" + option + "\" not recognized");
}

[[noreturn]] void ThrowInvalid(const string &message) {
	ThrowCopyError(SQLSTATE_INVALID_PARAMETER_VALUE, message);
}

string OptionText(const vector<Value> &values) {
	return values.empty() ? string() : values[0].ToString();
}

char OptionChar(const vector<Value> &values, const string &what) {
	const auto text = OptionText(values);
	if (text.size() != 1) {
		ThrowCopyError(SQLSTATE_FEATURE_NOT_SUPPORTED, "COPY " + what + " must be a single one-byte character");
	}
	return text[0];
}

idx_t ParseRejectLimit(const vector<Value> &values) {
	if (values.empty()) {
		ThrowCopyError(SQLSTATE_SYNTAX_ERROR, "reject_limit requires a numeric value");
	}
	const auto text = OptionText(values);
	int64_t limit = 0;
	const auto end = text.data() + text.size();
	const auto parsed = duckdb_fast_float::from_chars(text.data(), end, limit);
	if (parsed.ec != std::errc() || parsed.ptr != end) {
		ThrowCopyError(SQLSTATE_INVALID_TEXT_REPRESENTATION, "invalid input syntax for type bigint: \"" + text + "\"");
	}
	if (limit <= 0) {
		ThrowInvalid("REJECT_LIMIT (" + std::to_string(limit) + ") must be greater than zero");
	}
	return static_cast<idx_t>(limit);
}

bool ParseBoolOption(const vector<Value> &values, const string &option) {
	if (values.empty()) {
		return true;
	}
	bool result;
	if (!ParsePostgresBool(OptionText(values), result)) {
		ThrowInvalid(option + " requires a Boolean value");
	}
	return result;
}

void ParseHeader(CopyFormatOptions &out, const vector<Value> &values) {
	if (values.empty()) {
		out.header = true;
		return;
	}
	const auto text = OptionText(values);
	if (StringUtil::CIEquals(text, "match")) {
		out.header = true;
		out.header_match = true;
		return;
	}
	if (!ParsePostgresBool(text, out.header)) {
		ThrowInvalid("header requires a Boolean value or \"match\"");
	}
}

CopyColumns ParseColumns(const vector<Value> &values, const string &option) {
	if (values.empty()) {
		ThrowCopyError(SQLSTATE_SYNTAX_ERROR, option + " expects a column list or * as parameter");
	}
	CopyColumns columns;
	for (auto &value : values) {
		if (value.type().id() == LogicalTypeId::LIST) {
			for (auto &child : ListValue::GetChildren(value)) {
				columns.names.push_back(child.ToString());
			}
			continue;
		}
		auto name = value.ToString();
		if (name == "*") {
			columns.all = true;
			continue;
		}
		columns.names.push_back(std::move(name));
	}
	return columns;
}

bool IsDuckDBDefault(const Identifier &key, const vector<Value> &values) {
	if (values.size() != 1) {
		return false;
	}
	const auto text = values[0].ToString();
	bool parsed;
	if (key == "allow_quoted_nulls") {
		return ParsePostgresBool(text, parsed) && !parsed;
	}
	if (key == "comment") {
		return text.empty();
	}
	if (key == "skip") {
		return text == "0";
	}
	return false;
}

bool IsDuckDBWriteOption(const Identifier &key) {
	return key == "new_line" || key == "prefix" || key == "suffix" || key == "dateformat" || key == "date_format" ||
	       key == "timestampformat" || key == "timestamp_format";
}

void ApplyDuckDBWriteOption(CopyFormatOptions &out, const Identifier &key, const vector<Value> &values) {
	if (key == "new_line") {
		auto new_line = OptionText(values);
		new_line = StringUtil::Replace(new_line, "\\r", "\r");
		out.new_line = StringUtil::Replace(new_line, "\\n", "\n");
	} else if (key == "prefix") {
		out.prefix = OptionText(values);
	} else if (key == "suffix") {
		out.suffix = OptionText(values);
	} else if (key == "dateformat" || key == "date_format") {
		out.date_format = OptionText(values);
	} else {
		out.timestamp_format = OptionText(values);
	}
}

void ApplyBinaryOption(CopyFormatOptions &out, const Identifier &key, const vector<Value> &values, bool from) {
	if (key == "log_verbosity") {
		out.log_verbosity = ParseCopyLogVerbosity(values);
	} else if (key == "freeze") {
		if (!from) {
			ThrowInvalid("COPY FREEZE cannot be used with COPY TO");
		}
		out.freeze = ParseBoolOption(values, "freeze");
	} else if (key == "ignore_errors") {
		if (!from) {
			ThrowInvalid("COPY ON_ERROR cannot be used with COPY TO");
		}
		if (ParseBoolOption(values, "ON_ERROR")) {
			ThrowCopyError(SQLSTATE_SYNTAX_ERROR, "only ON_ERROR STOP is allowed in BINARY mode");
		}
	} else if (key == "rejects_limit") {
		ThrowInvalid("COPY REJECT_LIMIT requires ON_ERROR to be set to IGNORE");
	} else if (key == "delimiter" || key == "delim" || key == "sep" || key == "separator") {
		ThrowCopyError(SQLSTATE_SYNTAX_ERROR, "cannot specify DELIMITER in BINARY mode");
	} else if (key == "null" || key == "nullstr") {
		ThrowCopyError(SQLSTATE_SYNTAX_ERROR, "cannot specify NULL in BINARY mode");
	} else if (key == "default") {
		ThrowCopyError(SQLSTATE_SYNTAX_ERROR, "cannot specify DEFAULT in BINARY mode");
	} else if (key == "header") {
		ThrowCopyError(SQLSTATE_FEATURE_NOT_SUPPORTED, "cannot specify HEADER in BINARY mode");
	} else if (key == "quote" || key == "escape" || key == "force_quote" || key == "force_not_null" ||
	           key == "force_null") {
		ThrowRequiresCsv(StringUtil::Upper(key.GetIdentifierName()));
	} else {
		ThrowNotRecognized(key.GetIdentifierName());
	}
}

void ApplyOption(CopyFormatOptions &out, const Identifier &key, const vector<Value> &values, bool from) {
	const bool csv = out.format == CopyFormat::CSV;
	if (key == "format") {
		return;
	}
	if (out.format == CopyFormat::BINARY) {
		ApplyBinaryOption(out, key, values, from);
		return;
	}
	if (csv && !from && IsDuckDBWriteOption(key)) {
		ApplyDuckDBWriteOption(out, key, values);
		return;
	}
	if (csv && !from && (key == "quote" || key == "escape") && OptionText(values).empty()) {
		(key == "quote" ? out.quote : out.escape) = '\0';
		return;
	}
	if (key == "delimiter" || key == "delim" || key == "sep" || key == "separator") {
		out.delimiter = OptionChar(values, "delimiter");
	} else if (key == "null" || key == "nullstr") {
		if (values.size() != 1) {
			ThrowInvalid("NULL requires a single string value");
		}
		out.null_str = values[0].ToString();
	} else if (key == "header") {
		ParseHeader(out, values);
	} else if (key == "default") {
		if (values.empty()) {
			ThrowCopyError(SQLSTATE_SYNTAX_ERROR, "default requires a parameter");
		}
		out.has_default = true;
		out.default_str = values[0].ToString();
	} else if (key == "ignore_errors") {
		out.ignore_errors = ParseBoolOption(values, "ON_ERROR");
	} else if (key == "rejects_limit") {
		out.reject_limit = ParseRejectLimit(values);
	} else if (key == "log_verbosity") {
		out.log_verbosity = ParseCopyLogVerbosity(values);
	} else if (key == "freeze") {
		out.freeze = ParseBoolOption(values, "freeze");
	} else if (key == "compression") {
		out.compression = FileCompressionType(OptionText(values));
	} else if (key == "encoding") {
		out.encoding = CopyEncodingName(OptionText(values));
	} else if (key == "rendering") {
		out.rendering = ValueRenderingFromString(OptionText(values), "RENDERING");
	} else if (key == "quote") {
		if (!csv) {
			ThrowRequiresCsv("QUOTE");
		}
		out.quote = OptionChar(values, "quote");
	} else if (key == "escape") {
		if (!csv) {
			ThrowRequiresCsv("ESCAPE");
		}
		out.escape = OptionChar(values, "escape");
	} else if (key == "force_quote") {
		if (!csv) {
			ThrowRequiresCsv("FORCE_QUOTE");
		}
		out.force_quote = ParseColumns(values, "FORCE_QUOTE");
	} else if (key == "force_not_null") {
		if (!csv) {
			ThrowRequiresCsv("FORCE_NOT_NULL");
		}
		out.force_not_null = ParseColumns(values, "FORCE_NOT_NULL");
	} else if (key == "force_null") {
		if (!csv) {
			ThrowRequiresCsv("FORCE_NULL");
		}
		out.force_null = ParseColumns(values, "FORCE_NULL");
	} else if (csv && IsDuckDBDefault(key, values)) {
		return;
	} else if (csv) {
		out.foreign.push_back(key.GetIdentifierName());
	} else {
		ThrowNotRecognized(key.GetIdentifierName());
	}
}

} // namespace

void ThrowCopyError(const char *sqlstate, const string &message) {
	ThrowCopyError(sqlstate, message, string());
}

void ThrowCopyError(const char *sqlstate, const string &message, const string &context, const string &detail,
                    const string &hint) {
	unordered_map<string, string> extra_info {{"sqlstate", sqlstate}};
	if (!context.empty()) {
		extra_info.emplace("context", context);
	}
	if (!detail.empty()) {
		extra_info.emplace("detail", detail);
	}
	if (!hint.empty()) {
		extra_info.emplace("hint", hint);
	}
	throw InvalidInputException(extra_info, message);
}

bool ParsePostgresBool(const string &text, bool &result) {
	for (auto word : {"1", "on", "true", "yes", "t", "y"}) {
		if (StringUtil::CIEquals(text, word)) {
			result = true;
			return true;
		}
	}
	for (auto word : {"0", "off", "false", "no", "f", "n"}) {
		if (StringUtil::CIEquals(text, word)) {
			result = false;
			return true;
		}
	}
	return false;
}

CopyFormatOptions ParseCopyFormatOptions(CopyFormat format, bool from, const identifier_map_t<vector<Value>> &options) {
	CopyFormatOptions result;
	result.format = format;
	if (format == CopyFormat::CSV) {
		result.delimiter = ',';
		result.null_str.clear();
	}
	bool escape_set = false;
	for (auto &entry : options) {
		ApplyOption(result, entry.first, entry.second, from);
		escape_set |= entry.first == "escape";
	}
	if (format == CopyFormat::CSV && !escape_set) {
		result.escape = result.quote;
	}
	return result;
}

void CheckCopyFormatOptions(const CopyFormatOptions &options, bool from) {
	if (options.format == CopyFormat::BINARY) {
		return;
	}
	const bool csv = options.format == CopyFormat::CSV;
	const auto delimiter = string(1, options.delimiter);
	if (from && (options.delimiter == '\r' || options.delimiter == '\n')) {
		ThrowInvalid("COPY delimiter cannot be newline or carriage return");
	}
	if (options.prefix.empty() != options.suffix.empty()) {
		ThrowInvalid("COPY ... (FORMAT CSV) must have both PREFIX and SUFFIX, or none at all");
	}
	if (!options.prefix.empty() && options.header) {
		ThrowInvalid("COPY ... (FORMAT CSV)'s HEADER cannot be combined with PREFIX/SUFFIX");
	}
	if (options.null_str.find_first_of("\r\n") != string::npos) {
		ThrowInvalid("COPY null representation cannot use newline or carriage return");
	}
	if (options.has_default && options.default_str.find_first_of("\r\n") != string::npos) {
		ThrowInvalid("COPY default representation cannot use newline or carriage return");
	}
	if (!csv && TEXT_RESERVED_DELIMITERS.find(options.delimiter) != std::string_view::npos) {
		ThrowInvalid("COPY delimiter cannot be \"" + delimiter + "\"");
	}
	if (options.header_match && !from) {
		ThrowCopyError(SQLSTATE_FEATURE_NOT_SUPPORTED, "cannot use \"match\" with HEADER in COPY TO");
	}
	if (csv && options.quote != '\0' && options.delimiter == options.quote) {
		ThrowInvalid("COPY delimiter and quote must be different");
	}
	const auto check_columns = [&](const CopyColumns &columns, const string &option, bool allowed_from) {
		if ((columns.all || !columns.names.empty()) && from != allowed_from) {
			ThrowInvalid("COPY " + option + " cannot be used with " + (from ? "COPY FROM" : "COPY TO"));
		}
	};
	check_columns(options.force_quote, "FORCE_QUOTE", false);
	check_columns(options.force_not_null, "FORCE_NOT_NULL", true);
	check_columns(options.force_null, "FORCE_NULL", true);
	if (options.null_str.find(options.delimiter) != string::npos) {
		ThrowInvalid("COPY delimiter character must not appear in the NULL specification");
	}
	if (csv && options.quote != '\0' && options.null_str.find(options.quote) != string::npos) {
		ThrowInvalid("CSV quote character must not appear in the NULL specification");
	}
	if (options.has_default) {
		if (!from) {
			ThrowInvalid("COPY DEFAULT cannot be used with COPY TO");
		}
		if (options.default_str.find(options.delimiter) != string::npos) {
			ThrowInvalid("COPY delimiter character must not appear in the DEFAULT specification");
		}
		if (csv && options.default_str.find(options.quote) != string::npos) {
			ThrowInvalid("CSV quote character must not appear in the DEFAULT specification");
		}
		if (options.default_str == options.null_str) {
			ThrowInvalid("NULL specification and DEFAULT specification cannot be the same");
		}
	}
	if (!from && options.ignore_errors) {
		ThrowInvalid("COPY ON_ERROR cannot be used with COPY TO");
	}
	if (!from && options.freeze) {
		ThrowInvalid("COPY FREEZE cannot be used with COPY TO");
	}
	if (options.reject_limit != 0 && !options.ignore_errors) {
		ThrowInvalid("COPY REJECT_LIMIT requires ON_ERROR to be set to IGNORE");
	}
	if (!from && !options.foreign.empty()) {
		ThrowNotRecognized(options.foreign.front());
	}
}

void NormalizeCopyErrorOptions(identifier_map_t<vector<Value>> &options) {
	auto on_error = options.find("on_error");
	if (on_error != options.end()) {
		const auto value = OptionText(on_error->second);
		const bool ignore = StringUtil::CIEquals(value, "ignore");
		if (!ignore && !StringUtil::CIEquals(value, "stop")) {
			ThrowInvalid("invalid value for parameter \"on_error\": \"" + value + "\"");
		}
		options.erase(on_error);
		if (ignore) {
			options["ignore_errors"] = {Value::BOOLEAN(true)};
		}
	}
	auto reject_limit = options.find("reject_limit");
	if (reject_limit != options.end()) {
		auto values = std::move(reject_limit->second);
		ParseRejectLimit(values);
		options.erase(reject_limit);
		options["rejects_limit"] = std::move(values);
	}
}

bool IsCopyFormatOption(const Identifier &key) {
	for (auto option : COPY_FORMAT_OPTIONS) {
		if (key == option) {
			return true;
		}
	}
	return false;
}

CopyLogVerbosity ParseCopyLogVerbosity(const vector<Value> &values) {
	const auto text = OptionText(values);
	if (StringUtil::CIEquals(text, "default")) {
		return CopyLogVerbosity::DEFAULT;
	}
	if (StringUtil::CIEquals(text, "verbose")) {
		return CopyLogVerbosity::VERBOSE;
	}
	if (StringUtil::CIEquals(text, "silent")) {
		return CopyLogVerbosity::SILENT;
	}
	ThrowInvalid("COPY LOG_VERBOSITY \"" + text + "\" not recognized");
}

vector<bool> CopyColumnMask(const CopyColumns &columns, const vector<string> &names, const string &option) {
	if (!columns.all && columns.names.empty()) {
		return {};
	}
	vector<bool> mask(names.size(), columns.all);
	for (auto &name : columns.names) {
		optional_idx found;
		for (idx_t i = 0; i < names.size() && !found.IsValid(); i++) {
			if (names[i] == name) {
				found = i;
			}
		}
		for (idx_t i = 0; i < names.size() && !found.IsValid(); i++) {
			if (StringUtil::CIEquals(names[i], name)) {
				found = i;
			}
		}
		if (!found.IsValid()) {
			ThrowCopyError(SQLSTATE_INVALID_COLUMN_REFERENCE,
			               option + " column \"" + name + "\" not referenced by COPY");
		}
		mask[found.GetIndex()] = true;
	}
	return mask;
}

string CopyEncodingName(const string &name) {
	string key;
	for (auto c : name) {
		if (c != '-' && c != '_') {
			key.push_back(StringUtil::CharacterToLower(c));
		}
	}
	if (key == "utf8" || key == "unicode" || key == "sqlascii") {
		return "utf-8";
	}
	if (key == "latin1" || key == "iso88591") {
		return "latin-1";
	}
	if (key == "utf16" || key == "utf16le") {
		return "utf-16le";
	}
	if (key == "utf16be") {
		return "utf-16be";
	}
	if (key == "utf32" || key == "utf32le") {
		return "utf-32le";
	}
	if (key == "utf32be") {
		return "utf-32be";
	}
	for (auto unsupported : UNSUPPORTED_POSTGRES_ENCODINGS) {
		if (key == unsupported) {
			ThrowCopyError(SQLSTATE_FEATURE_NOT_SUPPORTED, "COPY ENCODING \"" + name + "\" is not supported");
		}
	}
	ThrowInvalid("argument to option \"encoding\" must be a valid encoding name");
}

string CopyEncodingTitle(const string &name) {
	string title;
	for (auto c : name) {
		if (c != '-' && c != '_') {
			title.push_back(StringUtil::CharacterToUpper(c));
		}
	}
	return title;
}

} // namespace duckdb
