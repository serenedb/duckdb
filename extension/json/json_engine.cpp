#include "json_engine.hpp"

#include "absl/cleanup/cleanup.h"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/formats/csv_simd.hpp"
#include "duckdb/formats/value_parsers.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <bit>
#include <cmath>

namespace duckdb {

namespace {

constexpr idx_t JSON_MIN_RANGE_BYTES = 1024 * 1024;
constexpr idx_t JSON_MAX_RANGE_BYTES = 8 * 1024 * 1024;
constexpr idx_t JSON_READ_BYTES = 8 * 1024 * 1024;
constexpr idx_t JSON_LOOKUP_BYTES = 64 * 1024;
constexpr idx_t JSON_PADDING = RowWindow::PADDING;
static_assert(JSON_PADDING >= JSONTreeParser::PADDING);

void SkipWhitespace(const char *data, idx_t &offset, idx_t size) {
	while (offset < size && StringUtil::CharacterIsSpace(data[offset])) {
		offset++;
	}
}

const char *NextNewline(const char *ptr, idx_t size) {
	return const_char_ptr_cast(memchr(ptr, '\n', size));
}

const char *NextJSONDefault(const char *ptr, const char *const end) {
	if (*ptr == '{' || *ptr == '[') {
		return ptr + JsonContainerEnd(ptr, NumericCast<idx_t>(end - ptr));
	}
	idx_t parents = 0;
	while (ptr != end) {
		switch (*ptr++) {
		case '{':
		case '[':
			parents++;
			continue;
		case '}':
		case ']':
			parents--;
			break;
		case '"':
			while (ptr != end) {
				auto string_char = *ptr++;
				if (string_char == '"') {
					break;
				} else if (string_char == '\\') {
					if (ptr != end) {
						ptr++;
					}
				}
			}
			break;
		default:
			continue;
		}
		if (parents == 0) {
			break;
		}
	}
	return ptr;
}

const char *NextJSON(const char *ptr, idx_t size) {
	const char *const end = ptr + size;
	switch (*ptr) {
	case '{':
	case '[':
	case '"':
		ptr = NextJSONDefault(ptr, end);
		break;
	default:
		while (ptr != end) {
			switch (*ptr++) {
			case ',':
			case ']':
				ptr--;
				break;
			default:
				continue;
			}
			break;
		}
	}
	return ptr == end ? nullptr : ptr;
}

bool SkipJSONValue(const char *data, idx_t size, idx_t &pos) {
	const auto skip_string = [&]() {
		pos++;
		while (pos < size && data[pos] != '"') {
			pos += data[pos] == '\\' ? 2 : 1;
		}
		if (pos >= size) {
			return false;
		}
		pos++;
		return true;
	};
	if (pos >= size) {
		return false;
	}
	if (data[pos] == '"') {
		return skip_string();
	}
	if (data[pos] == '{' || data[pos] == '[') {
		idx_t depth = 0;
		while (pos < size) {
			const auto c = data[pos];
			if (c == '"') {
				if (!skip_string()) {
					return false;
				}
				continue;
			}
			if (c == '{' || c == '[') {
				depth++;
			} else if (c == '}' || c == ']') {
				if (--depth == 0) {
					pos++;
					return true;
				}
			}
			pos++;
		}
		return false;
	}
	while (pos < size && data[pos] != ',' && data[pos] != '}' && !StringUtil::CharacterIsSpace(data[pos])) {
		pos++;
	}
	return true;
}

bool FindFeatureCollectionArray(const char *data, idx_t size, idx_t &features_offset) {
	idx_t pos = 0;
	SkipWhitespace(data, pos, size);
	if (pos >= size || data[pos] != '{') {
		return false;
	}
	pos++;
	while (pos < size) {
		SkipWhitespace(data, pos, size);
		if (pos >= size || data[pos] == '}') {
			return false;
		}
		if (data[pos] == ',') {
			pos++;
			continue;
		}
		if (data[pos] != '"') {
			return false;
		}
		const auto key_start = ++pos;
		while (pos < size && data[pos] != '"') {
			pos += data[pos] == '\\' ? 2 : 1;
		}
		if (pos >= size) {
			return false;
		}
		const auto key_size = pos++ - key_start;
		SkipWhitespace(data, pos, size);
		if (pos >= size || data[pos] != ':') {
			return false;
		}
		pos++;
		SkipWhitespace(data, pos, size);
		if (pos >= size) {
			return false;
		}
		if (key_size == 8 && memcmp(data + key_start, "features", 8) == 0) {
			if (data[pos] != '[') {
				return false;
			}
			features_offset = pos;
			return true;
		}
		if (key_size == 4 && memcmp(data + key_start, "type", 4) == 0) {
			static constexpr auto FEATURE_COLLECTION = "\"FeatureCollection\"";
			static constexpr idx_t FEATURE_COLLECTION_SIZE = 19;
			if (pos + FEATURE_COLLECTION_SIZE > size ||
			    memcmp(data + pos, FEATURE_COLLECTION, FEATURE_COLLECTION_SIZE) != 0) {
				return false;
			}
		}
		if (!SkipJSONValue(data, size, pos)) {
			return false;
		}
	}
	return false;
}

bool IsEmptyOrRecordsArray(const JSONNode &array) {
	return array.size == 0 || array.First()->type == JSONValueType::OBJECT;
}

const char *UnitName(JSONFormat format) {
	return format == JSONFormat::NEWLINE_DELIMITED ? "line" : "record/value";
}

bool ParseJsonPath(const string &text, vector<string> &path) {
	if (text.empty() || text[0] == '/') {
		return false;
	}
	if (text[0] != '$') {
		if (text.find('"') != string::npos) {
			return false;
		}
		path.push_back(text);
		return true;
	}
	idx_t pos = 1;
	while (pos < text.size()) {
		if (text[pos] != '.' || pos + 1 >= text.size()) {
			return false;
		}
		pos++;
		string key;
		if (text[pos] == '"') {
			const auto close = text.find('"', pos + 1);
			if (close == string::npos || text.find('\\', pos + 1) < close) {
				return false;
			}
			key = text.substr(pos + 1, close - pos - 1);
			pos = close + 1;
		} else {
			const auto start = pos;
			while (pos < text.size() && text[pos] != '.' && text[pos] != '[' && text[pos] != '*' && text[pos] != '"') {
				pos++;
			}
			if (pos < text.size() && text[pos] != '.') {
				return false;
			}
			key = text.substr(start, pos - start);
		}
		if (key.empty()) {
			return false;
		}
		path.push_back(std::move(key));
	}
	return !path.empty();
}

bool PathOf(const Expression &expr, vector<string> &path, bool &text) {
	if (ExpressionFilter::IsSimpleFilterColumnRef(expr)) {
		return expr.GetReturnType().id() == LogicalTypeId::STRUCT;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	auto &children = function.GetChildren();
	if (children.size() != 2 || children[1]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &key = children[1]->Cast<BoundConstantExpression>().GetValue();
	if (key.IsNull() || key.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	auto &name = function.Function().GetName();
	if (name == "struct_extract") {
		if (!PathOf(*children[0], path, text) || text) {
			return false;
		}
		path.push_back(StringValue::Get(key));
		return true;
	}
	if (name == "json_extract_string" || name == "->>" || name == "json_extract_path_text") {
		if (!ExpressionFilter::IsSimpleFilterColumnRef(*children[0]) || !children[0]->GetReturnType().IsJSONType()) {
			return false;
		}
		text = true;
		return ParseJsonPath(StringValue::Get(key), path);
	}
	return false;
}

LogicalTypeId WalkLeaf(const LogicalType &type) {
	if (type.HasAlias()) {
		return LogicalTypeId::INVALID;
	}
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return LogicalTypeId::BIGINT;
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::VARCHAR:
		return type.id();
	default:
		return LogicalTypeId::INVALID;
	}
}

bool WalkLeafType(const LogicalType &type) {
	return WalkLeaf(type) != LogicalTypeId::INVALID;
}

bool FinishWalk(JsonPathPredicate &predicate, const LogicalType &leaf_type) {
	predicate.leaf = WalkLeaf(leaf_type);
	if (predicate.leaf == LogicalTypeId::INVALID) {
		return false;
	}
	if (predicate.leaf == LogicalTypeId::BIGINT) {
		predicate.lower = Value::MinimumValue(leaf_type).GetValue<int64_t>();
		predicate.upper = Value::MaximumValue(leaf_type).GetValue<int64_t>();
	}
	for (auto &constant : predicate.constants) {
		if (constant.IsNull()) {
			continue;
		}
		string error;
		auto converted = constant.DefaultTryCastAs(LogicalType(predicate.leaf), &error);
		if (!converted) {
			return false;
		}
		constant = std::move(*converted);
	}
	return true;
}

bool CollectWalk(const Expression &expr, idx_t column, vector<JsonPathPredicate> &out) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		if (expr.GetExpressionType() != ExpressionType::CONJUNCTION_AND) {
			return false;
		}
		bool any = false;
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			any = CollectWalk(*child, column, out) || any;
		}
		return any;
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto &children = op.GetChildren();
		const auto type = op.GetExpressionType();
		JsonPathPredicate predicate {column, {}, false, JsonPathCheck::IS_NULL};
		if (children.empty() || !PathOf(*children[0], predicate.path, predicate.text) || predicate.path.empty() ||
		    !WalkLeafType(children[0]->GetReturnType())) {
			return false;
		}
		if (type == ExpressionType::OPERATOR_IS_NULL || type == ExpressionType::OPERATOR_IS_NOT_NULL) {
			predicate.check =
			    type == ExpressionType::OPERATOR_IS_NULL ? JsonPathCheck::IS_NULL : JsonPathCheck::IS_NOT_NULL;
		} else if (type == ExpressionType::COMPARE_IN) {
			predicate.check = JsonPathCheck::IN_LIST;
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return false;
				}
				predicate.constants.push_back(children[i]->Cast<BoundConstantExpression>().GetValue());
			}
		} else {
			return false;
		}
		if (!FinishWalk(predicate, children[0]->GetReturnType())) {
			return false;
		}
		out.push_back(std::move(predicate));
		return true;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = expr.Cast<BoundFunctionExpression>();
		auto type = function.GetExpressionType();
		if (!BoundComparisonExpression::IsComparison(type) || type == ExpressionType::COMPARE_DISTINCT_FROM ||
		    type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return false;
		}
		auto &children = function.GetChildren();
		idx_t side = 0;
		if (children[0]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			side = 1;
			type = FlipComparisonExpression(type);
		}
		if (children[1 - side]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
		    !WalkLeafType(children[side]->GetReturnType())) {
			return false;
		}
		JsonPathPredicate predicate {column, {}, false, JsonPathCheck::COMPARE, type};
		if (!PathOf(*children[side], predicate.path, predicate.text) || predicate.path.empty()) {
			return false;
		}
		predicate.constants.push_back(children[1 - side]->Cast<BoundConstantExpression>().GetValue());
		if (!FinishWalk(predicate, children[side]->GetReturnType())) {
			return false;
		}
		out.push_back(std::move(predicate));
		return true;
	}
	default:
		return false;
	}
}

bool CompareLeaf(const Value &leaf, const JsonPathPredicate &predicate) {
	if (predicate.check == JsonPathCheck::IN_LIST) {
		for (auto &constant : predicate.constants) {
			if (!constant.IsNull() && leaf == constant) {
				return true;
			}
		}
		return false;
	}
	auto &constant = predicate.constants[0];
	if (constant.IsNull()) {
		return false;
	}
	switch (predicate.compare) {
	case ExpressionType::COMPARE_EQUAL:
		return leaf == constant;
	case ExpressionType::COMPARE_NOTEQUAL:
		return leaf != constant;
	case ExpressionType::COMPARE_LESSTHAN:
		return leaf < constant;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return leaf <= constant;
	case ExpressionType::COMPARE_GREATERTHAN:
		return leaf > constant;
	default:
		return leaf >= constant;
	}
}

} // namespace

bool JsonDetectFormat(const char *data, idx_t size, bool complete, JSONTreeParser &parser, JSONFormat &format,
                      JSONRecordType &record_type) {
	auto line_end = NextNewline(data, size);
	if (line_end != nullptr) {
		idx_t line_size = line_end - data;
		SkipWhitespace(data, line_size, size);
		JSONParseError error;
		auto root = parser.Parse(data, line_size, error);
		if (root) {
			if (root->type == JSONValueType::ARRAY && line_size == size) {
				record_type = IsEmptyOrRecordsArray(*root) ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
				format = JSONFormat::ARRAY;
				return complete;
			}
			record_type = root->type == JSONValueType::OBJECT ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
			format = JSONFormat::NEWLINE_DELIMITED;
			return true;
		}
	}
	const bool decided = complete || line_end != nullptr;
	idx_t offset = 0;
	SkipWhitespace(data, offset, size);
	auto remaining = size - offset;
	if (remaining == 0 || data[offset] == '{') {
		record_type = JSONRecordType::RECORDS;
		format = JSONFormat::UNSTRUCTURED;
		return complete || (decided && remaining > 0);
	}
	if (data[offset] != '[') {
		record_type = JSONRecordType::VALUES;
		format = JSONFormat::UNSTRUCTURED;
		return decided;
	}
	JSONParseError error;
	idx_t read_size = 0;
	auto root = parser.ParsePrefix(data + offset, remaining, read_size, error);
	if (root) {
		offset += read_size;
		SkipWhitespace(data, offset, size);
		if (offset != size) {
			record_type = JSONRecordType::VALUES;
			format = JSONFormat::UNSTRUCTURED;
			return decided;
		}
		record_type = IsEmptyOrRecordsArray(*root) ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
		format = JSONFormat::ARRAY;
		return complete;
	}
	offset++;
	SkipWhitespace(data, offset, size);
	record_type = offset == size || data[offset] == '{' ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
	format = JSONFormat::ARRAY;
	return complete;
}

static shared_ptr<CSVBufferManager> OpenJsonFile(ClientContext &context, const OpenFileInfo &file,
                                                 FileCompressionType compression) {
	CSVReaderOptions options;
	options.compression = compression;
	return CSVBufferManager::Open(context, options, file);
}

JsonSource::JsonSource(ClientContext &context, OpenFileInfo file_p, FileCompressionType compression)
    : ScanFile(context, file_p, OpenJsonFile(context, file_p, compression)) {
}

JsonScanGlobal::JsonScanGlobal(ClientContext &context_p, const JSONScanData &bind_p, shared_ptr<JsonSource> source_p,
                               JsonScanColumns columns_p, bool parallel)
    : context(context_p), bind(bind_p), options(bind_p.options), source(std::move(source_p)),
      columns(std::move(columns_p)),
      read_flags(bind_p.options.strict ? JSONCommon::STRICT_READ_FLAG : JSONCommon::READ_FLAG) {
	for (auto &type : columns.types) {
		if (TypeVisitor::Contains(type, LogicalTypeId::DECIMAL) || TypeVisitor::Contains(type, LogicalTypeId::BIGNUM)) {
			read_flags = read_flags | JSONReadFlags::NUMBER_AS_RAW;
			break;
		}
	}
	format = options.format;
	source->Rewind();
	if (format == JSONFormat::AUTO_DETECT) {
		Detect();
	}
	idx_t range_bytes = DConstants::INVALID_INDEX;
	const bool split_values = (format == JSONFormat::ARRAY && !features_prefix) || format == JSONFormat::UNSTRUCTURED;
	stream_bytes = RowInput::StreamBytes(source->HostStream());
	if (format == JSONFormat::NEWLINE_DELIMITED && parallel && source->Stream()) {
		range_bytes = stream_bytes;
	} else if ((format == JSONFormat::NEWLINE_DELIMITED || split_values) && parallel && !source->Stream()) {
		range_bytes = RowRanges::RangeBytes(source->Size(), TaskScheduler::QueryThreads(context), JSON_MIN_RANGE_BYTES,
		                                    JSON_MAX_RANGE_BYTES);
	}
	ranges = make_uniq<RowRanges>(BufferAllocator::Get(context), source->Rows(), 0, source->Size(), range_bytes);
	ranges->path = source->Path();
	parallel_values = split_values && ranges->RangeCount() > 1;
	if (parallel_values) {
		JsonRangeStart first;
		if (format == JSONFormat::UNSTRUCTURED) {
			first.origin = JsonRangeOrigin::OUTSIDE;
		}
		chain.Assign(ranges->RangeCount(), first);
	}
	validate_only = options.type == JSONScanType::READ_JSON_OBJECTS ||
	                (columns.names.empty() && options.type == JSONScanType::READ_JSON);
	PlanDirect();
}

void JsonScanGlobal::PlanDirect() {
	direct = options.type == JSONScanType::READ_JSON && options.record_type == JSONRecordType::RECORDS &&
	         format != JSONFormat::AUTO_DETECT && !columns.names.empty() && columns.feature_columns.empty();
	if (!direct) {
		return;
	}
	const idx_t column_count = columns.names.size();
	kinds.resize(column_count);
	for (idx_t i = 0; i < column_count; i++) {
		const auto &type = columns.types[i];
		const bool rendered = options.rendering != ValueRendering::NATIVE && type.id() != LogicalTypeId::VARCHAR;
		kinds[i] = rendered ? JsonDirectKind::TREE : DirectKind(type, columns.column_indices[i]);
	}
	fast.assign(column_count, JsonDirectFast::NONE);
	for (idx_t i = 0; i < column_count; i++) {
		fast[i] = DirectFast(kinds[i], columns.types[i].id());
	}
	filtered.assign(column_count, false);
	for (auto &filter : columns.filters) {
		if (filter.name.IsValid()) {
			filtered[filter.name.GetIndex()] = true;
		}
	}
	pass_skips.assign(3 * column_count, false);
	for (idx_t i = 0; i < column_count; i++) {
		pass_skips[static_cast<idx_t>(JsonDirectPass::FILTERS) * column_count + i] = !filtered[i];
		pass_skips[static_cast<idx_t>(JsonDirectPass::OTHERS) * column_count + i] = filtered[i];
	}
	for (idx_t i = 0; i < column_count; i++) {
		keys.emplace(columns.names[i], i);
		folded_keys.emplace(columns.names[i], i);
	}
	walked.assign(column_count, false);
	for (auto &filter : columns.filters) {
		if (!filter.name.IsValid() || kinds[filter.name.GetIndex()] != JsonDirectKind::TREE) {
			continue;
		}
		auto &expression_filter = ExpressionFilter::GetExpressionFilter(filter.filter.get(), "read_json");
		if (CollectWalk(*expression_filter.expr, filter.name.GetIndex(), walk)) {
			walked[filter.name.GetIndex()] = true;
		}
	}
	column_nodes.assign(column_count, DConstants::INVALID_INDEX);
	if (options.rendering != ValueRendering::NATIVE) {
		return;
	}
	for (idx_t i = 0; i < column_count; i++) {
		const auto &type = columns.types[i];
		const auto id = type.id();
		if (kinds[i] != JsonDirectKind::TREE || walked[i] || type.HasAlias() ||
		    (id != LogicalTypeId::STRUCT && id != LogicalTypeId::LIST && id != LogicalTypeId::MAP)) {
			continue;
		}
		const auto node =
		    PlanJsonNode(nodes, type, columns.column_indices.empty() ? nullptr : &columns.column_indices[i]);
		if (nodes[node].kind != JsonDirectKind::TREE) {
			column_nodes[i] = node;
			kinds[i] = nodes[node].kind;
		}
	}
}

bool JsonWalkPushdown(const Expression &expr) {
	vector<JsonPathPredicate> predicates;
	if (!CollectWalk(expr, 0, predicates)) {
		return false;
	}
	for (auto &predicate : predicates) {
		if (!predicate.text) {
			return false;
		}
	}
	return expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION;
}

void JsonScanGlobal::Detect() {
	const idx_t limit = MinValue(options.maximum_object_size * 2, source->Size());
	auto &allocator = BufferAllocator::Get(context);
	JSONTreeParser parser(read_flags, allocator);
	idx_t want = MinValue(RowWindow::PIECE, limit);
	auto detect = allocator.Allocate(want + JSON_PADDING);
	idx_t read = 0;
	while (true) {
		auto data = char_ptr_cast(detect.get());
		read += source->Rows().ReadFully(data + read, want - read, read);
		if (read == 0) {
			format = JSONFormat::NEWLINE_DELIMITED;
			return;
		}
		const bool complete = read < want || want == limit;
		idx_t features_offset;
		if (options.geojson.value_or(false) && FindFeatureCollectionArray(data, read, features_offset)) {
			features_prefix = true;
			format = JSONFormat::ARRAY;
			return;
		}
		JSONRecordType record_type;
		if ((complete || !options.geojson.value_or(false)) &&
		    JsonDetectFormat(data, read, complete, parser, format, record_type)) {
			return;
		}
		want = format == JSONFormat::ARRAY ? limit : MinValue(want * 2, limit);
		detect.Reallocate(want + JSON_PADDING);
	}
}

idx_t JsonScanGlobal::MaxThreads() const {
	if (ranges->Intake()) {
		return bind.max_threads.IsValid() ? bind.max_threads.GetIndex() : GlobalTableFunctionState::MAX_THREADS;
	}
	return ranges->RangeCount();
}

idx_t JsonScanGlobal::BufferCapacity() const {
	return options.maximum_object_size * 2;
}

void JsonScanGlobal::Computing(idx_t range, bool value) {
	lock_guard<mutex> guard(lock);
	if (value) {
		chain.computing[range]++;
	} else {
		chain.computing[range]--;
	}
}

void JsonScanGlobal::Publish(idx_t range, JsonRangeOrigin origin, const JsonRangeEntry &entry) {
	lock_guard<mutex> guard(lock);
	chain.Publish(range, static_cast<uint8_t>(origin), entry, ranges->RangeCount());
}

JsonRangeStart JsonScanGlobal::StartState(idx_t range) {
	return chain.StartState(
	    lock, range,
	    [&](idx_t missing, uint8_t origin) {
		    RowBytes bytes;
		    const idx_t begin = ranges->RangeBegin(missing);
		    ranges->ReadInto(bytes, begin, ranges->RangeEnd(missing) - begin);
		    return JsonRangeTransfer(bytes.data, bytes.size, static_cast<JsonRangeOrigin>(origin), missing == 0);
	    },
	    [&](idx_t missing, uint8_t origin, const JsonRangeEntry &entry) {
		    chain.Publish(missing, origin, entry, ranges->RangeCount());
	    });
}

bool JsonScanGlobal::Claim(idx_t &unit, idx_t &begin) {
	{
		lock_guard<mutex> guard(lock);
		if (failure.Failed()) {
			return false;
		}
	}
	if (!ranges->Claim(unit)) {
		return false;
	}
	begin = ranges->RangeBegin(unit, NumericLimits<idx_t>::Maximum());
	return true;
}

void JsonScanGlobal::Finish(idx_t unit, idx_t records) {
	ranges->Finish(unit);
	lock_guard<mutex> guard(lock);
	lines.Add(unit, records);
	ThrowIfPossible();
}

void JsonScanGlobal::AddError(idx_t unit, idx_t record, string message) {
	lock_guard<mutex> guard(lock);
	if (failure.Fail(unit, record)) {
		error_message = std::move(message);
	}
	ThrowIfPossible();
}

void JsonScanGlobal::ThrowIfPossible() {
	const auto line = failure.Line(lines);
	if (!line.IsValid()) {
		return;
	}
	throw InvalidInputException(StringUtil::Replace(error_message, "{line}", to_string(line.GetIndex())));
}

void JsonScanGlobal::ThrowObjectSizeError(idx_t object_size) const {
	throw InvalidInputException(
	    "\"maximum_object_size\" of %llu bytes exceeded while reading file \"%s\" (>%llu bytes)."
	    "\n Try increasing \"maximum_object_size\".",
	    options.maximum_object_size, source->Path(), object_size);
}

JsonScan::JsonScan(JsonScanGlobal &global_p)
    : global(global_p), options(global_p.options), window(*global_p.ranges),
      arena(BufferAllocator::Get(global_p.context)),
      parser(global_p.read_flags, BufferAllocator::Get(global_p.context)), writer(JSONCommon::WRITE_FLAG),
      transform_options(global_p.columns.transform_options), direct_memory(BufferAllocator::Get(global_p.context)),
      decoder(global_p.nodes, transform_options, parser, writer) {
	transform_options.parameters.context = &global.context;
	for (auto &filter : global.columns.filters) {
		pushed_filters.Add(global.context, filter.filter.get(), filter.output_column);
	}
	filter_selection.Initialize(STANDARD_VECTOR_SIZE);
	if (global.direct) {
		direct_parser.set_allocator(&direct_memory.callbacks);
		walk_parser.set_allocator(&direct_memory.callbacks);
		candidates.Initialize(STANDARD_VECTOR_SIZE);
		const idx_t column_count = global.columns.names.size();
		direct_raw.resize(column_count);
		direct_found.Resize(column_count);
		for (idx_t i = 0; i < column_count; i++) {
			direct_strings.emplace_back(LogicalType::VARCHAR, idx_t(STANDARD_VECTOR_SIZE));
			if (global.kinds[i] == JsonDirectKind::TREE) {
				direct_raw[i].resize(STANDARD_VECTOR_SIZE);
			}
		}
	}
}

JsonScan::~JsonScan() {
	if (claimed && !loaded && global.ranges->Intake()) {
		global.ranges->FailIntake();
	}
}

bool JsonScan::Claim() {
	claimed = false;
	done = false;
	stopped = false;
	unit_records = 0;
	if (!global.Claim(unit, begin)) {
		return false;
	}
	loaded = false;
	claimed = true;
	return true;
}

void JsonScan::Load() {
	auto &ranges = *global.ranges;
	if (ranges.Intake()) {
		RowIntake intake(window);
		if (!intake.Start(unit, global.stream_bytes)) {
			claimed = false;
			return;
		}
		const auto &claim = intake.claim;
		begin = claim.begin;
		first = begin == 0;
		data = window.bytes->data;
		size = window.bytes->size;
		file_offset = window.bytes->offset;
		cursor = begin - file_offset;
		count = 0;
		RowSearch search;
		search.searched_to = claim.resumed ? claim.search.searched_to : begin;
		limit = CutRecords(search.searched_to);
		if (limit == DConstants::INVALID_INDEX) {
			intake.CarryOn(search);
			global.Finish(unit, 0);
			claimed = false;
			return;
		}
		intake.Cut(limit);
		Begin();
		return;
	}
	first = begin == 0;
	limit = ranges.RangeEnd(unit, NumericLimits<idx_t>::Maximum());
	if (global.parallel_values) {
		LoadValueRange();
		Begin();
		return;
	}
	const idx_t from = first ? 0 : begin - 1;
	window.Start(from, !sampling);
	file_offset = from;
	cursor = 0;
	count = 0;
	ReadWindow(from, MinValue(limit - from, JSON_READ_BYTES));
	if (!first) {
		while (true) {
			auto newline = NextNewline(data + cursor, size - cursor);
			if (newline) {
				cursor = NumericCast<idx_t>(newline - data) + 1;
				break;
			}
			cursor = size;
			if (file_offset + size >= limit || !ReadMore()) {
				done = true;
				break;
			}
		}
	}
	Begin();
}

void JsonScan::LoadValueRange() {
	auto &ranges = *global.ranges;
	const bool array = global.format == JSONFormat::ARRAY;
	const bool publish = unit + 1 < ranges.RangeCount();
	window.Start(begin, !sampling);
	file_offset = begin;
	cursor = 0;
	count = 0;
	if (publish) {
		global.Computing(unit, true);
	}
	idx_t span;
	{
		absl::Cleanup stop = [&] {
			if (publish) {
				global.Computing(unit, false);
			}
		};
		ReadWindow(begin, limit - begin);
		while (file_offset + size < limit && ReadMore()) {
		}
		span = MinValue(size, limit - file_offset);
		if (publish && first && array) {
			global.Publish(unit, JsonRangeOrigin::BEFORE_ARRAY,
			               JsonRangeTransfer(data, span, JsonRangeOrigin::BEFORE_ARRAY, true));
		} else if (publish) {
			JsonRangeEntry outside;
			JsonRangeEntry string;
			JsonRangeTransfers(data, span, outside, string);
			global.Publish(unit, JsonRangeOrigin::OUTSIDE, outside);
			global.Publish(unit, JsonRangeOrigin::STRING, string);
		}
	}
	if (first) {
		return;
	}
	const auto start = global.StartState(unit);
	const idx_t found = array ? JsonArraySeparator(data, span, start) : JsonValueStart(data, span, start);
	if (found == DConstants::INVALID_INDEX) {
		cursor = span;
		done = true;
	} else {
		cursor = array ? found + 1 : found;
	}
}

void JsonScan::Begin() {
	if (first) {
		Fill(3);
		StringUtil::SkipBOM(data, size, cursor);
		if (global.format == JSONFormat::ARRAY) {
			SkipArrayStart();
		}
	}
	line_start = file_offset + cursor;
	loaded = true;
}

void JsonScan::Release() {
	claimed = false;
}

bool JsonScan::ReadMore(bool may_idle) {
	if (Last()) {
		return false;
	}
	if (may_idle && window.Waits()) {
		idle = true;
		return false;
	}
	const idx_t keep = count != 0 ? MinValue(starts[0], cursor) : cursor;
	const idx_t rest = limit > window.read_to ? MinValue(limit - window.read_to, JSON_READ_BYTES) : JSON_LOOKUP_BYTES;
	return ReadWindow(file_offset + keep, MaxValue(rest, size - cursor));
}

bool JsonScan::ReadWindow(idx_t keep, idx_t piece) {
	const idx_t before = window.read_to;
	window.Read(keep, piece);
	auto &bytes = *window.bytes;
	const idx_t shift = bytes.offset - file_offset;
	data = bytes.data;
	size = bytes.size;
	file_offset = bytes.offset;
	if (shift != 0) {
		cursor -= shift;
		for (idx_t i = 0; i < count; i++) {
			starts[i] -= shift;
		}
	}
	if (!global.source->Stream()) {
		global.source->Consume(window.read_to - before);
	}
	return window.read_to != before;
}

idx_t JsonScan::CutRecords(idx_t &searched_to) {
	const idx_t from = MaxValue(searched_to, begin) - file_offset;
	const auto last = std::string_view(data + from, size - from).rfind('\n');
	if (last != std::string_view::npos) {
		return file_offset + from + last + 1;
	}
	if (Last()) {
		return file_offset + size;
	}
	const idx_t record = file_offset + size - begin;
	if (record > options.maximum_object_size) {
		global.ThrowObjectSizeError(record);
	}
	searched_to = file_offset + size;
	return DConstants::INVALID_INDEX;
}

void JsonScan::Fill(idx_t bytes) {
	while (size - cursor < bytes && ReadMore()) {
	}
}

void JsonScan::SkipBlank() {
	while (true) {
		SkipWhitespace(data, cursor, size);
		if (cursor < size || !ReadMore()) {
			return;
		}
	}
}

void JsonScan::SkipArrayStart() {
	SkipBlank();
	if (cursor == size) {
		return;
	}
	if (global.features_prefix) {
		Fill(global.BufferCapacity());
		idx_t features_offset;
		if (!FindFeatureCollectionArray(data + cursor, size - cursor, features_offset)) {
			throw InvalidInputException("Expected a GeoJSON FeatureCollection in file \"%s\"", global.source->Path());
		}
		cursor += features_offset;
	}
	if (data[cursor] != '[') {
		throw InvalidInputException(
		    "Expected top-level JSON array with format='array', but first character is '%c' in file \"%s\"."
		    "\n Try setting format='auto' or format='newline_delimited'.",
		    data[cursor], global.source->Path());
	}
	cursor++;
	SkipBlank();
	if (cursor >= size) {
		throw InvalidInputException("Missing closing brace ']' in JSON array with format='array' in file \"%s\"",
		                            global.source->Path());
	}
	if (data[cursor] == ']') {
		cursor++;
		SkipBlank();
		if (cursor != size && !global.features_prefix) {
			throw InvalidInputException(
			    "Empty array with trailing data when parsing JSON array with format='array' in file \"%s\"",
			    global.source->Path());
		}
		cursor = size;
		done = true;
	}
}

bool JsonScan::FrameRecord(idx_t &record_end, bool may_idle) {
	while (true) {
		const idx_t remaining = size - cursor;
		const char *start = data + cursor;
		const char *end = remaining == 0                                   ? nullptr
		                  : global.format == JSONFormat::NEWLINE_DELIMITED ? NextNewline(start, remaining)
		                                                                   : NextJSON(start, remaining);
		if (end) {
			record_end = NumericCast<idx_t>(end - data);
			return true;
		}
		if (Last()) {
			record_end = size;
			return remaining != 0;
		}
		if (remaining > options.maximum_object_size) {
			global.ThrowObjectSizeError(remaining);
		}
		if (!ReadMore(may_idle)) {
			if (idle) {
				return false;
			}
			record_end = size;
			return remaining != 0;
		}
	}
}

void JsonScan::SkipSpace(bool may_idle) {
	while (true) {
		while (cursor < size && StringUtil::CharacterIsSpace(data[cursor])) {
			if (data[cursor++] == '\n') {
				line_start = file_offset + cursor;
				if (line_start >= limit && global.format == JSONFormat::NEWLINE_DELIMITED) {
					return;
				}
			}
		}
		if (cursor < size || !ReadMore(may_idle)) {
			return;
		}
	}
}

bool JsonScan::AfterArrayRecord(idx_t record_size) {
	SkipSpace();
	if (cursor < size && data[cursor] == ']' && global.features_prefix) {
		cursor = size;
		done = true;
		return true;
	}
	if (cursor < size && (data[cursor] == ',' || data[cursor] == ']')) {
		if (global.parallel_values && data[cursor] == ']') {
			limit = NumericLimits<idx_t>::Maximum();
		} else if (global.parallel_values && file_offset + cursor >= limit) {
			done = true;
			return true;
		}
		cursor++;
		return true;
	}
	JSONParseError error;
	error.has_error = true;
	error.message = "unexpected character";
	error.position = record_size;
	ParseError(unit_records, error, "");
	return false;
}

void JsonScan::ResetBatch() {
	count = 0;
	batch_bytes = 0;
	arena.Reset();
	parser.Clear();
}

bool JsonScan::NextRecords() {
	ResetBatch();
	idle = false;
	const idx_t batch_start = file_offset + cursor;
	while (count < STANDARD_VECTOR_SIZE && !done) {
		SkipSpace(count != 0);
		if (idle) {
			break;
		}
		if ((line_start >= limit && global.format == JSONFormat::NEWLINE_DELIMITED) || cursor == size ||
		    (global.parallel_values && global.format == JSONFormat::UNSTRUCTURED && file_offset + cursor >= limit)) {
			done = true;
			break;
		}
		idx_t record_end;
		if (!FrameRecord(record_end, count != 0)) {
			done = !idle;
			break;
		}
		const idx_t begin = cursor;
		const idx_t json_size = record_end - begin;
		if (!global.direct && !Parse(count, unit_records, data + begin, json_size, size + JSON_PADDING - begin)) {
			stopped = true;
			return false;
		}
		lengths[count] = json_size;
		idx_t trimmed_begin = begin;
		idx_t trimmed_end = record_end;
		while (trimmed_end > trimmed_begin && StringUtil::CharacterIsSpace(data[trimmed_end - 1])) {
			trimmed_end--;
		}
		starts[count] = trimmed_begin;
		sizes[count] = trimmed_end - trimmed_begin;
		count++;
		unit_records++;
		cursor = record_end;
		if (global.format == JSONFormat::ARRAY && !AfterArrayRecord(json_size)) {
			stopped = true;
			return false;
		}
	}
	if (sampling && count != 0) {
		SkipSpace(true);
	}
	batch_bytes = file_offset + cursor - batch_start;
	return count != 0;
}

bool JsonScan::ParseBatch() {
	parser.Clear();
	for (idx_t i = 0; i < count; i++) {
		if (!Parse(i, unit_records - count + i, data + starts[i], lengths[i], size + JSON_PADDING - starts[i])) {
			stopped = true;
			return false;
		}
	}
	return true;
}

bool JsonScan::Parse(idx_t index, idx_t record, const char *json, idx_t len, idx_t readable) {
	JSONParseError error;
	if (global.validate_only && !sampling) {
		if (parser.Validate(json, len, readable, error)) {
			roots[index] = 0;
			return true;
		}
	} else {
		auto root = parser.ParseAppend(json, len, readable, error);
		if (root.IsValid()) {
			roots[index] = root.GetIndex();
			return true;
		}
	}
	bool can_ignore = options.ignore_errors;
	string extra;
	if (global.format != JSONFormat::NEWLINE_DELIMITED) {
		can_ignore = false;
		extra = options.ignore_errors ? "Parse errors cannot be ignored for JSON formats other than 'newline_delimited'"
		                              : "";
	}
	if (!can_ignore) {
		if (extra.empty() &&
		    (error.message == "unexpected end of data" || error.message == "unexpected content after document")) {
			extra = "Try auto-detecting the JSON format";
		}
		ParseError(record, error, extra);
		return false;
	}
	roots[index] = DConstants::INVALID_INDEX;
	return true;
}

void JsonScan::ParseError(idx_t record, const JSONParseError &error, const string &extra) {
	auto message =
	    StringUtil::Format("Malformed JSON in file \"%s\", at byte %llu in %s {line}: %s. %s", global.source->Path(),
	                       error.position + 1, UnitName(global.format), error.message, extra);
	global.AddError(unit, record, std::move(message));
	stopped = true;
}

void JsonScan::TransformError(idx_t record, const string &message) {
	auto full = StringUtil::Format("JSON transform error in file \"%s\", in %s {line}: %s", global.source->Path(),
	                               UnitName(global.format), message);
	global.AddError(unit, record, std::move(full));
	stopped = true;
}

idx_t JsonScan::Scan(DataChunk &output) {
	absl::Cleanup fail = [&] {
		global.ranges->FailIntake();
	};
	const idx_t rows = ScanUnit(output);
	std::move(fail).Cancel();
	return rows;
}

idx_t JsonScan::ScanUnit(DataChunk &output) {
	output.Reset();
	if (claimed && !loaded) {
		Load();
	}
	while (claimed && !stopped) {
		if (!NextRecords()) {
			if (!stopped) {
				global.Finish(unit, unit_records);
				claimed = false;
			}
			return 0;
		}
		const auto rows =
		    options.type == JSONScanType::READ_JSON_OBJECTS ? DecodeObjects(output, count) : Decode(output, count);
		if (stopped) {
			return 0;
		}
		if (rows != 0) {
			return rows;
		}
		output.Reset();
	}
	return 0;
}

void JsonScan::HoldWindow(Vector &vector) {
	StringVector::AddAuxiliaryData(vector, make_uniq<RowBytesHolder>(window.bytes));
}

idx_t JsonScan::Sample(const JSONNode **&sample, idx_t &bytes) {
	sampling = true;
	if (!claimed && !Claim()) {
		return 0;
	}
	if (!loaded) {
		Load();
	}
	if (!claimed || stopped || !NextRecords()) {
		return 0;
	}
	for (idx_t i = 0; i < count; i++) {
		values[i] = roots[i] == DConstants::INVALID_INDEX ? nullptr : parser.Node(roots[i]);
	}
	sample = values;
	bytes = batch_bytes;
	return count;
}

void JsonScan::FillRowNumbers(DataChunk &output, idx_t rows) {
	if (global.columns.file_row_number == DConstants::INVALID_INDEX) {
		return;
	}
	auto numbers = FlatVector::Writer<int64_t>(output.data[global.columns.file_row_number], rows);
	for (idx_t i = 0; i < rows; i++) {
		numbers.WriteValue(NumericCast<int64_t>(file_offset + starts[i]));
	}
}

idx_t JsonScan::ApplyFilters(DataChunk &output, idx_t rows, bool filtered_columns_only, optional_idx candidate_count) {
	auto &filters = global.columns.filters;
	idx_t approved = rows;
	if (candidate_count.IsValid() && candidate_count.GetIndex() != rows) {
		filter_selection.Initialize(candidates);
		approved = candidate_count.GetIndex();
	} else {
		filter_selection.Initialize(nullptr);
	}
	for (auto &filter : filters) {
		FlatVector::SetSize(output.data[filter.output_column], count_t(rows));
	}
	approved = pushed_filters.Apply(filter_selection, output, rows, approved);
	if (approved == rows) {
		return rows;
	}
	for (idx_t i = 0; i < output.ColumnCount(); i++) {
		if (filtered_columns_only) {
			bool filtered = false;
			for (auto &filter : filters) {
				filtered = filtered || filter.output_column == i;
			}
			if (!filtered) {
				continue;
			}
		}
		output.data[i].Slice(filter_selection, approved);
		output.data[i].Flatten(approved);
	}
	return approved;
}

static bool TransformGeoJSONFeatures(const JSONNode *values[], JSONTransformScratch &scratch, const idx_t count,
                                     const JsonScanColumns &columns, const vector<Vector *> &result_vectors,
                                     JSONTransformOptions &transform_options) {
	vector<string> feature_names, property_names;
	vector<Vector *> feature_vectors, property_vectors;
	vector<ColumnIndex> feature_indices, property_indices;
	for (idx_t i = 0; i < columns.feature_columns.size(); i++) {
		const auto &feature_column = columns.feature_columns[i];
		const auto from_properties = feature_column.from_properties;
		(from_properties ? property_names : feature_names).push_back(feature_column.key);
		(from_properties ? property_vectors : feature_vectors).push_back(result_vectors[i]);
		if (!columns.column_indices.empty()) {
			(from_properties ? property_indices : feature_indices).push_back(columns.column_indices[i]);
		}
	}
	bool success = true;
	if (!feature_names.empty()) {
		success = JSONTransform::TransformObject(values, scratch, count, feature_names, feature_vectors,
		                                         transform_options, feature_indices, false);
	}
	if (success && !property_names.empty()) {
		vector<const JSONNode *> property_values(count);
		for (idx_t i = 0; i < count; i++) {
			auto &value = values[i];
			property_values[i] = value ? value->Get(std::string_view("properties")) : nullptr;
		}
		success = JSONTransform::TransformObject(property_values.data(), scratch, count, property_names,
		                                         property_vectors, transform_options, property_indices, false);
	}
	return success;
}

void JsonScan::NullRow(idx_t row, JsonDirectPass pass, const vector<Vector *> &vectors) {
	for (idx_t column = 0; column < vectors.size(); column++) {
		const bool filtered = global.filtered[column];
		if (!vectors[column] || (pass == JsonDirectPass::FILTERS && !filtered) ||
		    (pass == JsonDirectPass::OTHERS && filtered)) {
			continue;
		}
		const auto kind = global.kinds[column];
		if (global.column_nodes[column] != DConstants::INVALID_INDEX) {
			decoder.Null(global.column_nodes[column], row);
		} else if (kind == JsonDirectKind::TREE) {
			direct_raw[column][row] = std::string_view();
		} else {
			FlatVector::ValidityMutable(kind == JsonDirectKind::STRING ? direct_strings[column] : *vectors[column])
			    .SetInvalid(row);
		}
	}
}

bool JsonScan::WalkRejects(idx_t column, std::string_view raw) {
	namespace ondemand = simdjson::ondemand;
	const char *const end = data + size + JSON_PADDING;
	for (auto &predicate : global.walk) {
		if (predicate.column != column) {
			continue;
		}
		std::string_view current = raw;
		bool null = false;
		for (auto &key : predicate.path) {
			ondemand::document document;
			if (walk_parser.iterate(current.data(), current.size(), NumericCast<idx_t>(end - current.data()))
			        .get(document)) {
				return false;
			}
			ondemand::json_type type;
			if (document.type().get(type)) {
				return false;
			}
			if (type == ondemand::json_type::null) {
				null = true;
				break;
			}
			if (type != ondemand::json_type::object) {
				if (!predicate.text) {
					return false;
				}
				null = true;
				break;
			}
			ondemand::object object;
			if (document.get_object().get(object)) {
				return false;
			}
			idx_t exact = 0;
			idx_t folded = 0;
			std::string_view exact_raw;
			std::string_view folded_raw;
			for (auto field_result : object) {
				ondemand::field field;
				if (std::move(field_result).get(field)) {
					return false;
				}
				const auto name = field.escaped_key();
				if (memchr(name.data(), '\\', name.size())) {
					return false;
				}
				const bool is_exact = name == key;
				const bool is_folded = !is_exact && !predicate.text && StringUtil::CIEquals(name, key);
				if (!is_exact && !is_folded) {
					continue;
				}
				std::string_view value_raw;
				auto value = std::move(field).value();
				if (value.raw_json().get(value_raw)) {
					return false;
				}
				if (is_exact) {
					exact++;
					exact_raw = TrimToken(value_raw);
				} else {
					folded++;
					folded_raw = TrimToken(value_raw);
				}
			}
			if (exact > 1 || (exact == 0 && folded > 1)) {
				return false;
			}
			if (exact == 0 && folded == 0) {
				null = true;
				break;
			}
			current = exact == 1 ? exact_raw : folded_raw;
		}
		Value leaf;
		if (!null) {
			ondemand::document document;
			if (walk_parser.iterate(current.data(), current.size(), NumericCast<idx_t>(end - current.data()))
			        .get(document)) {
				return false;
			}
			ondemand::json_type type;
			if (document.type().get(type)) {
				return false;
			}
			switch (type) {
			case ondemand::json_type::null: {
				bool is_null = false;
				if (document.is_null().get(is_null) || !is_null) {
					return false;
				}
				null = true;
				break;
			}
			case ondemand::json_type::string: {
				std::string_view text;
				if (predicate.leaf != LogicalTypeId::VARCHAR || document.get_string().get(text)) {
					return false;
				}
				leaf = Value(string(text));
				break;
			}
			case ondemand::json_type::boolean: {
				bool boolean;
				if (predicate.leaf != LogicalTypeId::BOOLEAN || document.get_bool().get(boolean)) {
					return false;
				}
				leaf = Value::BOOLEAN(boolean);
				break;
			}
			case ondemand::json_type::number: {
				ondemand::number number;
				if (document.get_number().get(number)) {
					return false;
				}
				const auto number_type = number.get_number_type();
				if (predicate.leaf == LogicalTypeId::DOUBLE) {
					leaf = Value::DOUBLE(number.as_double());
				} else if (predicate.leaf == LogicalTypeId::BIGINT &&
				           number_type == ondemand::number_type::signed_integer) {
					const auto integer = number.get_int64();
					if (integer < predicate.lower || integer > predicate.upper) {
						return false;
					}
					leaf = Value::BIGINT(integer);
				} else {
					return false;
				}
				break;
			}
			default:
				return false;
			}
		}
		switch (predicate.check) {
		case JsonPathCheck::IS_NULL:
			if (!null) {
				return true;
			}
			break;
		case JsonPathCheck::IS_NOT_NULL:
			if (null) {
				return true;
			}
			break;
		default:
			if (null || !CompareLeaf(leaf, predicate)) {
				return true;
			}
			break;
		}
	}
	return false;
}

JsonRecordResult JsonScan::DirectRecord(idx_t record, idx_t row, JsonDirectPass pass, const vector<Vector *> &vectors) {
	namespace ondemand = simdjson::ondemand;
	constexpr auto FALLBACK = JsonRecordResult::FALLBACK;
	const idx_t column_count = vectors.size();
	const uint8_t *skips = global.pass_skips.data() + static_cast<idx_t>(pass) * column_count;
	const idx_t begin = starts[record];
	ondemand::document document;
	auto code = direct_parser.iterate(data + begin, lengths[record], size + JSON_PADDING - begin).get(document);
	if (code == simdjson::MEMALLOC && direct_memory.error.HasError()) {
		auto error = std::move(direct_memory.error);
		direct_memory.error = ErrorData();
		error.Throw();
	}
	if (code) {
		return FALLBACK;
	}
	ondemand::object object;
	if (document.get_object().get(object)) {
		return FALLBACK;
	}
	auto found = direct_found.Data();
	memset(found, 0, column_count);
	const bool unknown_error = pass != JsonDirectPass::OTHERS && direct_unknown_error;
	const bool duplicate_error = pass != JsonDirectPass::OTHERS && direct_duplicate_error;
	const bool walking = pass == JsonDirectPass::FILTERS && !global.walk.empty();
	bool rejected = false;
	idx_t position = 0;
	for (auto field_result : object) {
		ondemand::field field;
		if (std::move(field_result).get(field)) {
			return FALLBACK;
		}
		const auto key = field.escaped_key();
		if (position == key_hints.size()) {
			key_hints.emplace_back();
		}
		auto &hint = key_hints[position++];
		if (hint.strength == 0 || !SameKey(hint, key)) {
			if (memchr(key.data(), '\\', key.size())) {
				return FALLBACK;
			}
			hint.key.assign(key.data(), key.size());
			KeyWords(key, hint.words);
			hint.column = DConstants::INVALID_INDEX;
			hint.strength = 3;
			auto exact = global.keys.find(key);
			if (exact != global.keys.end()) {
				hint.column = exact->second;
				hint.strength = 2;
			} else {
				auto folded = global.folded_keys.find(key);
				if (folded != global.folded_keys.end()) {
					hint.column = folded->second;
					hint.strength = 1;
				}
			}
		}
		if (hint.column == DConstants::INVALID_INDEX) {
			if (unknown_error) {
				return FALLBACK;
			}
			continue;
		}
		const idx_t column = hint.column;
		const uint8_t strength = hint.strength;
		if (found[column] == strength) {
			if (duplicate_error) {
				return FALLBACK;
			}
			continue;
		}
		if (found[column] > strength) {
			continue;
		}
		found[column] = strength;
		if (rejected || skips[column] || !vectors[column]) {
			continue;
		}
		auto value = std::move(field).value();
		if (walking && global.walked[column]) {
			std::string_view raw;
			if (value.raw_json().get(raw)) {
				return FALLBACK;
			}
			if (WalkRejects(column, TrimToken(raw))) {
				rejected = true;
			} else {
				direct_raw[column][row] = raw;
			}
			continue;
		}
		const auto kind = global.kinds[column];
		if (kind == JsonDirectKind::STRUCT || kind == JsonDirectKind::LIST || kind == JsonDirectKind::MAP) {
			if (!decoder.Decode(global.column_nodes[column], value, row)) {
				return FALLBACK;
			}
			continue;
		}
		if (kind == JsonDirectKind::TREE) {
			std::string_view raw;
			if (value.raw_json().get(raw)) {
				return FALLBACK;
			}
			direct_raw[column][row] = raw;
			continue;
		}
		direct_validity[column]->SetValid(row);
		switch (global.fast[column]) {
		case JsonDirectFast::BIGINT:
			if (!value.get_int64().get(reinterpret_cast<int64_t *>(direct_data[column])[row])) {
				continue;
			}
			break;
		case JsonDirectFast::INTEGER: {
			int64_t integer;
			if (!value.get_int64().get(integer) && integer >= NumericLimits<int32_t>::Minimum() &&
			    integer <= NumericLimits<int32_t>::Maximum()) {
				reinterpret_cast<int32_t *>(direct_data[column])[row] = static_cast<int32_t>(integer);
				continue;
			}
			break;
		}
		case JsonDirectFast::DOUBLE: {
			const auto token = value.raw_json_token();
			auto &result = reinterpret_cast<double *>(direct_data[column])[row];
			if (!value.get_double().get(result)) {
				if (result == 0 && std::signbit(result) &&
				    TrimToken(token).find_first_of(".eE") == std::string_view::npos) {
					result = 0;
				}
				continue;
			}
			break;
		}
		case JsonDirectFast::STRING: {
			const auto token = TrimToken(value.raw_json_token());
			if (token.size() >= 2 && token.front() == '"') {
				const auto content = token.substr(1, token.size() - 2);
				if (PlainString(content)) {
					MakeString(content.data(), content.size(), data + size + JSON_PADDING,
					           reinterpret_cast<string_t *>(direct_data[column])[row]);
					continue;
				}
			}
			break;
		}
		default:
			break;
		}
		if (!decoder.Value(kind, direct_data[column], *direct_validity[column], direct_strings[column],
		                   *vectors[column], value, row)) {
			return FALLBACK;
		}
	}
	if (!document.at_end()) {
		return FALLBACK;
	}
	for (idx_t column = 0; column < column_count; column++) {
		if (found[column] || skips[column] || !vectors[column]) {
			continue;
		}
		if (global.column_nodes[column] != DConstants::INVALID_INDEX) {
			continue;
		}
		if (global.kinds[column] == JsonDirectKind::TREE) {
			direct_raw[column][row] = std::string_view();
		} else {
			direct_validity[column]->SetInvalid(row);
		}
	}
	if (rejected) {
		NullRow(row, pass, vectors);
		return JsonRecordResult::REJECTED;
	}
	return JsonRecordResult::DECODED;
}

bool JsonScan::FinishDirect(idx_t rows, JsonDirectPass pass, const vector<Vector *> &vectors) {
	auto &columns = global.columns;
	JSONTransformScratch scratch {arena, writer};
	for (idx_t column = 0; column < vectors.size(); column++) {
		const bool filtered = global.filtered[column];
		if (!vectors[column] || (pass == JsonDirectPass::FILTERS && !filtered) ||
		    (pass == JsonDirectPass::OTHERS && filtered)) {
			continue;
		}
		switch (global.kinds[column]) {
		case JsonDirectKind::STRING:
			if (!JSONTransform::TransformStrings(direct_strings[column], *vectors[column], rows, transform_options)) {
				return false;
			}
			break;
		case JsonDirectKind::TREE: {
			if (vectors[column]->GetType().IsJSONType()) {
				if (!decoder.WriteJsonValues(direct_raw[column].data(), rows, *vectors[column])) {
					return false;
				}
				break;
			}
			for (idx_t row = 0; row < rows; row++) {
				auto raw = direct_raw[column][row];
				if (raw.empty()) {
					roots[row] = DConstants::INVALID_INDEX;
					continue;
				}
				JSONParseError error;
				const idx_t readable = NumericCast<idx_t>(data + size + JSON_PADDING - raw.data());
				auto root = parser.ParseAppend(raw.data(), raw.size(), readable, error);
				if (!root.IsValid()) {
					return false;
				}
				roots[row] = root.GetIndex();
			}
			for (idx_t row = 0; row < rows; row++) {
				values[row] = roots[row] == DConstants::INVALID_INDEX ? nullptr : parser.Node(roots[row]);
			}
			if (!JSONTransform::Transform(values, scratch, *vectors[column], rows, transform_options,
			                              columns.column_indices[column])) {
				return false;
			}
			break;
		}
		case JsonDirectKind::STRUCT:
		case JsonDirectKind::LIST:
		case JsonDirectKind::MAP:
			if (!decoder.Finish(global.column_nodes[column], rows, scratch, data + size + JSON_PADDING)) {
				return false;
			}
			break;
		default:
			break;
		}
	}
	return true;
}

bool JsonScan::DecodeDirect(DataChunk &output, idx_t &rows) {
	auto &columns = global.columns;
	vector<Vector *> vectors;
	vectors.reserve(columns.names.size());
	for (auto i : columns.column_ids) {
		vectors.push_back(&output.data[i]);
	}
	direct_data.resize(vectors.size());
	direct_validity.resize(vectors.size());
	for (idx_t column = 0; column < vectors.size(); column++) {
		if (global.column_nodes[column] != DConstants::INVALID_INDEX) {
			decoder.Bind(global.column_nodes[column], *vectors[column], STANDARD_VECTOR_SIZE);
			continue;
		}
		const bool strings = global.kinds[column] == JsonDirectKind::STRING;
		if (strings) {
			direct_strings[column].Initialize(VectorDataInitialization::UNINITIALIZED, STANDARD_VECTOR_SIZE);
		}
		auto &target = strings ? direct_strings[column] : *vectors[column];
		direct_data[column] = FlatVector::GetDataMutable(target);
		direct_validity[column] = &FlatVector::ValidityMutable(target);
		if (!strings && global.fast[column] == JsonDirectFast::STRING) {
			HoldWindow(target);
		}
	}
	direct_unknown_error = transform_options.error_unknown_key;
	direct_duplicate_error = transform_options.error_duplicate_key;
	bool late = !columns.filters.empty();
	for (auto &filter : columns.filters) {
		late = late && filter.name.IsValid();
	}
	if (!late) {
		for (idx_t row = 0; row < rows; row++) {
			if (DirectRecord(row, row, JsonDirectPass::ALL, vectors) != JsonRecordResult::DECODED) {
				return false;
			}
		}
		if (!FinishDirect(rows, JsonDirectPass::ALL, vectors)) {
			return false;
		}
		FillRowNumbers(output, rows);
		if (!columns.filters.empty()) {
			rows = ApplyFilters(output, rows, false);
		}
		output.SetChildCardinality(rows);
		return true;
	}
	idx_t candidate_count = 0;
	for (idx_t row = 0; row < rows; row++) {
		const auto result = DirectRecord(row, row, JsonDirectPass::FILTERS, vectors);
		if (result == JsonRecordResult::FALLBACK) {
			return false;
		}
		if (result == JsonRecordResult::DECODED) {
			candidates.set_index(candidate_count++, row);
		}
	}
	if (!FinishDirect(rows, JsonDirectPass::FILTERS, vectors)) {
		return false;
	}
	const auto approved = ApplyFilters(output, rows, true, candidate_count);
	for (idx_t i = 0; i < approved; i++) {
		const auto record = approved == rows ? i : filter_selection.get_index(i);
		if (DirectRecord(record, i, JsonDirectPass::OTHERS, vectors) != JsonRecordResult::DECODED) {
			return false;
		}
	}
	if (!FinishDirect(approved, JsonDirectPass::OTHERS, vectors)) {
		return false;
	}
	if (approved != rows) {
		for (idx_t i = 0; i < approved; i++) {
			const auto record = filter_selection.get_index(i);
			starts[i] = starts[record];
			sizes[i] = sizes[record];
		}
	}
	rows = approved;
	FillRowNumbers(output, rows);
	output.SetChildCardinality(rows);
	return true;
}

idx_t JsonScan::Decode(DataChunk &output, idx_t rows) {
	auto &columns = global.columns;
	if (global.direct) {
		idx_t direct_rows = rows;
		if (DecodeDirect(output, direct_rows)) {
			return direct_rows;
		}
		output.Reset();
		transform_options.error_message.clear();
		transform_options.object_index = DConstants::INVALID_INDEX;
		if (!ParseBatch()) {
			return 0;
		}
	}
	if (!global.validate_only) {
		for (idx_t i = 0; i < rows; i++) {
			values[i] = roots[i] == DConstants::INVALID_INDEX ? nullptr : parser.Node(roots[i]);
		}
	}
	JSONTransformScratch scratch {arena, writer};
	const auto record_type = options.record_type;
	bool late = record_type == JSONRecordType::RECORDS && !columns.filters.empty() && !columns.names.empty();
	for (auto &filter : columns.filters) {
		late = late && filter.name.IsValid();
	}
	if (!columns.names.empty()) {
		vector<Vector *> result_vectors;
		result_vectors.reserve(columns.names.size());
		for (auto i : columns.column_ids) {
			result_vectors.emplace_back(&output.data[i]);
		}
		D_ASSERT(record_type != JSONRecordType::AUTO_DETECT);
		bool success;
		if (late) {
			vector<Vector *> filter_vectors(result_vectors.size(), nullptr);
			vector<Vector *> other_vectors = result_vectors;
			for (auto &filter : columns.filters) {
				filter_vectors[filter.name.GetIndex()] = result_vectors[filter.name.GetIndex()];
				other_vectors[filter.name.GetIndex()] = nullptr;
			}
			success =
			    JSONTransform::TransformObject(values, scratch, rows, columns.names, filter_vectors, transform_options,
			                                   columns.column_indices, transform_options.error_unknown_key);
			if (success) {
				const auto approved = ApplyFilters(output, rows, true);
				if (approved != rows) {
					for (idx_t i = 0; i < approved; i++) {
						const auto row = filter_selection.get_index(i);
						values[i] = values[row];
						starts[i] = starts[row];
						sizes[i] = sizes[row];
					}
				}
				auto other_options = transform_options;
				other_options.error_duplicate_key = false;
				other_options.error_missing_key = false;
				success = JSONTransform::TransformObject(values, scratch, approved, columns.names, other_vectors,
				                                         other_options, columns.column_indices, false);
				if (!success) {
					transform_options.error_message = other_options.error_message;
					transform_options.object_index = approved != rows
					                                     ? filter_selection.get_index(other_options.object_index)
					                                     : other_options.object_index;
				}
				rows = approved;
			}
		} else if (record_type == JSONRecordType::RECORDS) {
			success =
			    JSONTransform::TransformObject(values, scratch, rows, columns.names, result_vectors, transform_options,
			                                   columns.column_indices, transform_options.error_unknown_key);
		} else if (record_type == JSONRecordType::FEATURES) {
			success = TransformGeoJSONFeatures(values, scratch, rows, columns, result_vectors, transform_options);
		} else {
			D_ASSERT(record_type == JSONRecordType::VALUES);
			success = JSONTransform::Transform(values, scratch, *result_vectors[0], rows, transform_options,
			                                   columns.column_indices[0]);
		}
		if (!success) {
			string hint =
			    options.auto_detect
			        ? "\nTry increasing 'sample_size', reducing 'maximum_depth', specifying 'columns', 'format' or "
			          "'records' manually, setting 'ignore_errors' to true, or setting 'union_by_name' to true when "
			          "reading multiple files with a different structure."
			        : "\nTry setting 'auto_detect' to true, specifying 'format' or 'records' manually, or setting "
			          "'ignore_errors' to true.";
			TransformError(unit_records - count + transform_options.object_index,
			               transform_options.error_message + hint);
			return 0;
		}
	}
	FillRowNumbers(output, rows);
	if (!late && !columns.filters.empty()) {
		rows = ApplyFilters(output, rows, false);
	}
	output.SetChildCardinality(rows);
	return rows;
}

idx_t JsonScan::DecodeObjects(DataChunk &output, idx_t rows) {
	auto &columns = global.columns;
	if (!columns.names.empty()) {
		HoldWindow(output.data[columns.column_ids[0]]);
		auto strings = FlatVector::Writer<string_t>(output.data[columns.column_ids[0]], rows);
		for (idx_t i = 0; i < rows; i++) {
			if (roots[i] != DConstants::INVALID_INDEX) {
				strings.WriteStringRef(string_t(data + starts[i], UnsafeNumericCast<uint32_t>(sizes[i])));
			} else {
				strings.WriteNull();
			}
		}
	}
	FillRowNumbers(output, rows);
	if (!columns.filters.empty()) {
		rows = ApplyFilters(output, rows, false);
	}
	output.SetChildCardinality(rows);
	return rows;
}

idx_t JsonScan::Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output) {
	auto &columns = global.columns;
	ResetBatch();
	RowLookup lookup(*global.ranges);
	idx_t found = 0;
	for (idx_t i = 0; i < offsets.size(); i++) {
		if (offsets[i] < 0) {
			continue;
		}
		const auto offset = UnsafeNumericCast<idx_t>(offsets[i]);
		const auto position = lookup.Find(offset, offset, JSON_LOOKUP_BYTES);
		if (!position.IsValid()) {
			continue;
		}
		const idx_t begin = position.GetIndex();
		const idx_t record_end = lookup.ReadUntil(JSON_LOOKUP_BYTES, [&]() {
			const char *start = lookup.bytes->data + begin;
			const idx_t available = lookup.End() - begin;
			const char *end = global.format == JSONFormat::NEWLINE_DELIMITED ? NextNewline(start, available)
			                                                                 : NextJSON(start, available);
			if (end) {
				return optional_idx(begin + NumericCast<idx_t>(end - start));
			}
			return available > options.maximum_object_size ? optional_idx(lookup.End()) : optional_idx();
		});
		if (record_end == begin) {
			continue;
		}
		const char *start = lookup.bytes->data + begin;
		JSONParseError error;
		auto root = parser.ParseAppend(start, record_end - begin, lookup.bytes->size - begin + JSON_PADDING, error);
		if (!root.IsValid()) {
			continue;
		}
		idx_t trimmed = record_end - begin;
		while (trimmed != 0 && StringUtil::CharacterIsSpace(start[trimmed - 1])) {
			trimmed--;
		}
		roots[found] = root.GetIndex();
		starts[found] = begin;
		sizes[found] = trimmed;
		survivors[found] = i;
		found++;
	}
	for (idx_t r = 0; r < found; r++) {
		values[r] = parser.Node(roots[r]);
	}
	if (columns.file_row_number != DConstants::INVALID_INDEX) {
		auto numbers = FlatVector::Writer<int64_t>(output.data[columns.file_row_number], found);
		for (idx_t r = 0; r < found; r++) {
			numbers.WriteValue(offsets[survivors[r]]);
		}
	}
	if (!columns.column_ids.empty() && found != 0) {
		if (options.type == JSONScanType::READ_JSON_OBJECTS) {
			auto &vector = output.data[columns.column_ids[0]];
			StringVector::AddAuxiliaryData(vector, make_uniq<RowBytesHolder>(lookup.bytes));
			auto strings = FlatVector::Writer<string_t>(vector, found);
			for (idx_t r = 0; r < found; r++) {
				strings.WriteStringRef(string_t(lookup.bytes->data + starts[r], UnsafeNumericCast<uint32_t>(sizes[r])));
			}
		} else {
			vector<Vector *> result_vectors(columns.column_ids.size());
			for (idx_t c = 0; c < columns.column_ids.size(); c++) {
				result_vectors[c] = &output.data[columns.column_ids[c]];
			}
			JSONTransformScratch scratch {arena, writer};
			if (options.record_type == JSONRecordType::RECORDS) {
				JSONTransform::TransformObject(values, scratch, found, columns.names, result_vectors, transform_options,
				                               columns.column_indices, transform_options.error_unknown_key);
			} else {
				D_ASSERT(options.record_type == JSONRecordType::VALUES);
				optional_ptr<const ColumnIndex> column_index =
				    columns.column_indices.empty() ? nullptr : &columns.column_indices[0];
				JSONTransform::Transform(values, scratch, *result_vectors[0], found, transform_options, column_index);
			}
		}
	}
	if (!columns.filters.empty() && found != 0) {
		const auto approved = ApplyFilters(output, found, false);
		if (approved != found) {
			for (idx_t r = 0; r < approved; r++) {
				survivors[r] = survivors[filter_selection.get_index(r)];
			}
			found = approved;
		}
	}
	output.SetCardinality(found);
	return found;
}

} // namespace duckdb
