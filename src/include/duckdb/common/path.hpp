//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/path.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//
// Parsed representation of a path string, covering posix, windows, URI, and UNC forms.
//
// FromString(raw) parses and normalizes the input; ToString() reconstructs it as
// scheme + authority + anchor + join(segments, sep) + tail
//
// Supported input forms and their parsed fields:
//
//   Input                         scheme        authority      anchor   segments      IsAbsolute
//   ----------------------------  ------------  -----------    -------  ------------  -----------
//   "a/b"                         ""            ""             ""       "a/b"         false
//   ""  (empty)                   ""            ""             ""       "."           false
//   "/"                           ""            ""             "/"      ""            true
//   "/a/b"                        ""            ""             "/"      "a/b"         true
//   "/a/b/"                       ""            ""             "/"      "a/b"         true   (HasTrailingSeparator)
//   "file:/a/b"                   "file:"       ""             "/"      "a/b"         true
//   "file:///a/b"                 "file://"     ""             "/"      "a/b"         true
//   "file://localhost/a/b"        "file://"     "localhost"    "/"      "a/b"         true
//   "s3://bucket/bar/baz"         "s3://"       "bucket"       "/"      "bar/baz"     true
//   "C:\foo" (win)                ""            ""             "C:\"    "foo"         true
//   "C:relpath" (win)             ""            ""             "C:"     "relpath"     false
//   "\\server\share\p" (win)      "\\"          "server\share" "\"      "p"           true
//   "\\?\UNC\server\share" (win)  "\\?\UNC\"    "server\share" "\"      ""            true
//   "\\?\C:\foo" (win)            "\\?"         ""             "C:\"    "foo"         true
//
// HasTrailingSeparator is true when raw input ends with a separator. ToString() re-emits the
// separator after the last segment (skipped when segments is empty — anchor already ends with it).
// When Join()ing, LHS inherits trailing_separator from RHS. This is useful (and semantically
// meaningful) in distinguishing e.g. '/foo/*/'=dirs from '/foo/*'=all.
//
// Windows local paths may use '/' or '\\' -- whichever is found first will be applied to
// remainder of the normalized path; defaults to '/'.
//
class Path {
public:
	// Primary Constructor
	static Path FromString(std::string_view raw);

	//! What a caller can tell of a path without parsing it, to skip FromString where the answer is already there
	//! Whether FromString leaves [path] as it is, a trailing separator aside: a posix path, or a URL with a lowercase
	//! scheme and an authority, that has no query, no empty segment and no "." or ".." segment
	static bool IsNormalized(std::string_view path);
	//! Whether [name] is one segment of a path
	static bool IsSegment(std::string_view name);
	//! Whether FromString(path).IsRemote(), parsing no URL
	static bool IsRemote(std::string_view path);

	string ToString() const;

	// 3 constituent parts of to string -- base + path + trailing separator (IFF path not empty)
	string GetBase() const;               // scheme + authority + anchor
	string GetPath() const;               // relative path segments: join(segments, sep), or "." for empty relative
	string GetTrailingSeparator() const { // returns "" or string(1, separator)
		return !segments.empty() && has_trailing_separator ? string(1, separator) : "";
	}
	const string &GetScheme() const {
		return scheme;
	}
	const string &GetAuthority() const {
		return authority;
	}
	const string &GetAnchor() const {
		return anchor;
	}
	char GetSeparator() const {
		return separator;
	}

	bool HasScheme() const {
		return !scheme.empty();
	}
	bool HasAuthority() const {
		return !authority.empty();
	}
	bool HasAnchor() const {
		return !anchor.empty();
	}

	bool HasDrive() const;     // always false in non-windows
	char GetDriveChar() const; // returns \0 if no drive

	bool HasPathSegments() const {
		return !segments.empty();
	}

	bool HasTrailingSeparator() const {
		return has_trailing_separator;
	}

	bool IsAbsolute() const {
		return is_absolute;
	}

	// true for all relative paths, /*,  c:/* (not c:foo), file:/*, \\?\C:\*
	bool IsLocal() const {
		// note: HasDrive() covers UNC locals too!
		return (!IsAbsolute() || !HasScheme() || HasDrive() || StringUtil::StartsWith(scheme, "file:"));
	}

	bool IsRemote() const {
		return !IsLocal();
	}

	const vector<string> &GetPathSegments() const {
		return segments;
	}

	// Join (in several forms)
	Path Join(const Path &rhs) const;
	Path Join(const string &rhs) const;

	template <typename... Args>
	Path Join(const string &first, const Args &...rest) const {
		return Join(first).Join(rest...);
	}

	Path Join(const vector<string> &paths) const {
		Path result = *this;
		for (const auto &rhs : paths) {
			result = result.Join(rhs);
		}
		return result;
	}

	Path Parent(int n = 1) const;

	// public convenience - string-to-string normalize
	static string Normalize(const string &input) {
		return FromString(input).ToString();
	}

	static string AddSuffixToPath(const string &path, const string &suffix);

private:
	string scheme;
	string authority;
	string anchor;
	vector<string> segments;

	char separator = '/';
	bool has_trailing_separator = false;
	bool is_absolute = false;

	void NormalizeSegments(std::string_view raw, size_t path_offset);

	static size_t ParseURIScheme(std::string_view input, Path &parsed);
	static size_t ParseFilePathTail(std::string_view input, size_t start, Path &parsed);
	static size_t ParseFileSchemes(std::string_view input, Path &parsed);
#if defined(_WIN32)
	static size_t ParseUNCScheme(std::string_view input, Path &parsed);
#endif
};

} // namespace duckdb
