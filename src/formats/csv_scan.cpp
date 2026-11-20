#include "duckdb/formats/csv_scan.hpp"

#include "absl/cleanup/cleanup.h"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/formats/copy_fields.hpp"
#include "duckdb/formats/row_stream.hpp"
#include "duckdb/formats/value_parsers.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parallel/callback_async_task.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "simdutf.h"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

namespace {

constexpr idx_t LOOKUP_BYTES = 1 << 13;
constexpr idx_t RANGE_MIN = 1 << 20;
constexpr idx_t RANGE_MAX = 1 << 23;
constexpr idx_t HOST_STREAM_RANGE = 1 << 21;
constexpr idx_t HEAD_READ_BYTES = 1 << 16;

class WholeFileSource final : public RowSource {
public:
	explicit WholeFileSource(const CSVBufferHandle &handle_p) : handle(handle_p) {
	}

	idx_t Read(char *data, idx_t count, idx_t offset) const override {
		const idx_t available = offset < handle.actual_size ? MinValue(count, handle.actual_size - offset) : 0;
		memcpy(data, handle.Ptr() + offset, available);
		return available;
	}

private:
	const CSVBufferHandle &handle;
};

bool LeanDecodes(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::VARCHAR:
		return true;
	default:
		return false;
	}
}

[[gnu::always_inline]] inline void MakeString(const char *value, idx_t size, const char *limit, string_t &out) {
	if constexpr (string_t::INLINE_LENGTH == 12 && string_t::PREFIX_LENGTH == 4) {
		uint64_t words[2];
		if (size <= 12 && value + 12 <= limit) {
			uint32_t head;
			uint64_t tail;
			memcpy(&head, value, sizeof(head));
			memcpy(&tail, value + 4, sizeof(tail));
			const idx_t head_bytes = MinValue<idx_t>(size, 4);
			const idx_t tail_bytes = size > 4 ? size - 4 : 0;
			head &= head_bytes == 4 ? ~uint32_t(0) : (uint32_t(1) << (head_bytes * 8)) - 1;
			tail &= tail_bytes == 8 ? ~uint64_t(0) : (uint64_t(1) << (tail_bytes * 8)) - 1;
			words[0] = size | (uint64_t(head) << 32);
			words[1] = tail;
			memcpy(&out, words, sizeof(words));
			return;
		}
		if (size > 12) {
			uint32_t prefix;
			memcpy(&prefix, value, sizeof(prefix));
			words[0] = size | (uint64_t(prefix) << 32);
			words[1] = reinterpret_cast<uintptr_t>(value);
			memcpy(&out, words, sizeof(words));
			return;
		}
	}
	out = string_t(value, UnsafeNumericCast<uint32_t>(size));
}

inline bool LeanUnquote(const char *data, char quote, bool spaced, idx_t &begin, idx_t &end) {
	idx_t open = begin;
	if (spaced && end - open > 1 && data[open] == ' ' && data[open + 1] == quote) {
		open++;
	}
	if (end <= open || data[open] != quote) {
		return false;
	}
	idx_t close = end;
	while (spaced && close > open + 1 && data[close - 1] == ' ') {
		close--;
	}
	begin = open + 1;
	end = MaxValue(close - 1, begin);
	return true;
}

template <class T, class PARSE>
void LeanValues(const LeanSource &source, idx_t count, bool force_not_null, Vector &vector, PARSE parse,
                duckdb::vector<idx_t> &retry) {
	T *const out = FlatVector::GetDataMutable<T>(vector);
	const uint32_t *const begins = source.begins;
	const idx_t begin_stride = source.begin_stride;
	const idx_t begin_bias = source.begin_bias;
	const uint32_t *const ends = source.ends;
	const idx_t stride = source.stride;
	const char *const data = source.data;
	const idx_t shift = source.shift;
	const char quote = source.quote;
	const bool quotes = source.quotes;
	const bool doubled = source.doubled;
	const uint32_t flag_test =
	    quotes || source.padded || source.unquoted_escape || source.retry_flagged ? ROW_FIELD_FLAG : 0;
	const bool empty_only_null = source.empty_only_null && source.allow_quoted_nulls;
	const auto default_marker = source.default_marker;
	const auto selected = source.selected;
	const idx_t first = source.first;
	for (idx_t row = first; row < first + count; row++) {
		if (selected && !(*selected)[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		const uint32_t end_bits = ends[row * stride];
		idx_t begin = (begins[row * begin_stride] & ROW_FIELD_MASK) + begin_bias + shift;
		idx_t end = (end_bits & ROW_FIELD_MASK) + shift;
		bool quoted = false;
		if (end_bits & flag_test) {
			if (begin > end) {
				FlatVector::SetNull(vector, row, true);
				continue;
			}
			if (source.retry_flagged) {
				retry.push_back(row);
				FlatVector::SetNull(vector, row, true);
				continue;
			}
			quoted = quotes && LeanUnquote(data, quote, source.spaced, begin, end);
			if (!quoted && source.unquoted_escape && memchr(data + begin, source.unquoted_escape, end - begin)) {
				retry.push_back(row);
				FlatVector::SetNull(vector, row, true);
				continue;
			}
		}
		const char *value = data + begin;
		const idx_t size = end - begin;
		if (empty_only_null ? size == 0 : source.IsNull(value, size, quoted)) {
			if (force_not_null) {
				retry.push_back(row);
			}
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		if (default_marker && size == default_marker->size() && memcmp(value, default_marker->data(), size) == 0) {
			retry.push_back(row);
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		if ((quoted && doubled && memchr(value, source.escape_probe, size)) || !parse(value, size, out[row])) {
			retry.push_back(row);
			FlatVector::SetNull(vector, row, true);
		}
	}
}

bool IsStringColumnRef(const Expression &expr) {
	return ExpressionFilter::IsSimpleFilterColumnRef(expr) && expr.GetReturnType().id() == LogicalTypeId::VARCHAR &&
	       !expr.GetReturnType().HasAlias();
}

const string *StringConstant(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		return nullptr;
	}
	return &StringValue::Get(value);
}

bool CollectRawPattern(const Expression &expr, bool negated, vector<CsvRawPredicate> &out) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &children = function.GetChildren();
	if (children.size() != 2 || !IsStringColumnRef(*children[0])) {
		return false;
	}
	auto pattern = StringConstant(*children[1]);
	if (!pattern) {
		return false;
	}
	auto &name = function.Function().GetName();
	CsvRawPredicate predicate {negated ? CsvRawKind::NOT_LIKE : CsvRawKind::LIKE};
	if (name == "prefix" || name == "starts_with" || name == "^@") {
		predicate.anchored_start = true;
		predicate.values.push_back(*pattern);
	} else if (name == "suffix" || name == "ends_with") {
		predicate.anchored_end = true;
		predicate.values.push_back(*pattern);
	} else if (name == "contains") {
		predicate.values.push_back(*pattern);
	} else if (name == "~~" || name == "!~~") {
		if (pattern->find_first_of("_\\") != string::npos) {
			return false;
		}
		if (name == "!~~") {
			predicate.kind = negated ? CsvRawKind::LIKE : CsvRawKind::NOT_LIKE;
		}
		predicate.anchored_start = pattern->empty() || pattern->front() != '%';
		predicate.anchored_end = pattern->empty() || pattern->back() != '%';
		idx_t begin = 0;
		while (begin <= pattern->size()) {
			auto end = std::min(pattern->find('%', begin), pattern->size());
			if (end > begin) {
				predicate.values.push_back(pattern->substr(begin, end - begin));
			}
			begin = end + 1;
		}
	} else {
		return false;
	}
	out.push_back(std::move(predicate));
	return true;
}

bool CollectRawPredicates(const Expression &expr, bool string_column, vector<CsvRawPredicate> &out) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		if (expr.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return false;
		}
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			if (!CollectRawPredicates(*child, string_column, out)) {
				return false;
			}
		}
		return true;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		const auto type = op.GetExpressionType();
		auto &children = op.GetChildren();
		if (type == ExpressionType::OPERATOR_NOT) {
			return string_column && children.size() == 1 && CollectRawPattern(*children[0], true, out);
		}
		if (children.empty() || !ExpressionFilter::IsSimpleFilterColumnRef(*children[0])) {
			return false;
		}
		if (type == ExpressionType::OPERATOR_IS_NULL || type == ExpressionType::OPERATOR_IS_NOT_NULL) {
			out.push_back({type == ExpressionType::OPERATOR_IS_NULL ? CsvRawKind::IS_NULL : CsvRawKind::IS_NOT_NULL});
			return true;
		}
		if (type != ExpressionType::COMPARE_IN || !string_column) {
			return false;
		}
		CsvRawPredicate predicate {CsvRawKind::IN_LIST};
		for (idx_t i = 1; i < children.size(); i++) {
			auto value = StringConstant(*children[i]);
			if (!value) {
				return false;
			}
			predicate.values.push_back(*value);
		}
		std::sort(predicate.values.begin(), predicate.values.end());
		out.push_back(std::move(predicate));
		return true;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (!string_column) {
			return false;
		}
		auto &function = expr.Cast<BoundFunctionExpression>();
		auto type = function.GetExpressionType();
		if (type == ExpressionType::BOUND_FUNCTION) {
			return CollectRawPattern(function, false, out);
		}
		if (!BoundComparisonExpression::IsComparison(type) || type == ExpressionType::COMPARE_DISTINCT_FROM ||
		    type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return false;
		}
		auto constant = ExpressionFilter::TryGetColumnConstantComparison(function, type);
		if (!constant) {
			return false;
		}
		auto &value = constant->GetValue();
		if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		out.push_back({CsvRawKind::COMPARE, type, {StringValue::Get(value)}});
		return true;
	}
	default:
		return false;
	}
}

bool RawLike(const CsvRawPredicate &predicate, std::string_view field) {
	auto &segments = predicate.values;
	idx_t first = 0;
	idx_t last = segments.size();
	idx_t begin = 0;
	idx_t end = field.size();
	if (predicate.anchored_start && last > 0) {
		if (!field.starts_with(segments[0])) {
			return false;
		}
		begin = segments[0].size();
		first = 1;
	}
	if (predicate.anchored_end) {
		if (first == last) {
			return begin == end;
		}
		auto &segment = segments[last - 1];
		if (end - begin < segment.size() || !field.ends_with(segment)) {
			return false;
		}
		end -= segment.size();
		last--;
	}
	const auto window = field.substr(0, end);
	for (idx_t i = first; i < last; i++) {
		const auto position = window.find(segments[i], begin);
		if (position == std::string_view::npos) {
			return false;
		}
		begin = position + segments[i].size();
	}
	return true;
}

bool RawCompare(ExpressionType type, std::string_view field, const string &constant) {
	const string_t left(field.data(), UnsafeNumericCast<uint32_t>(field.size()));
	const string_t right(constant.data(), UnsafeNumericCast<uint32_t>(constant.size()));
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return Equals::Operation(left, right);
	case ExpressionType::COMPARE_NOTEQUAL:
		return NotEquals::Operation(left, right);
	case ExpressionType::COMPARE_LESSTHAN:
		return LessThan::Operation(left, right);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return LessThanEquals::Operation(left, right);
	case ExpressionType::COMPARE_GREATERTHAN:
		return GreaterThan::Operation(left, right);
	default:
		return GreaterThanEquals::Operation(left, right);
	}
}

bool RawPasses(const vector<CsvRawPredicate> &predicates, const char *value, idx_t size, bool null) {
	for (auto &predicate : predicates) {
		if (predicate.kind == CsvRawKind::IS_NULL) {
			if (!null) {
				return false;
			}
			continue;
		}
		if (null) {
			return false;
		}
		const std::string_view field(value, size);
		switch (predicate.kind) {
		case CsvRawKind::IS_NOT_NULL:
			break;
		case CsvRawKind::IN_LIST:
			if (!std::binary_search(predicate.values.begin(), predicate.values.end(), field)) {
				return false;
			}
			break;
		case CsvRawKind::LIKE:
		case CsvRawKind::NOT_LIKE:
			if (RawLike(predicate, field) != (predicate.kind == CsvRawKind::LIKE)) {
				return false;
			}
			break;
		default:
			if (!RawCompare(predicate.compare, field, predicate.values[0])) {
				return false;
			}
			break;
		}
	}
	return true;
}

void LeanRawPredicates(const LeanSource &source, idx_t count, const vector<CsvRawPredicate> &predicates,
                       vector<bool> &selected) {
	const uint32_t flag_test = source.quotes || source.padded || source.unquoted_escape ? ROW_FIELD_FLAG : 0;
	const bool empty_only_null = source.empty_only_null && source.allow_quoted_nulls;
	for (idx_t row = 0; row < count; row++) {
		if (!selected[row]) {
			continue;
		}
		const uint32_t end_bits = source.ends[row * source.stride];
		idx_t begin = (source.begins[row * source.begin_stride] & ROW_FIELD_MASK) + source.begin_bias + source.shift;
		idx_t end = (end_bits & ROW_FIELD_MASK) + source.shift;
		bool quoted = false;
		if (end_bits & flag_test) {
			if (begin > end) {
				selected[row] = RawPasses(predicates, nullptr, 0, true);
				continue;
			}
			quoted = source.quotes && LeanUnquote(source.data, source.quote, source.spaced, begin, end);
			if (!quoted && source.unquoted_escape && memchr(source.data + begin, source.unquoted_escape, end - begin)) {
				continue;
			}
		}
		const char *value = source.data + begin;
		const idx_t size = end - begin;
		if (quoted && source.doubled && memchr(value, source.escape_probe, size)) {
			continue;
		}
		const bool null = empty_only_null ? size == 0 : source.IsNull(value, size, quoted);
		selected[row] = RawPasses(predicates, value, size, null);
	}
}

void LeanStrings(const LeanSource &source, idx_t count, const CsvScanColumn &column, bool validate,
                 const CsvGrammar &grammar, Vector &vector, duckdb::vector<idx_t> &retry) {
	string_t *const out = FlatVector::GetDataMutable<string_t>(vector);
	const uint32_t *const begins = source.begins;
	const idx_t begin_stride = source.begin_stride;
	const idx_t begin_bias = source.begin_bias;
	const uint32_t *const ends = source.ends;
	const idx_t stride = source.stride;
	const char *const limit = source.limit;
	const char *const data = source.data;
	const idx_t shift = source.shift;
	const char quote = source.quote;
	const bool quotes = source.quotes;
	const uint32_t flag_test =
	    quotes || source.padded || source.unquoted_escape || source.retry_flagged ? ROW_FIELD_FLAG : 0;
	const bool empty_only_null = source.empty_only_null && source.allow_quoted_nulls;
	const auto default_marker = source.default_marker;
	const auto selected = source.selected;
	const idx_t first = source.first;
	for (idx_t row = first; row < first + count; row++) {
		if (selected && !(*selected)[row]) {
			FlatVector::SetNull(vector, row, true);
			continue;
		}
		const uint32_t end_bits = ends[row * stride];
		idx_t begin = (begins[row * begin_stride] & ROW_FIELD_MASK) + begin_bias + shift;
		idx_t end = (end_bits & ROW_FIELD_MASK) + shift;
		bool quoted = false;
		if (end_bits & flag_test) {
			if (begin > end) {
				if (source.pad_empty) {
					out[row] = string_t();
				} else {
					FlatVector::SetNull(vector, row, true);
				}
				continue;
			}
			if (source.retry_flagged) {
				retry.push_back(row);
				out[row] = string_t();
				continue;
			}
			quoted = quotes && LeanUnquote(data, quote, source.spaced, begin, end);
			if (!quoted && source.unquoted_escape && memchr(data + begin, source.unquoted_escape, end - begin)) {
				retry.push_back(row);
				out[row] = string_t();
				continue;
			}
		}
		const char *value = data + begin;
		const idx_t size = end - begin;
		if (empty_only_null ? size == 0 : source.IsNull(value, size, quoted)) {
			if (column.force_not_null) {
				out[row] = string_t();
			} else {
				FlatVector::SetNull(vector, row, true);
			}
			continue;
		}
		if (default_marker && size == default_marker->size() && memcmp(value, default_marker->data(), size) == 0) {
			retry.push_back(row);
			out[row] = string_t();
			continue;
		}
		if (validate && !Utf8Proc::IsValid(value, UnsafeNumericCast<uint32_t>(size))) {
			retry.push_back(row);
			out[row] = string_t();
			continue;
		}
		if (quoted && source.doubled && memchr(value, source.escape_probe, size)) {
			out[row] = CsvRemoveEscape(value, size, grammar.escape, grammar.quote, grammar.strict_mode, vector);
			continue;
		}
		MakeString(value, size, limit, out[row]);
	}
}

string CastMessage(const char *value, idx_t size, const string &type) {
	string message = "Could not convert string \"" + string(value, size) + "\" to '" + type + "'";
	CsvSanitize(message);
	return message;
}

idx_t SkipBom(const char *data, idx_t size) {
	if (size >= 3 && data[0] == '\xEF' && data[1] == '\xBB' && data[2] == '\xBF') {
		return 3;
	}
	return 0;
}

} // namespace

bool CsvDecodesDirectly(const LogicalType &type, bool icu_loaded) {
	if (type.HasAlias()) {
		return false;
	}
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIME:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::BOOLEAN:
		return true;
	case LogicalTypeId::TIMESTAMP_TZ:
		return !icu_loaded;
	case LogicalTypeId::VARCHAR:
		return true;
	default:
		return false;
	}
}

string_t CsvRemoveEscape(const char *value, idx_t size, char escape, char quote, bool strict_mode, Vector &vector) {
	auto keep = [&](idx_t pos, bool &escaped) {
		const char c = value[pos];
		if (c == escape && !escaped) {
			escaped = true;
			return false;
		}
		const bool kept = c != quote || escaped || !strict_mode;
		escaped = false;
		return kept;
	};
	idx_t length = 0;
	bool escaped = false;
	for (idx_t pos = 0; pos < size; pos++) {
		length += keep(pos, escaped);
	}
	auto result = StringVector::EmptyString(vector, length);
	auto out = result.GetDataWriteable();
	escaped = false;
	for (idx_t pos = 0, written = 0; pos < size; pos++) {
		if (keep(pos, escaped)) {
			out[written++] = value[pos];
		}
	}
	result.Finalize();
	return result;
}

void CsvSanitize(string &text) {
	text.push_back('\0');
	Utf8Proc::MakeValid(&text[0], text.size());
	text.pop_back();
}

static CsvScanSource FileSource(CSVFileScan &file) {
	CsvScanSource source;
	source.state_machine = file.state_machine;
	source.buffer_manager = file.buffer_manager;
	source.error_handler = file.error_handler;
	source.projection.assign(file.projection_ids.begin(), file.projection_ids.end());
	source.types = file.file_types;
	source.names = file.GetNames();
	source.path = file.GetFileName();
	source.file_row_number = file.file_row_number_idx;
	source.bytes_read = &file.bytes_read;
	source.placeholder = file.column_ids.empty();
	return source;
}

static CsvScanSource FileSource(CSVFileScan &file, optional_ptr<TableFilterSet> filters) {
	auto source = FileSource(file);
	source.filters = filters;
	return source;
}

CsvScanGlobal::CsvScanGlobal(ClientContext &context_p, CSVFileScan &file, optional_ptr<TableFilterSet> filters_p)
    : CsvScanGlobal(context_p, file.options, FileSource(file, filters_p)) {
}

CsvScanGlobal::CsvScanGlobal(ClientContext &context_p, const CSVReaderOptions &options_p, CsvScanSource source_p)
    : context(context_p), options(options_p), source(std::move(source_p)) {
	auto &state_machine = *source.state_machine;
	grammar = CsvGrammar::For(state_machine);
	copy = options.copy_rules.get();

	const bool icu_loaded = context.db->ExtensionIsLoaded("icu");
	for (idx_t i = 0; i < source.projection.size(); i++) {
		CsvScanColumn column;
		column.file_column = source.projection[i].first;
		column.output_column = source.projection[i].second;
		column.type = source.types[i];
		const bool rendered = options.rendering == ValueRendering::POSTGRES && !source.sniffing &&
		                      column.type.id() != LogicalTypeId::VARCHAR;
		auto parse_type = column.type.HasAlias() || rendered ? LogicalType::VARCHAR : column.type;
		if (CsvDecodesDirectly(parse_type, icu_loaded)) {
			column.parse_type = parse_type;
			column.validate_utf8 = true;
		} else {
			column.parse_type = LogicalType::VARCHAR;
			column.validate_utf8 = rendered || column.type.id() == LogicalTypeId::VARCHAR || column.type.IsNested();
		}
		column.force_not_null =
		    column.file_column < options.force_not_null.size() && options.force_not_null[column.file_column];
		column.match_null = !copy || copy->force_not_null.empty() || !copy->force_not_null[column.file_column];
		columns.push_back(std::move(column));
	}
	filtered.assign(columns.size(), false);
	if (source.filters) {
		for (auto &entry : *source.filters) {
			CsvScanFilter filter {entry.GetIndex(), entry.Filter()};
			for (idx_t i = 0; i < columns.size(); i++) {
				if (columns[i].output_column == filter.output_column) {
					filtered[i] = true;
					filter.column = i;
				}
			}
			auto &expression_filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "read_csv");
			if (filter.column != DConstants::INVALID_INDEX && expression_filter.column_indexes.empty() &&
			    !columns[filter.column].force_not_null) {
				auto &column = columns[filter.column];
				const bool string_column = column.type.id() == LogicalTypeId::VARCHAR && !column.type.HasAlias() &&
				                           column.parse_type.id() == LogicalTypeId::VARCHAR;
				if (!CollectRawPredicates(*expression_filter.expr, string_column, filter.raw)) {
					filter.raw.clear();
				}
			}
			filters.push_back(std::move(filter));
		}
	}
	late_materialization = !filters.empty() && !options.store_rejects.GetValue();
	date_format = options.dialect_options.date_format.at(LogicalTypeId::DATE).GetValue();
	timestamp_format = options.dialect_options.date_format.at(LogicalTypeId::TIMESTAMP).GetValue();
	decimal_separator = options.decimal_separator[0];
	null_strings = options.null_str;
	for (auto &null_string : null_strings) {
		empty_is_null = empty_is_null || null_string.empty();
	}
	ignore_errors = options.IgnoreErrors();
	print_line = options.store_rejects.GetValue() || !options.ignore_errors.GetValue();
	if (source.sniffing) {
		skip_rows = state_machine.dialect_options.skip_rows.GetValue();
		skip_bom = skip_rows + state_machine.dialect_options.header.GetValue() == 0;
	} else {
		skip_rows = options.GetSkipRows() + options.dialect_options.header.GetValue();
		skip_bom = skip_rows == 0 && !copy;
	}

	auto &manager = *source.buffer_manager;
	if (manager.file_handle->compression_type == FileCompressionType::UNCOMPRESSED && options.encoding == "utf-8") {
		host_stream = FindRowStream(context, source.path);
	}
	stream = host_stream || !manager.HasKnownBufferRanges();
	const idx_t size = stream ? NumericLimits<idx_t>::Maximum() : manager.file_handle->FileSize();
	num_columns = source.state_machine->dialect_options.num_cols;
	if (num_columns == 0) {
		throw InternalException("CSV scan of \"%s\" without columns", source.path);
	}
	RowSource *rows = host_stream.get();
	if (!rows) {
		row_source = make_uniq<CsvRowSource>(manager, size, source.bytes_read);
		rows = row_source.get();
	}
	shared_ptr<CSVBufferHandle> whole_file;
	if (!stream && size <= manager.GetBufferSize()) {
		auto handle = Buffer(0);
		if (handle && handle->actual_size >= size) {
			whole_file = std::move(handle);
		}
	}
	unique_ptr<WholeFileSource> whole_source;
	if (whole_file) {
		whole_source = make_uniq<WholeFileSource>(*whole_file);
	}
	RowSource &head = whole_source ? *whole_source : *rows;
	if (copy) {
		auto &copy_options = copy->options;
		const char line_break = CopyInputLineBreak(head);
		const auto dialect = copy->Text() ? RowDialect::PostgresText(copy_options.delimiter, line_break)
		                                  : RowDialect::PostgresCsv(copy_options.delimiter, copy_options.quote,
		                                                            copy_options.escape, line_break);
		lean = make_uniq<RowLexer>(dialect, num_columns, NumericLimits<idx_t>::Maximum(), true, false, true);
	} else {
		auto dialect = RowDialect::DuckDB(grammar);
		dialect.empty_lines = num_columns == 1 && empty_is_null;
		lean = make_uniq<RowLexer>(dialect, num_columns, options.maximum_line_size.GetValue(), !ignore_errors,
		                           options.null_padding, true);
	}
	count_only = (columns.empty() || source.placeholder) && filters.empty() &&
	             source.file_row_number == DConstants::INVALID_INDEX;
	empty_only_null = null_strings.size() == 1 && null_strings[0].empty();
	lean_batches = !count_only && !late_materialization && !(copy && copy->Text());
	for (auto &column : columns) {
		lean_batches = lean_batches && !CsvScan::NeedsFlushCast(column);
	}
	FindStart(size, head);
	if (copy && copy->options.header_match) {
		MatchCopyHeader(head);
	}
	idx_t range_bytes = DConstants::INVALID_INDEX;
	if (!stream && !source.sniffing) {
		const idx_t data_size = size - MinValue(start, size);
		const idx_t threads = MaxValue<idx_t>(TaskScheduler::QueryThreads(context), 1);
		const idx_t wanted = MinValue(MaxValue(data_size / (threads * 4), RANGE_MIN), RANGE_MAX);
		range_bytes = MaxValue<idx_t>(MinValue<idx_t>(options.buffer_size_option.GetValue(), wanted), 1);
	} else if (stream && !source.sniffing && options.parallel) {
		const idx_t limit = host_stream ? HOST_STREAM_RANGE : RANGE_MAX;
		range_bytes = MaxValue<idx_t>(MinValue<idx_t>(options.buffer_size_option.GetValue(), limit), 1);
	}
	ranges = make_uniq<RowRanges>(*lean, *rows, start, size, range_bytes, skip_rows == 0);
	ranges->path = source.path;
	if (whole_file) {
		const char *data = whole_file->Ptr();
		ranges->SetWholeFile(make_shared_ptr<CsvBufferView>(std::move(whole_file)), data);
	}
}

CsvRowSource::CsvRowSource(CSVBufferManager &manager_p, idx_t size_p, optional_ptr<atomic<idx_t>> bytes_read_p)
    : manager(manager_p), size(size_p), bytes_read(bytes_read_p) {
}

idx_t CsvRowSource::Read(char *data, idx_t count, idx_t offset) const {
	if (manager.HasKnownBufferRanges()) {
		const idx_t available = offset < size ? MinValue(count, size - offset) : 0;
		if (available > 0) {
			manager.file_handle->ReadAt(data, available, offset);
		}
		return available;
	}
	const idx_t buffer_bytes = manager.GetBufferSize();
	idx_t read = 0;
	while (read < count) {
		const idx_t position = offset + read;
		auto handle = manager.GetBuffer(position / buffer_bytes);
		const idx_t within = position % buffer_bytes;
		if (!handle || within >= handle->actual_size) {
			break;
		}
		const idx_t piece = MinValue(count - read, handle->actual_size - within);
		memcpy(data + read, handle->Ptr() + within, piece);
		read += piece;
	}
	return read;
}

void CsvRowSource::Release(idx_t position) {
	const idx_t buffer_bytes = manager.GetBufferSize();
	for (; released < position / buffer_bytes; released++) {
		*bytes_read += buffer_bytes;
		manager.ResetBuffer(released);
	}
}

void CsvScanGlobal::FindStart(idx_t size, const RowSource &rows) {
	if (skip_rows == 0) {
		char head[3];
		start = skip_bom ? SkipBom(head, rows.Read(head, sizeof(head), 0)) : 0;
		start_lines = 0;
		return;
	}
	RowSkip skip(copy ? lean->Dialect() : RowDialect::DuckDB(grammar), skip_rows);
	string chunk(HEAD_READ_BYTES, '\0');
	idx_t offset = 0;
	while (true) {
		const idx_t got = rows.Read(&chunk[0], chunk.size(), offset);
		const bool last = got < chunk.size();
		const bool found = skip.Feed(chunk.data(), offset, offset + got, last);
		offset += got;
		if (found || last) {
			break;
		}
	}
	if (skip.Start() == DConstants::INVALID_INDEX && source.sniffing) {
		start = 0;
		start_lines = 0;
		return;
	}
	start = MinValue(skip.Start() == DConstants::INVALID_INDEX ? offset : skip.Start(), size);
	start_lines = skip.Lines();
}

char CsvScanGlobal::CopyInputLineBreak(const RowSource &rows) const {
	CopyLineBreakFinder finder(copy->options);
	string chunk(HEAD_READ_BYTES, '\0');
	for (idx_t offset = 0;;) {
		const idx_t got = rows.Read(&chunk[0], chunk.size(), offset);
		const char found = finder.Feed(std::string_view(chunk.data(), got));
		if (found != '\0') {
			return found;
		}
		if (got < chunk.size()) {
			return finder.Finish();
		}
		offset += got;
	}
}

void CsvScanGlobal::MatchCopyHeader(RowSource &rows) const {
	if (start == 0) {
		return;
	}
	RowLexer lexer(lean->Dialect(), num_columns, NumericLimits<idx_t>::Maximum(), true, false, true);
	RowRanges head_ranges(lexer, rows, 0, start, DConstants::INVALID_INDEX, true);
	RowRangeReader reader(head_ranges);
	reader.Start(0, true, false, false);
	const auto result = reader.Advance(1);
	if ((result != RowReadResult::ROWS && result != RowReadResult::FLUSH) || reader.Ready() == 0) {
		return;
	}
	auto &fields = reader.fields;
	auto &window = *reader.bytes;
	const idx_t shift = reader.cursor.anchor - window.offset;
	const idx_t row_begin = fields.rows[0] + shift;
	const std::string_view line(window.data + row_begin, reader.RowEnd(0) - window.offset - row_begin);
	const auto context = CopyContext(copy->table, 1, string(), true, CopyPrintout(line));
	if (reader.cursor.unfinished) {
		ThrowCopyError("22P04", "unterminated CSV quoted field", context);
	}
	const auto *anomaly = fields.anomalies.empty() ? nullptr : fields.Anomaly(0);
	const idx_t count = anomaly ? anomaly->fields : num_columns;
	auto &names = copy->names;
	if (count != names.size()) {
		ThrowCopyError("22P04",
		               "wrong number of fields in header line: got " + std::to_string(count) + ", expected " +
		                   std::to_string(names.size()),
		               context);
	}
	const idx_t delimiter_size = lexer.Dialect().delimiter_size;
	string buffer;
	for (idx_t column = 0; column < count; column++) {
		const idx_t begin =
		    column == 0 ? row_begin : (fields.ends[column - 1] & ROW_FIELD_MASK) + shift + delimiter_size;
		const idx_t end = (fields.ends[column] & ROW_FIELD_MASK) + shift;
		const std::string_view raw(window.data + begin, end - begin);
		buffer.resize(raw.size());
		std::string_view value = raw;
		const auto kind = ReadCopyField(*copy, column, raw, buffer.data(), value);
		const auto field = std::to_string(column + 1);
		if (kind == CopyFieldKind::NULL_VALUE) {
			ThrowCopyError("22P04",
			               "column name mismatch in header line field " + field + ": got null value (\"" +
			                   copy->options.null_str + "\"), expected \"" + names[column] + "\"",
			               context);
		}
		if (kind == CopyFieldKind::DEFAULT_VALUE) {
			value = raw;
		}
		if (value != names[column]) {
			ThrowCopyError("22P04",
			               "column name mismatch in header line field " + field + ": got \"" + string(value) +
			                   "\", expected \"" + names[column] + "\"",
			               context);
		}
	}
}

idx_t CsvScanGlobal::MaxThreads() const {
	if (!options.parallel) {
		return 1;
	}
	if (ranges->Dynamic()) {
		return MaxValue<idx_t>(TaskScheduler::QueryThreads(context), 1);
	}
	return MaxValue<idx_t>(ranges->RangeCount(), 1);
}

vector<unique_ptr<AsyncTask>> CsvScanGlobal::LoadTasks(idx_t range) const {
	return vector<unique_ptr<AsyncTask>>();
}

bool CsvScanGlobal::FinishRange(idx_t range, idx_t lines) {
	source.error_handler->Insert(range, lines);
	return ranges->Finish(range);
}

CsvScan::CsvScan(CsvScanGlobal &global_p)
    : global(global_p), source(global_p.source), options(global_p.options), grammar(global_p.grammar),
      reader(*global_p.ranges), bytes(reader.bytes), cursor(reader.cursor), fields(reader.fields) {
	value_errors.resize(STANDARD_VECTOR_SIZE);
	failed.resize(STANDARD_VECTOR_SIZE);
	selected.resize(STANDARD_VECTOR_SIZE);
	keep.Initialize(STANDARD_VECTOR_SIZE);
	filter_candidates.Initialize(STANDARD_VECTOR_SIZE);
	for (auto &filter : global.filters) {
		filter_states.push_back(TableFilterState::Initialize(global.context, filter.filter.get()));
	}
	auto &allocator = Allocator::Get(global.context);
	vector<LogicalType> parse_types(global.columns.size(), LogicalType::VARCHAR);
	if (!parse_types.empty()) {
		parse_chunk.Initialize(allocator, parse_types);
	}
	scratch_chunk.Initialize(allocator, vector<LogicalType> {LogicalType::VARCHAR});
	decoders.resize(global.columns.size());
	for (idx_t i = 0; i < global.columns.size(); i++) {
		auto &column = global.columns[i];
		if (NeedsFlushCast(column)) {
			decoders[i] = MakeValueDecoder(global.context, column.type, options.rendering);
		}
	}
	if (global.copy) {
		copy_defaulted.resize(global.columns.size());
		copy_defaults.resize(global.columns.size());
		auto &defaults = global.copy->defaults;
		for (idx_t i = 0; i < global.columns.size(); i++) {
			const auto file_column = global.columns[i].file_column;
			if (file_column < defaults.size() && defaults[file_column]) {
				copy_defaults[i] = make_uniq<ExpressionExecutor>(global.context, *defaults[file_column]);
			}
		}
	}
}

void CsvScan::Start(idx_t range_p) {
	range = range_p;
	done = false;
	started = false;
	reported = false;
	counting = false;
	chunk_open = false;
	chunk_rows = 0;
	held = nullptr;
	lean_rows = DConstants::INVALID_INDEX;
	base = 0;
	copy_ended = false;
}

bool CsvScan::ScanLean(DataChunk &output, idx_t max_rows) {
	while (true) {
		if (counting) {
			if (lean_rows == 0) {
				return false;
			}
			const idx_t count = MinValue(lean_rows, max_rows);
			lean_rows -= count;
			output.Reset();
			for (auto &column : output.data) {
				column.SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(column, true);
			}
			output.SetChildCardinality(count);
			return true;
		}
		if (!global.lean_batches) {
			const auto result = reader.Advance(max_rows);
			if (result == RowReadResult::COUNTED) {
				counting = true;
				lean_rows = reader.CountedRows();
				continue;
			}
			if (result == RowReadResult::DONE) {
				return false;
			}
			const idx_t count = MinValue(reader.Ready(), max_rows);
			const idx_t rows = global.copy && global.copy->Text() ? CopyEndMarker(count) : count;
			if (rows > 0) {
				DecodeLean(rows, output);
			} else {
				output.Reset();
			}
			reader.Emit(count);
			if (output.size() > 0) {
				return true;
			}
			continue;
		}
		if (!chunk_open) {
			output.Reset();
			chunk_rows = 0;
			held = nullptr;
			chunk_open = true;
		}
		const auto result = reader.Advance(chunk_rows + 1);
		if (result == RowReadResult::ROWS) {
			const idx_t limit = MinValue(reader.Ready(), max_rows);
			DecodeLeanBatch(chunk_rows, limit - chunk_rows, output);
			chunk_rows = limit;
			reader.Hold(chunk_rows);
			if (chunk_rows < max_rows) {
				continue;
			}
		} else if (chunk_rows == 0) {
			chunk_open = false;
			if (result == RowReadResult::COUNTED) {
				counting = true;
				lean_rows = reader.CountedRows();
				continue;
			}
			return false;
		}
		LeanRowNumbers(chunk_rows, output);
		if (!global.filters.empty()) {
			for (idx_t row = 0; row < chunk_rows; row++) {
				selected[row] = true;
			}
			Filter(chunk_rows, output);
		}
		FinishLeanChunk(chunk_rows, output);
		reader.Emit(chunk_rows);
		chunk_open = false;
		if (output.size() > 0) {
			return true;
		}
	}
}

idx_t CsvScan::Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output) {
	D_ASSERT(offsets.size() <= STANDARD_VECTOR_SIZE);
	auto arena = make_shared_ptr<RowBytes>();
	idx_t capacity = 0;
	idx_t window_begin = 0;
	idx_t window_end = 0;
	idx_t window_offset = 0;
	bool window_eof = false;
	auto read = [&](idx_t from, idx_t want) {
		if (arena->size + want > capacity) {
			capacity = MaxValue(arena->size + want, capacity * 2);
			auto grown = make_unsafe_uniq_array_uninitialized<char>(capacity);
			if (arena->size) {
				memcpy(grown.get(), arena->owned.get(), arena->size);
			}
			arena->owned = std::move(grown);
			arena->data = arena->owned.get();
		}
		const idx_t got = global.ranges->Read(arena->owned.get() + arena->size, want, from);
		arena->size += got;
		window_end = from + got;
		window_eof = got < want;
	};
	fields.Clear();
	lookup_offsets = offsets;
	lookup_rows.clear();
	RowCursor merged;
	bool unfinished = false;
	const auto &lexer = *global.lean;
	for (idx_t i = 0; i < offsets.size(); i++) {
		const auto offset = static_cast<idx_t>(offsets[i]);
		if (offsets[i] < 0 || offset < global.start) {
			continue;
		}
		const idx_t first = offset > global.start ? offset - 1 : offset;
		if (first < window_begin || offset >= window_end) {
			if (window_end > window_begin && !window_eof && first >= window_end && first - window_end < LOOKUP_BYTES) {
				read(window_end, offset + LOOKUP_BYTES - window_end);
			} else {
				window_offset = arena->size;
				window_begin = first;
				read(first, offset + LOOKUP_BYTES - first);
			}
			if (offset >= window_end) {
				continue;
			}
		}
		const idx_t position = window_offset + (offset - window_begin);
		const bool file_start = offset == global.start && global.skip_rows == 0;
		const bool after_cr = offset > global.start && grammar.carry_on_newline && arena->data[position - 1] == '\r';
		RowCursor probe;
		lexer.Start(probe, position, true, false);
		probe.file_start = file_start;
		probe.after_cr = after_cr;
		idx_t stop;
		idx_t grow = LOOKUP_BYTES;
		while (true) {
			const idx_t arena_end = window_offset + (window_end - window_begin);
			lexer.Feed(probe, arena->data, 0, arena_end, window_eof, nullptr);
			if (probe.first_term != DConstants::INVALID_INDEX) {
				stop = probe.first_term + 1;
				if (grammar.carry_on_newline && arena->data[probe.first_term] == '\r' && stop < arena_end &&
				    arena->data[stop] == '\n') {
					stop++;
				}
				break;
			}
			if (window_eof) {
				stop = arena_end;
				break;
			}
			read(window_end, grow);
			grow *= 2;
		}
		const bool eof = window_eof && stop == window_offset + (window_end - window_begin);
		RowCursor row_cursor;
		lexer.Start(row_cursor, position, true);
		row_cursor.anchor = 0;
		row_cursor.file_start = file_start;
		row_cursor.after_cr = after_cr;
		const idx_t rows_before = fields.row_count;
		CsvRangeEntry entry;
		if (!lexer.Feed(row_cursor, arena->data, 0, stop, true, &fields) ||
		    !lexer.Finish(row_cursor, arena->data, 0, stop, eof, &fields, CSVState::RECORD_SEPARATOR, entry)) {
			throw InternalException("CSV row lexer refused the row at byte %llu of \"%s\"", offset, source.path);
		}
		merged.quotes = merged.quotes || row_cursor.quotes;
		merged.doubled = merged.doubled || row_cursor.doubled;
		merged.padded = merged.padded || row_cursor.padded;
		merged.non_ascii |= row_cursor.non_ascii;
		merged.long_rows = merged.long_rows || row_cursor.long_rows;
		merged.unstrict = merged.unstrict || row_cursor.unstrict;
		merged.extras_dropped = merged.extras_dropped || row_cursor.extras_dropped;
		unfinished = unfinished || row_cursor.unfinished;
		for (idx_t row = rows_before; row < fields.row_count; row++) {
			lookup_rows.push_back(i);
		}
	}
	const idx_t count = fields.row_count;
	merged.unfinished = unfinished;
	reader.Adopt(arena, merged);
	idx_t found = 0;
	if (count > 0) {
		DecodeLean(count, output);
		const bool filters = !global.filters.empty();
		for (idx_t row = 0; row < count; row++) {
			if ((!filters || selected[row]) && !failed[row]) {
				survivors[found++] = lookup_rows[row];
			}
		}
	}
	output.SetCardinality(found);
	lookup_offsets = {};
	bytes.reset();
	fields.Clear();
	return found;
}

bool CsvScan::IsNull(const CsvField &field, const char *value, idx_t size) const {
	const bool quoted = field.flags & CSV_FIELD_QUOTED;
	if (quoted && !options.allow_quoted_nulls) {
		return false;
	}
	const bool escaped_null = !grammar.strict_mode && (field.flags & CSV_FIELD_ESCAPED) && !quoted && size == 1;
	for (auto &null_string : global.null_strings) {
		if (null_string.size() == 2 && null_string[0] == grammar.escape) {
			if (escaped_null && null_string[1] == value[0]) {
				return true;
			}
		} else if (size == null_string.size() && !escaped_null && memcmp(null_string.c_str(), value, size) == 0) {
			return true;
		}
	}
	return false;
}

bool CsvScan::DecodeValue(const CsvScanColumn &column, const CsvField &field, Vector &vector, idx_t row,
                          CsvRowError &error) {
	const char *value = bytes->data + field.offset;
	idx_t size = field.size;
	const auto parse_id = column.parse_type.id();
	string_t unescaped;
	if (field.flags & CSV_FIELD_ESCAPED) {
		if (parse_id != LogicalTypeId::VARCHAR) {
			error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start,
			         global.ignore_errors ? string() : CastMessage(value, size, LogicalTypeIdToString(parse_id))};
			return false;
		}
		unescaped = CsvRemoveEscape(value, size, grammar.escape, grammar.quote, grammar.strict_mode, vector);
		value = unescaped.GetData();
		size = unescaped.GetSize();
	}
	if (IsNull(field, value, size)) {
		if (column.force_not_null) {
			if (parse_id != LogicalTypeId::VARCHAR) {
				error = {CSVErrorType::CAST_ERROR, column.file_column, base + field.start, string()};
				return false;
			}
			FlatVector::GetDataMutable<string_t>(vector)[row] = string_t();
		} else {
			FlatVector::SetNull(vector, row, true);
		}
		return true;
	}
	return ParseField(column, value, size, field.flags & CSV_FIELD_ESCAPED, base + field.start, vector, row, error);
}

std::string_view CsvScan::RawField(idx_t file_column, idx_t row) const {
	const idx_t index = row * global.num_columns + file_column;
	const idx_t shift = cursor.anchor - bytes->offset;
	const idx_t begin =
	    (file_column == 0 ? fields.rows[row] : (fields.ends[index - 1] & ROW_FIELD_MASK) + grammar.delimiter_size) +
	    shift;
	const idx_t end = (fields.ends[index] & ROW_FIELD_MASK) + shift;
	return {bytes->data + begin, end - begin};
}

bool CsvScan::DecodeCopyValue(const CsvScanColumn &column, idx_t row, Vector &vector, CsvRowError &error) {
	const auto raw = RawField(column.file_column, row);
	const idx_t position = static_cast<idx_t>(raw.data() - bytes->data) + bytes->offset;
	copy_buffer.resize(raw.size());
	std::string_view value = raw;
	CopyFieldKind kind;
	try {
		kind = ReadCopyField(*global.copy, column.file_column, raw, copy_buffer.data(), value);
	} catch (const Exception &ex) {
		const ErrorData data(ex);
		error = {CSVErrorType::INVALID_STATE, column.file_column, position, data.RawMessage()};
		error.sqlstate = data.ExtraInfo().at("sqlstate");
		error.value = CopyPrintout(raw);
		error.has_value = true;
		error.line_error = true;
		return false;
	}
	if (kind == CopyFieldKind::NULL_VALUE) {
		FlatVector::SetNull(vector, row, true);
		return true;
	}
	if (kind == CopyFieldKind::DEFAULT_VALUE) {
		const auto column_idx = static_cast<idx_t>(&column - global.columns.data());
		FlatVector::SetNull(vector, row, true);
		if (!copy_defaults[column_idx]) {
			error = {CSVErrorType::INVALID_STATE, column.file_column, position,
			         "unexpected default marker in COPY data"};
			error.sqlstate = "22P04";
			error.detail = "Column \"" + global.copy->names[column.file_column] + "\" has no default value.";
			error.line_value = true;
			return false;
		}
		copy_defaulted[column_idx].push_back(row);
		return true;
	}
	return ParseField(column, value.data(), value.size(), value.data() != raw.data(), position, vector, row, error);
}

bool CsvScan::ParseField(const CsvScanColumn &column, const char *value, idx_t size, bool owned, idx_t position,
                         Vector &vector, idx_t row, CsvRowError &error) {
	const auto parse_id = column.parse_type.id();
	string thousands;
	if ((LogicalType::IsNumeric(parse_id) || parse_id == LogicalTypeId::BIGNUM) &&
	    options.thousands_separator != '\0') {
		thousands = CsvStripSeparator(value, size, options.thousands_separator);
		value = thousands.c_str();
		size = thousands.size();
	}
	bool success = true;
	if (parse_id == LogicalTypeId::BIGNUM) {
		while (size > 0 && StringUtil::CharacterIsSpace(*value)) {
			value++;
			size--;
		}
		while (size > 0 && StringUtil::CharacterIsSpace(value[size - 1])) {
			size--;
		}
		try {
			auto bignum = Bignum::VarcharToBignum(string_t(value, NumericCast<uint32_t>(size)));
			FlatVector::GetDataMutable<string_t>(vector)[row] =
			    StringVector::AddStringOrBlob(vector, bignum.data(), bignum.size());
		} catch (const ConversionException &) {
			success = false;
		}
	} else if (parse_id != LogicalTypeId::VARCHAR) {
		const ValueParseContext context {&global.date_format, &global.timestamp_format, value + size,
		                                 global.decimal_separator};
		success = WithValueParser(column.parse_type, context, [&](auto parsed, auto parse) {
			using T = typename decltype(parsed)::type;
			return parse(value, size, FlatVector::GetDataMutable<T>(vector)[row]);
		});
	} else {
		auto &out = FlatVector::GetDataMutable<string_t>(vector)[row];
		if (column.validate_utf8 && !Utf8Proc::IsValid(value, UnsafeNumericCast<uint32_t>(size))) {
			out = string_t();
			error = {CSVErrorType::INVALID_ENCODING, column.file_column, position, string()};
			if (owned) {
				error.value = string(value, size);
			}
			return false;
		}
		if (owned || !thousands.empty()) {
			out = StringVector::AddStringOrBlob(vector, value, size);
		} else {
			out = string_t(value, UnsafeNumericCast<uint32_t>(size));
		}
	}
	if (!success) {
		error = {CSVErrorType::CAST_ERROR, column.file_column, position,
		         global.ignore_errors ? string() : CastMessage(value, size, LogicalTypeIdToString(parse_id))};
		return false;
	}
	return true;
}

string CsvScan::LineText(const CsvRow &row, bool &first_nl) const {
	if (row.begin < base) {
		first_nl = true;
	}
	if (!global.print_line || row.begin == row.end || row.end <= base) {
		return string();
	}
	const char *data = bytes->data;
	idx_t start = 0;
	if (row.begin >= base) {
		start = row.begin - base;
		first_nl = data[start] == '\n' || data[start] == '\r';
		start += first_nl;
	}
	const idx_t stop = MinValue(row.end - base, bytes->size);
	if (start >= stop) {
		return string();
	}
	string line(data + start, stop - start);
	CsvSanitize(line);
	return line;
}

CSVError CsvScan::CopyRowError(const CsvRow &row, const CsvRowError &error) {
	auto &copy = *global.copy;
	CSVError result(error.message, error.type, LinesPerBoundary(range, LeanLinesAt(row.end)));
	result.sqlstate = error.sqlstate;
	result.detail = error.detail;
	result.hint = error.hint;
	result.copy_table = copy.table;
	bool line_value = error.line_value;
	switch (error.type) {
	case CSVErrorType::TOO_MANY_COLUMNS:
		result.error_message = "extra data after last expected column";
		result.sqlstate = "22P04";
		line_value = true;
		break;
	case CSVErrorType::TOO_FEW_COLUMNS:
		result.error_message = "missing data for column \"" + copy.names[error.column + 1] + "\"";
		result.sqlstate = "22P04";
		line_value = true;
		break;
	case CSVErrorType::UNTERMINATED_QUOTES:
		result.error_message = "unterminated CSV quoted field";
		result.sqlstate = "22P04";
		line_value = true;
		break;
	case CSVErrorType::INVALID_ENCODING: {
		const idx_t at = error.position - bytes->offset;
		result.error_message = CopyInvalidUtf8(
		    error.value.empty() ? std::string_view(bytes->data + at, bytes->size - at) : std::string_view(error.value));
		result.sqlstate = "22021";
		break;
	}
	case CSVErrorType::CAST_ERROR:
		if (result.sqlstate.empty()) {
			result.sqlstate = "22P02";
		}
		result.copy_column = copy.names[error.column];
		result.copy_has_value = true;
		result.copy_value = error.value;
		break;
	default:
		if (result.sqlstate.empty()) {
			result.error_message = "unterminated CSV quoted field";
			result.sqlstate = "22P04";
			line_value = true;
			break;
		}
		if (error.has_value) {
			result.copy_column = copy.names[error.column];
			result.copy_has_value = true;
			result.copy_value = error.value;
		}
		break;
	}
	if (line_value) {
		const idx_t begin = row.start - bytes->offset;
		const idx_t end = MinValue(row.end - bytes->offset, bytes->size);
		result.copy_has_value = true;
		result.copy_value = CopyPrintout(std::string_view(bytes->data + begin, end > begin ? end - begin : 0));
	}
	return result;
}

void CsvScan::ReportRow(idx_t row_idx, const CsvRow &row, vector<CsvRowError> &errors) {
	if (global.copy) {
		const auto rank = [](const CsvRowError &error) {
			switch (error.type) {
			case CSVErrorType::INVALID_ENCODING:
				return 0;
			case CSVErrorType::UNTERMINATED_QUOTES:
				return 1;
			case CSVErrorType::TOO_MANY_COLUMNS:
			case CSVErrorType::TOO_FEW_COLUMNS:
				return 2;
			case CSVErrorType::CAST_ERROR:
				return 4;
			default:
				return error.sqlstate.empty() || error.line_error ? 1 : 3;
			}
		};
		auto chosen = &errors[0];
		for (auto &error : errors) {
			if (rank(error) < rank(*chosen)) {
				chosen = &error;
			}
		}
		auto csv_error = CopyRowError(row, *chosen);
		Report(row_idx, csv_error, false);
		return;
	}
	if (source.sniffing) {
		for (auto &error : errors) {
			if (error.type != CSVErrorType::INVALID_ENCODING) {
				continue;
			}
			if (options.ignore_errors.GetValue()) {
				return;
			}
			auto encoding = std::move(error);
			errors.clear();
			errors.push_back(std::move(encoding));
			break;
		}
	}
	bool first_nl = false;
	auto line = LineText(row, first_nl);
	const LinesPerBoundary lines(range, row.lines);
	const auto row_position = row.begin + first_nl;
	const auto &names = source.names;
	const String path(source.path);
	for (auto &error : errors) {
		const optional_idx error_position(error.position == row.begin ? error.position + first_nl : error.position);
		CSVError csv_error;
		switch (error.type) {
		case CSVErrorType::TOO_MANY_COLUMNS:
		case CSVErrorType::TOO_FEW_COLUMNS:
			csv_error = CSVError::IncorrectColumnAmountError(options, error.column, lines, line, row_position,
			                                                 error_position, path);
			break;
		case CSVErrorType::INVALID_ENCODING:
			csv_error = CSVError::InvalidUTF8(options, error.column, lines, line, row_position, error_position, path);
			break;
		case CSVErrorType::UNTERMINATED_QUOTES:
			csv_error = CSVError::UnterminatedQuotesError(options, error.column, lines, line, row_position,
			                                              error_position, path);
			break;
		case CSVErrorType::CAST_ERROR: {
			string column_name;
			LogicalTypeId type_id = LogicalTypeId::INVALID;
			if (error.column < names.size()) {
				column_name = names[error.column];
			}
			for (auto &column : global.columns) {
				if (column.file_column == error.column) {
					type_id = column.parse_type.id();
				}
			}
			csv_error = CSVError::CastError(options, column_name, error.message, error.column, line, lines,
			                                row_position, error_position, type_id, path);
			break;
		}
		case CSVErrorType::MAXIMUM_LINE_SIZE:
			csv_error = CSVError::LineSizeError(options, lines, line, row_position, path);
			break;
		case CSVErrorType::INVALID_STATE:
			csv_error = CSVError::InvalidState(options, error.column, lines, line, row_position, error_position, path);
			break;
		default:
			throw InvalidInputException("CSV Error not allowed when inserting row");
		}
		const bool force = error.type == CSVErrorType::INVALID_STATE ||
		                   (source.sniffing && error.type == CSVErrorType::INVALID_ENCODING);
		Report(row_idx, csv_error, force);
	}
}

void CsvScan::Report(idx_t row, CSVError &error, bool force) {
	if (lookup_offsets.empty()) {
		source.error_handler->Error(error, force);
		return;
	}
	throw InvalidInputException("Could not read the CSV row at byte %llu of \"%s\": %s",
	                            lookup_offsets[lookup_rows[row]], source.path, error.error_message);
}

bool CsvScan::NeedsFlushCast(const CsvScanColumn &column) {
	auto &type = column.type;
	return type.HasAlias() || (type.id() != LogicalTypeId::VARCHAR && column.parse_type.id() == LogicalTypeId::VARCHAR);
}

bool CsvScan::EvaluatesOnFieldBytes(const Expression &expr) {
	vector<CsvRawPredicate> predicates;
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_NOT) {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		return children.size() == 1 && CollectRawPattern(*children[0], true, predicates);
	}
	return CollectRawPattern(expr, false, predicates);
}

void CsvScan::FlushCasts(idx_t count, DataChunk &output, bool late, bool filter_pass) {
	const auto &names = source.names;
	const String path(source.path);
	for (idx_t i = 0; i < global.columns.size(); i++) {
		auto &column = global.columns[i];
		if (!NeedsFlushCast(column) || column.output_column >= output.ColumnCount()) {
			continue;
		}
		if (late && global.filtered[i] != filter_pass) {
			continue;
		}
		auto &parse = parse_chunk.data[i];
		auto &result = output.data[column.output_column];
		ErrorData decode_error;
		if (decoders[i]->Decode(parse, result, count, decode_error)) {
			continue;
		}
		auto &result_validity = FlatVector::Validity(result);
		auto &parse_validity = FlatVector::Validity(parse);
		bool first = true;
		idx_t lines_now = DConstants::INVALID_INDEX;
		for (idx_t row = 0; row < count; row++) {
			if (result_validity.RowIsValid(row) || !parse_validity.RowIsValid(row)) {
				continue;
			}
			if (global.copy) {
				if (!failed[row]) {
					const auto value = FlatVector::GetData<string_t>(parse)[row];
					CsvRowError row_error {CSVErrorType::CAST_ERROR, column.file_column, 0, decode_error.RawMessage()};
					auto &extra_info = decode_error.ExtraInfo();
					const auto extra = [&](const char *key) {
						const auto entry = extra_info.find(key);
						return entry == extra_info.end() ? string() : entry->second;
					};
					row_error.sqlstate = extra("sqlstate");
					row_error.detail = extra("detail");
					row_error.hint = extra("hint");
					row_error.value = CopyPrintout(std::string_view(value.GetData(), value.GetSize()));
					row_error.has_value = true;
					auto csv_error = CopyRowError(LeanRow(row), row_error);
					Report(row, csv_error, false);
				}
				failed[row] = true;
				rows_failed = true;
				continue;
			}
			failed[row] = true;
			rows_failed = true;
			if (!global.ignore_errors) {
				if (lines_now == DConstants::INVALID_INDEX) {
					lines_now = LeanLinesAt(LeanRow(count - 1).end + 1);
				}
				const auto csv_row = LeanRow(row);
				bool first_nl = false;
				auto line = LineText(csv_row, first_nl);
				auto value = FlatVector::GetData<string_t>(parse)[row];
				auto message = CastMessage(value.GetData(), value.GetSize(),
				                           first ? column.type.ToString() : LogicalTypeIdToString(column.type.id()));
				idx_t row_position = csv_row.begin + first_nl;
				if (first && csv_row.begin == csv_row.end) {
					row_position = 0;
				}
				const LinesPerBoundary lines(range, lines_now - count + row);
				auto csv_error =
				    CSVError::CastError(options, names[column.file_column], message, column.file_column, line, lines,
				                        row_position, optional_idx::Invalid(), column.type.id(), path);
				Report(row, csv_error, false);
			}
			first = false;
		}
	}
}

Vector &CsvScan::Target(idx_t column, DataChunk &output) {
	auto &scan_column = global.columns[column];
	if (NeedsFlushCast(scan_column)) {
		return parse_chunk.data[column];
	}
	if (scan_column.output_column < output.ColumnCount()) {
		return output.data[scan_column.output_column];
	}
	return scratch_chunk.data[0];
}

void CsvScan::Filter(idx_t count, DataChunk &output) {
	idx_t approved = 0;
	for (idx_t row = 0; row < count; row++) {
		if (selected[row]) {
			filter_candidates.set_index(approved++, row);
		}
	}
	filter_selection.Initialize(filter_candidates);
	for (idx_t i = 0; i < global.filters.size() && approved > 0; i++) {
		auto &vector = output.data[global.filters[i].output_column];
		FlatVector::SetSize(vector, count_t(count));
		ColumnSegment::FilterSelection(filter_selection, vector, *filter_states[i], count, approved);
	}
	for (idx_t row = 0; row < count; row++) {
		selected[row] = false;
	}
	for (idx_t i = 0; i < approved; i++) {
		selected[filter_selection.get_index(i)] = true;
	}
}

LeanSource CsvScan::LeanSourceFor(const CsvScanColumn &column, idx_t first, bool selected_only) const {
	const idx_t columns = global.num_columns;
	LeanSource source;
	source.first = first;
	if (column.file_column == 0) {
		source.begins = fields.rows.get();
		source.begin_stride = 1;
		source.begin_bias = 0;
	} else {
		source.begins = fields.ends.get() + column.file_column - 1;
		source.begin_stride = columns;
		source.begin_bias = grammar.delimiter_size;
	}
	source.ends = fields.ends.get() + column.file_column;
	source.stride = columns;
	source.data = bytes->data;
	source.limit = bytes->data + bytes->size;
	source.shift = cursor.anchor - bytes->offset;
	source.quote = grammar.quote;
	source.escape_probe = grammar.escape && grammar.escape != grammar.quote ? grammar.escape : grammar.quote;
	source.unquoted_escape = grammar.unquoted_escape ? grammar.escape : '\0';
	source.quotes = cursor.quotes;
	source.doubled = cursor.doubled;
	source.padded = cursor.padded;
	source.spaced = global.lean->Dialect().quoted_spaces;
	source.pad_empty = column.force_not_null;
	source.allow_quoted_nulls = options.allow_quoted_nulls;
	source.empty_only_null = global.empty_only_null;
	source.null_strings = &global.null_strings;
	source.selected = selected_only ? &selected : nullptr;
	if (global.copy) {
		auto &copy_options = global.copy->options;
		source.quotes = false;
		source.doubled = false;
		source.unquoted_escape = '\0';
		source.spaced = false;
		source.pad_empty = false;
		source.retry_flagged = true;
		source.allow_quoted_nulls = false;
		source.empty_only_null = global.empty_only_null && column.match_null;
		source.null_strings = column.match_null ? &global.null_strings : &global.no_null_strings;
		source.default_marker = copy_options.has_default ? &copy_options.default_str : nullptr;
	}
	return source;
}

bool CsvScan::LeanRawFilter(idx_t count) {
	bool applied = false;
	for (auto &filter : global.filters) {
		if (filter.raw.empty()) {
			continue;
		}
		applied = true;
		LeanRawPredicates(LeanSourceFor(global.columns[filter.column], 0, false), count, filter.raw, selected);
	}
	return applied;
}

bool CsvScan::DecodeLeanColumn(idx_t column_idx, idx_t first, idx_t count, DataChunk &output, bool selected_only,
                               bool utf8_valid, bool hold) {
	auto &column = global.columns[column_idx];
	auto &vector = Target(column_idx, output);
	const auto source = LeanSourceFor(column, first, selected_only);
	const bool force = column.force_not_null;
	retry.clear();
	if (column.parse_type.id() == LogicalTypeId::VARCHAR) {
		if (hold) {
			StringVector::AddAuxiliaryData(vector, make_uniq<RowBytesHolder>(bytes));
		}
		LeanStrings(source, count, column, column.validate_utf8 && !utf8_valid, grammar, vector, retry);
	} else if (!LeanDecodes(column.parse_type) || options.thousands_separator != '\0') {
		const idx_t columns = global.num_columns;
		for (idx_t row = first; row < first + count; row++) {
			if (selected_only && !selected[row]) {
				FlatVector::SetNull(vector, row, true);
				continue;
			}
			const idx_t index = row * columns + column.file_column;
			const uint32_t end_bits = fields.ends[index];
			const idx_t begin = column.file_column == 0
			                        ? fields.rows[row]
			                        : (fields.ends[index - 1] & ROW_FIELD_MASK) + grammar.delimiter_size;
			if ((end_bits & ROW_FIELD_FLAG) && begin > (end_bits & ROW_FIELD_MASK)) {
				FlatVector::SetNull(vector, row, true);
				continue;
			}
			retry.push_back(row);
		}
	} else {
		const ValueParseContext context {&global.date_format, &global.timestamp_format, source.limit,
		                                 global.decimal_separator};
		WithValueParser(column.parse_type, context, [&](auto parsed, auto parse) {
			using T = typename decltype(parsed)::type;
			LeanValues<T>(source, count, force, vector, parse, retry);
			return true;
		});
	}
	if (!retry.empty()) {
		LeanRetry(column, vector);
	}
	return true;
}

CsvField CsvScan::LeanField(const CsvScanColumn &column, idx_t row) const {
	const idx_t columns = global.num_columns;
	const idx_t shift = cursor.anchor - bytes->offset;
	const idx_t index = row * columns + column.file_column;
	const uint32_t end_bits = fields.ends[index];
	idx_t begin =
	    column.file_column == 0 ? fields.rows[row] : (fields.ends[index - 1] & ROW_FIELD_MASK) + grammar.delimiter_size;
	begin += shift;
	idx_t end = (end_bits & ROW_FIELD_MASK) + shift;
	CsvField field {static_cast<uint32_t>(begin), 0, 0, 0};
	if ((end_bits & ROW_FIELD_FLAG) && cursor.quotes &&
	    LeanUnquote(bytes->data, grammar.quote, global.lean->Dialect().quoted_spaces, begin, end)) {
		field.flags |= CSV_FIELD_QUOTED;
		const char probe = grammar.escape && grammar.escape != grammar.quote ? grammar.escape : grammar.quote;
		if (cursor.doubled && memchr(bytes->data + begin, probe, end - begin)) {
			field.flags |= CSV_FIELD_ESCAPED;
		}
	} else if ((end_bits & ROW_FIELD_FLAG) && grammar.unquoted_escape && end > begin &&
	           memchr(bytes->data + begin, grammar.escape, end - begin)) {
		field.flags |= CSV_FIELD_ESCAPED;
	}
	field.offset = static_cast<uint32_t>(begin);
	field.size = static_cast<uint32_t>(end - begin);
	return field;
}

void CsvScan::LeanRetry(const CsvScanColumn &column, Vector &vector) {
	base = bytes->offset;
	auto &validity = FlatVector::ValidityMutable(vector);
	for (const auto row : retry) {
		validity.SetValid(row);
		CsvRowError error;
		const bool decoded = global.copy ? DecodeCopyValue(column, row, vector, error)
		                                 : DecodeValue(column, LeanField(column, row), vector, row, error);
		if (!decoded) {
			FlatVector::SetNull(vector, row, true);
			value_errors[row].push_back(std::move(error));
			lean_errors = true;
		}
	}
}

CsvRow CsvScan::LeanRow(idx_t row) const {
	CsvRow result;
	result.start = cursor.anchor + fields.rows[row];
	result.end = reader.RowEnd(row);
	result.begin = reader.RowBegin(row);
	if (result.begin == DConstants::INVALID_INDEX || result.begin >= result.start) {
		result.begin = result.start > 0 ? result.start - 1 : 0;
	} else if (grammar.comment) {
		idx_t content = result.start - 1;
		while (content > result.begin && content >= bytes->offset &&
		       (bytes->data[content - bytes->offset] == '\n' || bytes->data[content - bytes->offset] == '\r')) {
			content--;
		}
		if (content > result.begin && content >= bytes->offset) {
			result.begin = content + 1;
		}
	}
	const bool crlf = grammar.carry_on_newline && result.end >= bytes->offset &&
	                  result.end < bytes->offset + bytes->size && bytes->data[result.end - bytes->offset] == '\r';
	result.last_position = result.end + 1 + crlf;
	result.lines = 0;
	return result;
}

bool CsvScan::LeanNullExtra(idx_t begin, idx_t end, uint32_t end_bits) const {
	const idx_t shift = cursor.anchor - bytes->offset;
	begin += shift;
	end += shift;
	bool quoted = false;
	if ((end_bits & ROW_FIELD_FLAG) && cursor.quotes) {
		quoted = LeanUnquote(bytes->data, grammar.quote, global.lean->Dialect().quoted_spaces, begin, end);
	}
	if (quoted && !options.allow_quoted_nulls) {
		return false;
	}
	for (auto &null_string : global.null_strings) {
		if (end - begin == null_string.size() && memcmp(null_string.c_str(), bytes->data + begin, end - begin) == 0) {
			return true;
		}
	}
	return false;
}

idx_t CsvScan::LeanLinesAt(idx_t position) {
	if (!lookup_offsets.empty()) {
		return 0;
	}
	return reader.LinesAt(position, range == 0 ? global.start_lines : 0);
}

idx_t CsvScan::CopyEndMarker(idx_t count) {
	const idx_t shift = cursor.anchor - bytes->offset;
	idx_t rows = copy_ended ? 0 : count;
	bool data = false;
	for (idx_t row = 0; row < count; row++) {
		const bool empty = reader.RowEnd(row) - bytes->offset == fields.rows[row] + shift;
		if (copy_ended) {
			if (!empty) {
				ThrowCopyError("22P04", "end-of-copy marker is followed by data");
			}
			continue;
		}
		data = data || !empty;
		if (RawField(0, row) != "\\.") {
			continue;
		}
		const auto *anomaly = fields.anomalies.empty() ? nullptr : fields.Anomaly(row);
		if ((anomaly ? anomaly->fields : global.num_columns) != 1) {
			continue;
		}
		copy_ended = true;
		rows = row;
		auto marker = global.copy_marker_range.load();
		while (range < marker && !global.copy_marker_range.compare_exchange_weak(marker, range)) {
		}
		if (global.copy_data_range.load() > range) {
			ThrowCopyError("22P04", "end-of-copy marker is followed by data");
		}
	}
	if (data) {
		auto latest = global.copy_data_range.load();
		while (latest < range && !global.copy_data_range.compare_exchange_weak(latest, range)) {
		}
		if (global.copy_marker_range.load() < range) {
			ThrowCopyError("22P04", "end-of-copy marker is followed by data");
		}
	}
	return rows;
}

void CsvScan::ApplyCopyDefaults(DataChunk &output) {
	for (idx_t i = 0; i < copy_defaulted.size(); i++) {
		auto &rows = copy_defaulted[i];
		if (rows.empty()) {
			continue;
		}
		auto &column = global.columns[i];
		if (column.output_column < output.ColumnCount()) {
			auto &target = output.data[column.output_column];
			DataChunk input;
			input.SetCardinality(rows.size());
			Vector values(target.GetType(), rows.size());
			copy_defaults[i]->ExecuteExpression(input, values);
			for (idx_t j = 0; j < rows.size(); j++) {
				VectorOperations::Copy(values, target, j + 1, j, rows[j]);
			}
		}
		rows.clear();
	}
}

void CsvScan::ClearRowState(idx_t first, idx_t count) {
	for (idx_t row = first; row < first + count; row++) {
		failed[row] = false;
	}
	for (auto &rows : copy_defaulted) {
		rows.clear();
	}
	lean_errors = false;
	reader.ResetLines();
	base = bytes->offset;
}

void CsvScan::LeanReport(idx_t first, idx_t count) {
	if (cursor.extras_dropped || cursor.unstrict) {
		used_unstrictness = true;
	}
	const idx_t end_row = first + count;
	auto anomaly = std::lower_bound(fields.anomalies.begin(), fields.anomalies.end(), first,
	                                [](const RowAnomaly &entry, idx_t row) { return entry.row < row; });
	auto event = std::lower_bound(fields.events.cbegin(), fields.events.cend(), first,
	                              [](const RowEvent &entry, idx_t row) { return entry.row < row; });
	const idx_t unfinished_row =
	    cursor.unfinished && reader.Finished() ? fields.row_count - 1 : DConstants::INVALID_INDEX;
	const bool anomalies = anomaly != fields.anomalies.end() && anomaly->row < end_row;
	const bool events = event != fields.events.cend() && event->row < end_row;
	if (!lean_errors && !anomalies && !events && !cursor.long_rows) {
		return;
	}
	lean_errors = false;
	const idx_t max_line_size = options.maximum_line_size.GetValue();
	for (idx_t row = first; row < end_row; row++) {
		auto &values = value_errors[row];
		const RowAnomaly *row_anomaly = nullptr;
		if (anomaly != fields.anomalies.end() && anomaly->row == row) {
			row_anomaly = &*anomaly;
			++anomaly;
		}
		const bool row_events = event != fields.events.cend() && event->row == row;
		const bool unfinished = row == unfinished_row;
		bool long_row = false;
		if (cursor.long_rows && !unfinished) {
			const auto csv_row = LeanRow(row);
			long_row = csv_row.end - csv_row.begin > max_line_size;
		}
		if (values.empty() && !row_anomaly && !row_events && !long_row) {
			continue;
		}
		row_errors.clear();
		LeanRowErrors(row, row_anomaly, event, unfinished, long_row, values, row_errors);
		values.clear();
		if (row_errors.empty()) {
			continue;
		}
		failed[row] = true;
		rows_failed = true;
		if (!global.ignore_errors || global.copy) {
			auto csv_row = LeanRow(row);
			csv_row.lines = LeanLinesAt(csv_row.start);
			ReportRow(row, csv_row, row_errors);
		}
	}
}

CSVErrorType CsvScan::EventType(const RowEvent &event) const {
	const idx_t position = cursor.anchor + event.position;
	const auto at = [&](idx_t offset) {
		return bytes->data[offset - bytes->offset];
	};
	const idx_t limit = bytes->offset + bytes->size;
	const bool quoted = grammar.quote && position < limit &&
	                    (at(position) == grammar.quote || (grammar.empty_space && at(position) == ' ' &&
	                                                       position + 1 < limit && at(position + 1) == grammar.quote));
	return quoted ? CSVErrorType::UNTERMINATED_QUOTES : CSVErrorType::INVALID_STATE;
}

void CsvScan::LeanRowErrors(idx_t row, const RowAnomaly *anomaly, vector<RowEvent>::const_iterator &event,
                            bool unfinished, bool long_row, vector<CsvRowError> &values, vector<CsvRowError> &errors) {
	const idx_t columns = global.num_columns;
	const auto events_end = fields.events.cend();
	idx_t field_count = anomaly ? anomaly->fields : columns;
	if (unfinished) {
		for (auto it = event; it != events_end && it->row == row; ++it) {
			field_count = it->column;
		}
	}
	std::stable_sort(values.begin(), values.end(),
	                 [](const CsvRowError &a, const CsvRowError &b) { return a.column < b.column; });
	idx_t value = 0;
	const auto add_events_until = [&](idx_t column) {
		while (event != events_end && event->row == row && event->column <= column) {
			errors.push_back({EventType(*event), event->column, cursor.anchor + event->position, string()});
			++event;
		}
	};
	for (idx_t column = 0; column < MinValue(field_count, columns); column++) {
		add_events_until(column);
		while (value < values.size() && values[value].column == column) {
			errors.push_back(std::move(values[value++]));
		}
	}
	if (anomaly && anomaly->fields > columns) {
		idx_t begin = (fields.ends[(row + 1) * columns - 1] & ROW_FIELD_MASK) + grammar.delimiter_size;
		for (idx_t extra = 0; extra < anomaly->fields - columns; extra++) {
			add_events_until(columns + extra);
			const uint32_t end_bits = fields.extras[anomaly->extras + extra];
			const idx_t end = end_bits & ROW_FIELD_MASK;
			if (extra > 0 || !LeanNullExtra(begin, end, end_bits)) {
				errors.push_back({CSVErrorType::TOO_MANY_COLUMNS, columns + extra, cursor.anchor + begin, string()});
			}
			begin = end + grammar.delimiter_size;
		}
	}
	add_events_until(NumericLimits<idx_t>::Maximum());
	if (unfinished) {
		return;
	}
	if (long_row) {
		errors.push_back({CSVErrorType::MAXIMUM_LINE_SIZE, 1, LeanRow(row).last_position, string()});
	}
	if (anomaly && anomaly->fields < columns && !options.null_padding) {
		const idx_t last_position = LeanRow(row).last_position;
		for (idx_t column = anomaly->fields; column < columns; column++) {
			errors.push_back({CSVErrorType::TOO_FEW_COLUMNS, column - 1, last_position, string()});
		}
	}
}

void CsvScan::LeanRowNumbers(idx_t count, DataChunk &output) {
	if (source.file_row_number == DConstants::INVALID_INDEX || source.file_row_number >= output.ColumnCount()) {
		return;
	}
	auto data = FlatVector::GetDataMutable<int64_t>(output.data[source.file_row_number]);
	for (idx_t row = 0; row < count; row++) {
		data[row] = lookup_offsets.empty() ? static_cast<int64_t>(cursor.anchor + fields.rows[row])
		                                   : lookup_offsets[lookup_rows[row]];
	}
}

bool CsvScan::DecodeLean(idx_t count, DataChunk &output) {
	output.Reset();
	parse_chunk.Reset();
	scratch_chunk.Reset();
	const idx_t columns = global.num_columns;
	bool utf8_valid = !cursor.non_ascii;
	if (!utf8_valid) {
		const idx_t first = cursor.anchor + fields.rows[0];
		const idx_t last = cursor.anchor + (fields.ends[count * columns - 1] & ROW_FIELD_MASK);
		utf8_valid = simdutf::validate_utf8(bytes->data + (first - bytes->offset), last - first);
	}
	const bool late = global.late_materialization;
	for (idx_t row = 0; row < count; row++) {
		selected[row] = true;
	}
	ClearRowState(0, count);
	const bool raw = late && LeanRawFilter(count);
	for (idx_t i = 0; i < global.columns.size(); i++) {
		if ((!late || global.filtered[i]) && !DecodeLeanColumn(i, 0, count, output, raw, utf8_valid, true)) {
			return false;
		}
	}
	if (late) {
		FlushCasts(count, output, true, true);
		LeanRowNumbers(count, output);
		Filter(count, output);
		for (idx_t i = 0; i < global.columns.size(); i++) {
			if (!global.filtered[i] && !DecodeLeanColumn(i, 0, count, output, true, utf8_valid, true)) {
				return false;
			}
		}
	}
	LeanReport(0, count);
	FlushCasts(count, output, late, false);
	if (global.copy) {
		ApplyCopyDefaults(output);
	}
	if (!late) {
		LeanRowNumbers(count, output);
		if (!global.filters.empty()) {
			Filter(count, output);
		}
	}
	FinishLeanChunk(count, output);
	return true;
}

bool CsvScan::DecodeLeanBatch(idx_t first, idx_t count, DataChunk &output) {
	const idx_t columns = global.num_columns;
	bool utf8_valid = !cursor.non_ascii;
	if (!utf8_valid) {
		const idx_t from = cursor.anchor + fields.rows[first];
		const idx_t to = cursor.anchor + (fields.ends[(first + count) * columns - 1] & ROW_FIELD_MASK);
		utf8_valid = simdutf::validate_utf8(bytes->data + (from - bytes->offset), to - from);
	}
	const bool hold = bytes.get() != held;
	ClearRowState(first, count);
	for (idx_t i = 0; i < global.columns.size(); i++) {
		if (!DecodeLeanColumn(i, first, count, output, false, utf8_valid, hold)) {
			return false;
		}
	}
	held = bytes.get();
	LeanReport(first, count);
	if (global.copy) {
		ApplyCopyDefaults(output);
	}
	return true;
}

void CsvScan::FinishLeanChunk(idx_t count, DataChunk &output) {
	output.SetChildCardinality(count);
	const bool filters = !global.filters.empty();
	if (filters || rows_failed) {
		idx_t kept = 0;
		for (idx_t row = 0; row < count; row++) {
			if ((!filters || selected[row]) && !failed[row]) {
				keep.set_index(kept++, row);
			}
		}
		if (kept != count) {
			output.Slice(keep, kept);
		}
		output.SetChildCardinality(kept);
	}
	rows_failed = false;
}

bool CsvScan::Scan(DataChunk &output, idx_t max_rows) {
	source.error_handler->ErrorIfNeeded();
	if (!started) {
		started = true;
		reader.Start(range, !global.count_only, !source.sniffing, !global.columns.empty());
	}
	if (ScanLean(output, max_rows)) {
		return false;
	}
	done = true;
	output.Reset();
	if (reported) {
		return false;
	}
	reported = true;
	if (source.sniffing) {
		return true;
	}
	auto &ranges = *global.ranges;
	if (!global.row_source || !global.stream) {
		*source.bytes_read += ranges.RangeEnd(range) - ranges.RangeBegin(range);
	}
	const idx_t range_lines = reader.RangeLines();
	const idx_t lines =
	    range_lines == DConstants::INVALID_INDEX ? 0 : range_lines + (range == 0 ? global.start_lines : 0);
	const bool finished = global.FinishRange(range, lines);
	if (global.row_source && global.stream && finished) {
		const idx_t counted = global.row_source->released * source.buffer_manager->GetBufferSize();
		const idx_t size = ranges.Size();
		*source.bytes_read += size > counted ? size - counted : 0;
	}
	return finished;
}

} // namespace duckdb
