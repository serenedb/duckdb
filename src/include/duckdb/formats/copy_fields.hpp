//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/formats/copy_fields.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/formats/copy_from.hpp"

#include <string_view>

namespace duckdb {

enum class CopyFieldKind : uint8_t { VALUE, NULL_VALUE, DEFAULT_VALUE };

CopyFieldKind ReadCopyField(const CopyFromRules &rules, idx_t column, std::string_view raw, char *out,
                            std::string_view &value);

std::string_view UnescapePostgresText(std::string_view field, char *out, bool &escaped_high_or_nul);

std::string_view UnquotePostgresCsv(std::string_view field, char quote, char escape, char *out);

class CopyLineBreakFinder {
public:
	explicit CopyLineBreakFinder(const CopyFormatOptions &options);

	char Feed(std::string_view bytes);
	char Finish() const;

private:
	const bool csv;
	const char quote;
	const char escape;
	const bool quoted_escape;
	bool in_quote = false;
	bool after_escape = false;
	bool after_backslash = false;
	bool after_cr = false;
};

struct CopyEndScan {
	std::string_view carry;
	std::string_view data;
};

class CopyEndMarkerScanner {
public:
	CopyEndScan Scan(std::string_view chunk);
	std::string_view Finish();
	bool Ended() const {
		return ended;
	}

private:
	std::string_view Hold(std::string_view chunk, idx_t at);
	std::string_view ScanChunk(std::string_view chunk);

private:
	string pending;
	string carry;
	uint64_t escape_carry = 0;
	bool at_line_start = true;
	bool ended = false;
};

string CopyPrintout(std::string_view value);

idx_t Utf8SequenceLength(uint8_t lead);

string CopyInvalidUtf8(std::string_view bytes);

string CopyContext(const string &table, idx_t line, const string &column, bool has_value, const string &value);

} // namespace duckdb
