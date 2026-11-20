//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/copy_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/formats/value_codecs.hpp"

namespace duckdb {

enum class CopyTextFormat : uint8_t { TEXT, CSV };

enum class CopyLogVerbosity : uint8_t { DEFAULT, VERBOSE, SILENT };

struct CopyColumns {
	bool all = false;
	vector<string> names;
};

struct CopyFormatOptions {
	CopyTextFormat format = CopyTextFormat::TEXT;
	char delimiter = '\t';
	char quote = '"';
	char escape = '"';
	string null_str = "\\N";
	bool has_default = false;
	string default_str;
	bool header = false;
	bool header_match = false;
	bool ignore_errors = false;
	idx_t reject_limit = 0;
	CopyLogVerbosity log_verbosity = CopyLogVerbosity::DEFAULT;
	bool freeze = false;
	CopyColumns force_quote;
	CopyColumns force_not_null;
	CopyColumns force_null;
	FileCompressionType compression = FileCompressionType::AUTO_DETECT;
	string encoding = "utf-8";
	ValueRendering rendering = ValueRendering::POSTGRES;
	string new_line = "\n";
	string prefix;
	string suffix;
	string date_format;
	string timestamp_format;
	vector<string> foreign;
};

[[noreturn]] void ThrowCopyError(const char *sqlstate, const string &message);

[[noreturn]] void ThrowCopyError(const char *sqlstate, const string &message, const string &context,
                                 const string &detail = string(), const string &hint = string());

CopyFormatOptions ParseCopyFormatOptions(CopyTextFormat format, bool from,
                                         const identifier_map_t<vector<Value>> &options);

void CheckCopyFormatOptions(const CopyFormatOptions &options, bool from);

bool IsCopyFormatOption(const Identifier &key);

CopyLogVerbosity ParseCopyLogVerbosity(const vector<Value> &values);

vector<bool> CopyColumnMask(const CopyColumns &columns, const vector<string> &names, const string &option);

string CopyEncodingName(const string &name);

string CopyEncodingTitle(const string &name);

bool ParsePostgresBool(const string &text, bool &result);

} // namespace duckdb
