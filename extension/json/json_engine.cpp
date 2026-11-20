#include "json_engine.hpp"

#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/formats/row_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/caching_mode.hpp"
#include "duckdb/storage/table/column_segment.hpp"

#include <cmath>
#include <type_traits>

namespace duckdb {

namespace {

constexpr idx_t JSON_MIN_RANGE_BYTES = 1024 * 1024;
constexpr idx_t JSON_MAX_RANGE_BYTES = 8 * 1024 * 1024;
constexpr idx_t JSON_STREAM_BYTES = 8 * 1024 * 1024;
constexpr idx_t JSON_LOOKUP_BYTES = 64 * 1024;
constexpr idx_t JSON_PADDING = JSONTreeParser::PADDING;

void SkipWhitespace(const char *data, idx_t &offset, idx_t size) {
	while (offset < size && StringUtil::CharacterIsSpace(data[offset])) {
		offset++;
	}
}

const char *NextNewline(const char *ptr, idx_t size) {
	return const_char_ptr_cast(memchr(ptr, '\n', size));
}

const char *NextJSONDefault(const char *ptr, const char *const end) {
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

JsonDirectKind DirectKind(const LogicalType &type, const ColumnIndex &index) {
	if (index.ChildIndexCount() != 0 || type.HasAlias()) {
		return JsonDirectKind::TREE;
	}
	if (JSONTransform::ParsesFromString(type)) {
		return JsonDirectKind::STRING;
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::VARCHAR:
		return JsonDirectKind::VALUE;
	default:
		return JsonDirectKind::TREE;
	}
}

std::string_view TrimToken(std::string_view token) {
	while (!token.empty() && StringUtil::CharacterIsSpace(token.back())) {
		token.remove_suffix(1);
	}
	return token;
}

bool PlainString(std::string_view content) {
	constexpr uint64_t ONES = 0x0101010101010101ULL;
	constexpr uint64_t HIGHS = 0x8080808080808080ULL;
	const char *bytes = content.data();
	idx_t remaining = content.size();
	while (true) {
		uint64_t word;
		memcpy(&word, bytes, sizeof(word));
		const uint64_t backslashes = word ^ (ONES * '\\');
		const uint64_t found = (((backslashes - ONES) & ~backslashes) | ((word - ONES * 0x20) & ~word)) & HIGHS;
		if (remaining <= sizeof(word)) {
			return (found & (remaining == sizeof(word) ? ~uint64_t(0) : (uint64_t(1) << (8 * remaining)) - 1)) == 0;
		}
		if (found) {
			return false;
		}
		bytes += sizeof(word);
		remaining -= sizeof(word);
	}
}

void KeyWords(std::string_view key, uint64_t words[2]) {
	memcpy(words, key.data(), 2 * sizeof(uint64_t));
	const idx_t size = key.size();
	words[0] &= size >= 8 ? ~uint64_t(0) : (uint64_t(1) << (8 * size)) - 1;
	words[1] &= size >= 16 ? ~uint64_t(0) : size <= 8 ? 0 : (uint64_t(1) << (8 * (size - 8))) - 1;
}

bool SameKey(const JsonKeyHint &hint, std::string_view key) {
	if (hint.key.size() != key.size()) {
		return false;
	}
	if (key.size() > 16) {
		return memcmp(hint.key.data(), key.data(), key.size()) == 0;
	}
	uint64_t words[2];
	KeyWords(key, words);
	return words[0] == hint.words[0] && words[1] == hint.words[1];
}

template <class T>
bool DirectNumber(simdjson::ondemand::value &value, simdjson::ondemand::json_type type, bool strict, T &result,
                  bool &cast) {
	namespace ondemand = simdjson::ondemand;
	switch (type) {
	case ondemand::json_type::boolean: {
		bool boolean;
		if (value.get_bool().get(boolean)) {
			return false;
		}
		cast = TryCast::Operation<bool, T>(boolean, result, strict);
		return true;
	}
	case ondemand::json_type::number: {
		if constexpr (std::is_same_v<T, int64_t>) {
			if (!value.get_int64().get(result)) {
				cast = true;
				return true;
			}
		} else if constexpr (std::is_same_v<T, double>) {
			const auto token = value.raw_json_token();
			if (!value.get_double().get(result)) {
				if (result == 0 && std::signbit(result) &&
				    TrimToken(token).find_first_of(".eE") == std::string_view::npos) {
					result = 0;
				}
				cast = true;
				return true;
			}
		}
		ondemand::number number;
		if (value.get_number().get(number)) {
			return false;
		}
		switch (number.get_number_type()) {
		case ondemand::number_type::signed_integer:
			cast = TryCast::Operation<int64_t, T>(number.get_int64(), result, strict);
			return true;
		case ondemand::number_type::unsigned_integer:
			cast = TryCast::Operation<uint64_t, T>(number.get_uint64(), result, strict);
			return true;
		case ondemand::number_type::floating_point_number:
			cast = TryCast::Operation<double, T>(number.get_double(), result, strict);
			return true;
		default:
			return false;
		}
	}
	case ondemand::json_type::string: {
		std::string_view text;
		if (value.get_string().get(text)) {
			return false;
		}
		cast = TryCast::Operation<string_t, T>(string_t(text.data(), UnsafeNumericCast<uint32_t>(text.size())), result,
		                                       strict);
		return true;
	}
	default:
		cast = false;
		return true;
	}
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

template <class T>
bool DirectDecimal(simdjson::ondemand::value &value, simdjson::ondemand::json_type type, CastParameters &parameters,
                   uint8_t width, uint8_t scale, T &result, bool &cast) {
	namespace ondemand = simdjson::ondemand;
	switch (type) {
	case ondemand::json_type::boolean: {
		bool boolean;
		if (value.get_bool().get(boolean)) {
			return false;
		}
		cast = TryCastToDecimal::Operation<bool, T>(boolean, result, parameters, width, scale);
		return true;
	}
	case ondemand::json_type::number: {
		const auto token = TrimToken(value.raw_json_token());
		cast = TryCastToDecimal::Operation<string_t, T>(
		    string_t(token.data(), UnsafeNumericCast<uint32_t>(token.size())), result, parameters, width, scale);
		return true;
	}
	case ondemand::json_type::string: {
		std::string_view text;
		if (value.get_string().get(text)) {
			return false;
		}
		cast = TryCastToDecimal::Operation<string_t, T>(string_t(text.data(), UnsafeNumericCast<uint32_t>(text.size())),
		                                                result, parameters, width, scale);
		return true;
	}
	default:
		cast = false;
		return true;
	}
}

} // namespace

JSONFormat JsonDetectFormat(const char *data, idx_t size, JSONTreeParser &parser, JSONRecordType &record_type) {
	auto line_end = NextNewline(data, size);
	if (line_end != nullptr) {
		idx_t line_size = line_end - data;
		SkipWhitespace(data, line_size, size);
		JSONParseError error;
		auto root = parser.Parse(data, line_size, error);
		if (root) {
			if (root->type == JSONValueType::ARRAY && line_size == size) {
				record_type = IsEmptyOrRecordsArray(*root) ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
				return JSONFormat::ARRAY;
			}
			record_type = root->type == JSONValueType::OBJECT ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
			return JSONFormat::NEWLINE_DELIMITED;
		}
	}
	idx_t offset = 0;
	SkipWhitespace(data, offset, size);
	auto remaining = size - offset;
	if (remaining == 0 || data[offset] == '{') {
		record_type = JSONRecordType::RECORDS;
		return JSONFormat::UNSTRUCTURED;
	}
	if (data[offset] != '[') {
		record_type = JSONRecordType::VALUES;
		return JSONFormat::UNSTRUCTURED;
	}
	JSONParseError error;
	idx_t read_size = 0;
	auto root = parser.ParsePrefix(data + offset, remaining, read_size, error);
	if (root) {
		offset += read_size;
		SkipWhitespace(data, offset, size);
		if (offset != size) {
			record_type = JSONRecordType::VALUES;
			return JSONFormat::UNSTRUCTURED;
		}
		record_type = IsEmptyOrRecordsArray(*root) ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
		return JSONFormat::ARRAY;
	}
	offset++;
	SkipWhitespace(data, offset, size);
	record_type = offset == size || data[offset] == '{' ? JSONRecordType::RECORDS : JSONRecordType::VALUES;
	return JSONFormat::ARRAY;
}

void *JsonParserMemory::Allocate(void *context, size_t size) noexcept {
	auto &memory = *static_cast<JsonParserMemory *>(context);
	try {
		return memory.allocator.AllocateData(size);
	} catch (std::exception &ex) {
		memory.error = ErrorData(ex);
		return nullptr;
	}
}

void JsonParserMemory::Deallocate(void *context, void *pointer, size_t size) noexcept {
	static_cast<JsonParserMemory *>(context)->allocator.FreeData(static_cast<data_ptr_t>(pointer), size);
}

JsonSource::JsonSource(ClientContext &context_p, OpenFileInfo file_p, FileCompressionType compression)
    : context(context_p), file(std::move(file_p)) {
	auto &fs = FileSystem::GetFileSystem(context);
	FileOpenFlags flags = FileFlags::FILE_FLAGS_READ | compression;
	flags.SetCachingMode(CachingMode::CACHE_REMOTE_ONLY);
	handle = fs.OpenFile(file, flags);
	pipe = handle->IsPipe();
	compressed = handle->GetFileCompressionType() != FileCompressionType::UNCOMPRESSED;
	if (!compressed) {
		stream = FindRowStream(context, file.path);
	}
	if (stream) {
		pipe = false;
		return;
	}
	positional = !pipe && !compressed && handle->CanSeek();
	remote = !handle->OnDiskFile();
	size = handle->GetFileSize();
}

idx_t JsonSource::ReadAt(char *data, idx_t count, idx_t offset, optional_ptr<FileHandle> thread_handle) {
	D_ASSERT(positional);
	if (offset >= size) {
		return 0;
	}
	count = MinValue(count, size - offset);
	if (count == 0) {
		return 0;
	}
	auto &target = thread_handle ? *thread_handle : *handle;
	target.Read(QueryContext(context), data, count, offset);
	consumed += count;
	return count;
}

idx_t JsonSource::Read(char *data, idx_t count) {
	if (stream) {
		const auto read = stream->Read(data, count, position);
		position += read;
		consumed += read;
		return read;
	}
	if (positional) {
		const auto read = ReadAt(data, count, position);
		position += read;
		return read;
	}
	idx_t total = 0;
	for (idx_t i = 0, start = 0; total < count && replayed < recorded_size && i < recorded.size(); i++) {
		auto &piece = recorded[i];
		const idx_t end = start + piece.GetSize();
		if (replayed < end) {
			const idx_t within = replayed - start;
			const idx_t take = MinValue(count - total, piece.GetSize() - within);
			memcpy(data + total, piece.get() + within, take);
			total += take;
			replayed += take;
		}
		start = end;
	}
	while (total < count) {
		const auto read = handle->Read(QueryContext(context), data + total, count - total);
		if (read == 0) {
			break;
		}
		if (pipe && recording) {
			recorded.emplace_back(BufferAllocator::Get(context).Allocate(read));
			memcpy(recorded.back().get(), data + total, read);
			recorded_size += read;
			replayed += read;
		}
		total += read;
	}
	consumed += total;
	return total;
}

void JsonSource::Release(idx_t offset) {
	if (stream) {
		stream->Release(offset);
	}
}

void JsonSource::Rewind() {
	consumed = 0;
	position = 0;
	if (pipe) {
		replayed = 0;
		return;
	}
	if (!positional && !stream) {
		handle->Reset();
	}
}

double JsonSource::Progress() const {
	if (size == 0) {
		return 0;
	}
	const idx_t position = compressed ? handle->GetProgress() : consumed.load();
	return MinValue<double>(static_cast<double>(position) / static_cast<double>(size), 1.0);
}

unique_ptr<FileHandle> JsonSource::OpenThreadHandle() const {
	auto &fs = FileSystem::GetFileSystem(context);
	FileOpenFlags flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_DIRECT_IO;
	flags.SetCachingMode(CachingMode::CACHE_REMOTE_ONLY);
	return fs.OpenFile(file, flags);
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
	if (format == JSONFormat::NEWLINE_DELIMITED && parallel && source->Positional()) {
		mode = JsonUnitMode::RANGES;
		const idx_t threads = MaxValue<idx_t>(TaskScheduler::QueryThreads(context), 1);
		range_bytes = MinValue(JSON_MAX_RANGE_BYTES, MaxValue(JSON_MIN_RANGE_BYTES, source->Size() / threads));
		range_count = MaxValue<idx_t>((source->Size() + range_bytes - 1) / range_bytes, 1);
		unit_records.assign(range_count, -1);
	} else if (format == JSONFormat::NEWLINE_DELIMITED && parallel) {
		mode = JsonUnitMode::STREAM;
	} else {
		mode = JsonUnitMode::SEQUENTIAL;
	}
	validate_only = options.type == JSONScanType::READ_JSON_OBJECTS ||
	                (columns.names.empty() && options.type == JSONScanType::READ_JSON);
	PlanDirect();
}

void JsonScanGlobal::PlanDirect() {
	direct = options.type == JSONScanType::READ_JSON && options.record_type == JSONRecordType::RECORDS &&
	         format == JSONFormat::NEWLINE_DELIMITED && !columns.names.empty() && columns.feature_columns.empty();
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
		const auto id = columns.types[i].id();
		if (kinds[i] == JsonDirectKind::STRING || (kinds[i] == JsonDirectKind::VALUE && id == LogicalTypeId::VARCHAR)) {
			fast[i] = JsonDirectFast::STRING;
		} else if (kinds[i] == JsonDirectKind::VALUE && id == LogicalTypeId::BIGINT) {
			fast[i] = JsonDirectFast::BIGINT;
		} else if (kinds[i] == JsonDirectKind::VALUE && id == LogicalTypeId::DOUBLE) {
			fast[i] = JsonDirectFast::DOUBLE;
		}
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
	idx_t want = options.maximum_object_size * 2;
	if (source->Positional()) {
		want = MinValue(want, source->Size());
	}
	auto detect = BufferAllocator::Get(context).Allocate(want + JSON_PADDING);
	auto data = char_ptr_cast(detect.get());
	const idx_t read = source->Positional() ? source->ReadAt(data, want, 0) : source->Read(data, want);
	source->Rewind();
	if (read == 0) {
		format = JSONFormat::NEWLINE_DELIMITED;
		return;
	}
	JSONTreeParser parser(read_flags, BufferAllocator::Get(context));
	idx_t features_offset;
	if (options.geojson.value_or(false) && FindFeatureCollectionArray(data, read, features_offset)) {
		features_prefix = true;
		format = JSONFormat::ARRAY;
		return;
	}
	JSONRecordType record_type;
	format = JsonDetectFormat(data, read, parser, record_type);
}

idx_t JsonScanGlobal::MaxThreads() const {
	switch (mode) {
	case JsonUnitMode::RANGES:
		return range_count;
	case JsonUnitMode::STREAM:
		return bind.max_threads.IsValid() ? bind.max_threads.GetIndex() : GlobalTableFunctionState::MAX_THREADS;
	default:
		return 1;
	}
}

idx_t JsonScanGlobal::BufferCapacity() const {
	return options.maximum_object_size * 2;
}

bool JsonScanGlobal::Claim(idx_t &unit) {
	lock_guard<mutex> guard(lock);
	if (error_unit.IsValid()) {
		return false;
	}
	if (mode == JsonUnitMode::RANGES) {
		if (next_unit >= range_count) {
			return false;
		}
		unit = next_unit++;
		return true;
	}
	D_ASSERT(mode == JsonUnitMode::SEQUENTIAL);
	if (next_unit != 0) {
		return false;
	}
	unit = next_unit++;
	unit_records.push_back(-1);
	return true;
}

bool JsonScanGlobal::ClaimChunk(idx_t &unit, AllocatedData &buffer, idx_t &size, idx_t &file_offset, bool &last) {
	lock_guard<mutex> guard(lock);
	if (error_unit.IsValid() || stream_done) {
		return false;
	}
	auto &allocator = BufferAllocator::Get(context);
	idx_t filled = carry_size;
	idx_t want = JSON_STREAM_BYTES;
	while (true) {
		const idx_t capacity = filled + want + JSON_PADDING;
		if (buffer.GetSize() < capacity) {
			auto grown = allocator.Allocate(capacity);
			if (filled > carry_size) {
				memcpy(grown.get(), buffer.get(), filled);
			}
			buffer = std::move(grown);
		}
		auto data = char_ptr_cast(buffer.get());
		if (filled == carry_size && carry_size != 0) {
			memcpy(data, carry.get(), carry_size);
		}
		const idx_t read = source->Read(data + filled, want);
		filled += read;
		if (read < want) {
			last = true;
			stream_done = true;
			size = filled;
			carry_size = 0;
			break;
		}
		const char *newline = nullptr;
		for (idx_t i = filled; i > 0; i--) {
			if (data[i - 1] == '\n') {
				newline = data + i - 1;
				break;
			}
		}
		if (newline) {
			last = false;
			size = NumericCast<idx_t>(newline - data) + 1;
			carry_size = filled - size;
			if (carry.GetSize() < carry_size) {
				carry = allocator.Allocate(MaxValue<idx_t>(carry_size, carry.GetSize() * 2));
			}
			memcpy(carry.get(), data + size, carry_size);
			break;
		}
		if (filled > options.maximum_object_size) {
			ThrowObjectSizeError(filled);
		}
		want = filled;
	}
	memset(char_ptr_cast(buffer.get()) + size, 0, MinValue<idx_t>(JSON_PADDING, buffer.GetSize() - size));
	source->Release(source->Position());
	file_offset = stream_offset;
	stream_offset += size;
	unit = next_unit++;
	unit_records.push_back(-1);
	return true;
}

void JsonScanGlobal::Finish(idx_t unit, idx_t records) {
	lock_guard<mutex> guard(lock);
	if (unit >= unit_records.size()) {
		unit_records.resize(unit + 1, -1);
	}
	unit_records[unit] = NumericCast<int64_t>(records);
	ThrowIfPossible();
}

void JsonScanGlobal::AddError(idx_t unit, idx_t record, string message) {
	lock_guard<mutex> guard(lock);
	if (!error_unit.IsValid() || unit < error_unit.GetIndex() ||
	    (unit == error_unit.GetIndex() && record < error_record)) {
		error_unit = unit;
		error_record = record;
		error_message = std::move(message);
	}
	ThrowIfPossible();
}

void JsonScanGlobal::ThrowIfPossible() {
	if (!error_unit.IsValid()) {
		return;
	}
	idx_t line = error_record;
	for (idx_t unit = 0; unit < error_unit.GetIndex(); unit++) {
		if (unit >= unit_records.size() || unit_records[unit] < 0) {
			return;
		}
		line += NumericCast<idx_t>(unit_records[unit]);
	}
	throw InvalidInputException(StringUtil::Replace(error_message, "{line}", to_string(line + 1)));
}

void JsonScanGlobal::ThrowObjectSizeError(idx_t object_size) const {
	throw InvalidInputException(
	    "\"maximum_object_size\" of %llu bytes exceeded while reading file \"%s\" (>%llu bytes)."
	    "\n Try increasing \"maximum_object_size\".",
	    options.maximum_object_size, source->Path(), object_size);
}

JsonScan::JsonScan(JsonScanGlobal &global_p)
    : global(global_p), options(global_p.options), arena(BufferAllocator::Get(global_p.context)),
      parser(global_p.read_flags, BufferAllocator::Get(global_p.context)), writer(JSONCommon::WRITE_FLAG),
      transform_options(global_p.columns.transform_options), direct_memory(BufferAllocator::Get(global_p.context)) {
	transform_options.parameters.context = &global.context;
	for (auto &filter : global.columns.filters) {
		filter_states.push_back(TableFilterState::Initialize(global.context, filter.filter.get()));
	}
	filter_selection.Initialize(STANDARD_VECTOR_SIZE);
	if (global.mode == JsonUnitMode::RANGES && global.source->Remote()) {
		thread_handle = global.source->OpenThreadHandle();
	}
	if (global.direct) {
		direct_parser.allocator = &direct_memory.callbacks;
		direct_parser.allow_trailing_commas = HasFlag(global.read_flags, JSONReadFlags::ALLOW_TRAILING_COMMAS);
		direct_parser.allow_inf_and_nan = HasFlag(global.read_flags, JSONReadFlags::ALLOW_INF_AND_NAN);
		walk_parser.allocator = &direct_memory.callbacks;
		walk_parser.allow_trailing_commas = direct_parser.allow_trailing_commas;
		walk_parser.allow_inf_and_nan = direct_parser.allow_inf_and_nan;
		candidates.Initialize(STANDARD_VECTOR_SIZE);
		const idx_t column_count = global.columns.names.size();
		direct_raw.resize(column_count);
		direct_found.resize(column_count);
		for (idx_t i = 0; i < column_count; i++) {
			direct_strings.emplace_back(LogicalType::VARCHAR, idx_t(STANDARD_VECTOR_SIZE));
			if (global.kinds[i] == JsonDirectKind::TREE) {
				direct_raw[i].resize(STANDARD_VECTOR_SIZE);
			}
		}
	}
}

void JsonScan::EnsureCapacity(idx_t needed) {
	needed += JSON_PADDING;
	if (buffer.GetSize() >= needed) {
		return;
	}
	auto grown = BufferAllocator::Get(global.context).Allocate(MaxValue(needed, buffer.GetSize() * 2));
	if (size != 0) {
		memcpy(grown.get(), buffer.get(), size);
	}
	buffer = std::move(grown);
	data = char_ptr_cast(buffer.get());
}

bool JsonScan::Claim() {
	claimed = false;
	done = false;
	stopped = false;
	unit_records = 0;
	size = 0;
	cursor = 0;
	file_offset = 0;
	if (global.mode == JsonUnitMode::STREAM) {
		if (!global.ClaimChunk(unit, buffer, size, file_offset, last)) {
			return false;
		}
		data = char_ptr_cast(buffer.get());
		limit = size;
		first = unit == 0;
		Begin();
	} else {
		if (!global.Claim(unit)) {
			return false;
		}
		loaded = false;
	}
	claimed = true;
	return true;
}

void JsonScan::Load() {
	auto &source = *global.source;
	switch (global.mode) {
	case JsonUnitMode::RANGES: {
		const idx_t begin = global.RangeBegin(unit);
		const idx_t end = global.RangeEnd(unit);
		const idx_t read_begin = begin == 0 ? 0 : begin - 1;
		EnsureCapacity(end - read_begin + JSON_LOOKUP_BYTES);
		size = source.ReadAt(data, end - read_begin, read_begin, thread_handle);
		memset(data + size, 0, JSON_PADDING);
		file_offset = read_begin;
		limit = end - read_begin;
		last = end == source.Size();
		first = begin == 0;
		if (!first) {
			auto newline = NextNewline(data, size);
			cursor = newline ? NumericCast<idx_t>(newline - data) + 1 : size;
			if (!newline) {
				done = true;
			}
		}
		break;
	}
	default: {
		EnsureCapacity(global.BufferCapacity());
		first = true;
		last = false;
		limit = NumericLimits<idx_t>::Maximum();
		ReadMore();
		break;
	}
	}
	Begin();
}

void JsonScan::Begin() {
	if (first) {
		StringUtil::SkipBOM(data, size, cursor);
		if (global.format == JSONFormat::ARRAY) {
			SkipArrayStart();
		}
	}
	line_start = cursor;
	loaded = true;
}

void JsonScan::Release() {
	claimed = false;
}

bool JsonScan::ReadMore() {
	if (last) {
		return false;
	}
	auto &source = *global.source;
	if (global.mode == JsonUnitMode::RANGES) {
		const idx_t extra = MaxValue<idx_t>(JSON_LOOKUP_BYTES, size > limit ? size - limit : 0);
		EnsureCapacity(size + extra);
		const idx_t read = source.ReadAt(data + size, extra, file_offset + size, thread_handle);
		size += read;
		memset(data + size, 0, JSON_PADDING);
		if (read < extra) {
			last = true;
		}
		return read != 0;
	}
	if (global.mode == JsonUnitMode::STREAM) {
		return false;
	}
	const idx_t keep = count != 0 ? MinValue(starts[0], cursor) : cursor;
	if (keep != 0) {
		memmove(data, data + keep, size - keep);
		for (idx_t i = 0; i < count; i++) {
			starts[i] -= keep;
		}
		file_offset += keep;
		size -= keep;
		cursor -= keep;
		line_start = line_start > keep ? line_start - keep : 0;
		source.Release(file_offset);
	}
	idx_t want = global.BufferCapacity() > size ? global.BufferCapacity() - size : 0;
	if (want == 0) {
		want = size;
	}
	EnsureCapacity(size + want);
	const idx_t read = source.Read(data + size, want);
	size += read;
	memset(data + size, 0, JSON_PADDING);
	if (read < want) {
		last = true;
	}
	return read != 0;
}

void JsonScan::SkipArrayStart() {
	SkipWhitespace(data, cursor, size);
	if (cursor == size) {
		return;
	}
	if (global.features_prefix) {
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
	SkipWhitespace(data, cursor, size);
	if (cursor >= size) {
		throw InvalidInputException("Missing closing brace ']' in JSON array with format='array' in file \"%s\"",
		                            global.source->Path());
	}
	if (data[cursor] == ']') {
		cursor++;
		SkipWhitespace(data, cursor, size);
		if (cursor != size && !global.features_prefix) {
			throw InvalidInputException(
			    "Empty array with trailing data when parsing JSON array with format='array' in file \"%s\"",
			    global.source->Path());
		}
		cursor = size;
		done = true;
	}
}

bool JsonScan::FrameRecord(idx_t &record_end) {
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
		if (last) {
			record_end = size;
			return remaining != 0;
		}
		if (remaining > options.maximum_object_size) {
			global.ThrowObjectSizeError(remaining);
		}
		if (!ReadMore()) {
			record_end = size;
			return remaining != 0;
		}
	}
}

void JsonScan::SkipSpace() {
	while (true) {
		while (cursor < size && StringUtil::CharacterIsSpace(data[cursor])) {
			if (data[cursor++] == '\n') {
				line_start = cursor;
				if (line_start >= limit) {
					return;
				}
			}
		}
		if (cursor < size || last || !ReadMore()) {
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
	const idx_t batch_start = file_offset + cursor;
	while (count < STANDARD_VECTOR_SIZE && !done) {
		SkipSpace();
		if (line_start >= limit || cursor == size) {
			done = true;
			break;
		}
		idx_t record_end;
		if (!FrameRecord(record_end)) {
			done = true;
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
		SkipSpace();
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
		output.Reset();
		const auto rows =
		    options.type == JSONScanType::READ_JSON_OBJECTS ? DecodeObjects(output, count) : Decode(output, count);
		if (stopped) {
			return 0;
		}
		if (rows != 0) {
			return rows;
		}
	}
	return 0;
}

idx_t JsonScan::Sample(const JSONNode **&sample, idx_t &bytes) {
	sampling = true;
	if (!claimed && !Claim()) {
		return 0;
	}
	if (!loaded) {
		Load();
	}
	if (stopped || !NextRecords()) {
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
	for (idx_t i = 0; i < filters.size() && approved > 0; i++) {
		auto &vector = output.data[filters[i].output_column];
		FlatVector::SetSize(vector, count_t(rows));
		ColumnSegment::FilterSelection(filter_selection, vector, *filter_states[i], rows, approved);
	}
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

bool JsonScan::DirectValue(idx_t column, simdjson::ondemand::value &value, Vector &vector, idx_t row) {
	namespace ondemand = simdjson::ondemand;
	const auto kind = global.kinds[column];
	if (kind == JsonDirectKind::TREE) {
		std::string_view raw;
		if (value.raw_json().get(raw)) {
			return false;
		}
		direct_raw[column][row] = raw;
		return true;
	}
	ondemand::json_type type;
	if (value.type().get(type) || type == ondemand::json_type::unknown) {
		return false;
	}
	if (type == ondemand::json_type::null) {
		bool is_null = false;
		if (value.is_null().get(is_null) || !is_null) {
			return false;
		}
		direct_validity[column]->SetInvalid(row);
		return true;
	}
	const bool strict = transform_options.strict_cast;
	if (kind == JsonDirectKind::STRING || vector.GetType().id() == LogicalTypeId::VARCHAR) {
		auto &target = kind == JsonDirectKind::STRING ? direct_strings[column] : vector;
		auto strings = reinterpret_cast<string_t *>(direct_data[column]);
		if (type == ondemand::json_type::string) {
			const auto token = TrimToken(value.raw_json_token());
			if (token.size() < 2 || token.front() != '"' || token.back() != '"') {
				return false;
			}
			const auto content = token.substr(1, token.size() - 2);
			if (PlainString(content)) {
				strings[row] = string_t(content.data(), UnsafeNumericCast<uint32_t>(content.size()));
				return true;
			}
			std::string_view text;
			if (value.get_string().get(text)) {
				return false;
			}
			strings[row] = StringVector::AddString(target, text);
			return true;
		}
		if (kind == JsonDirectKind::STRING) {
			if (strict) {
				return false;
			}
			direct_validity[column]->SetInvalid(row);
			return true;
		}
		auto &heap = StringVector::GetStringHeap(vector);
		auto &result = strings[row];
		switch (type) {
		case ondemand::json_type::boolean: {
			bool boolean;
			if (value.get_bool().get(boolean)) {
				return false;
			}
			result = StringCast::Operation<bool>(boolean, heap);
			return true;
		}
		case ondemand::json_type::number: {
			ondemand::number number;
			if (value.get_number().get(number)) {
				return false;
			}
			switch (number.get_number_type()) {
			case ondemand::number_type::signed_integer: {
				const auto integer = number.get_int64();
				result = integer >= 0 ? StringCast::Operation<uint64_t>(UnsafeNumericCast<uint64_t>(integer), heap)
				                      : StringCast::Operation<int64_t>(integer, heap);
				return true;
			}
			case ondemand::number_type::unsigned_integer:
				result = StringCast::Operation<uint64_t>(number.get_uint64(), heap);
				return true;
			case ondemand::number_type::floating_point_number:
				result = StringCast::Operation<double>(number.get_double(), heap);
				return true;
			default:
				return false;
			}
		}
		default:
			return false;
		}
	}
	bool cast = false;
	bool parsed;
	const auto &column_type = vector.GetType();
	const data_ptr_t data = direct_data[column];
	switch (column_type.id()) {
	case LogicalTypeId::BOOLEAN:
		parsed = DirectNumber<bool>(value, type, strict, reinterpret_cast<bool *>(data)[row], cast);
		break;
	case LogicalTypeId::TINYINT:
		parsed = DirectNumber<int8_t>(value, type, strict, reinterpret_cast<int8_t *>(data)[row], cast);
		break;
	case LogicalTypeId::SMALLINT:
		parsed = DirectNumber<int16_t>(value, type, strict, reinterpret_cast<int16_t *>(data)[row], cast);
		break;
	case LogicalTypeId::INTEGER:
		parsed = DirectNumber<int32_t>(value, type, strict, reinterpret_cast<int32_t *>(data)[row], cast);
		break;
	case LogicalTypeId::BIGINT:
		parsed = DirectNumber<int64_t>(value, type, strict, reinterpret_cast<int64_t *>(data)[row], cast);
		break;
	case LogicalTypeId::UTINYINT:
		parsed = DirectNumber<uint8_t>(value, type, strict, reinterpret_cast<uint8_t *>(data)[row], cast);
		break;
	case LogicalTypeId::USMALLINT:
		parsed = DirectNumber<uint16_t>(value, type, strict, reinterpret_cast<uint16_t *>(data)[row], cast);
		break;
	case LogicalTypeId::UINTEGER:
		parsed = DirectNumber<uint32_t>(value, type, strict, reinterpret_cast<uint32_t *>(data)[row], cast);
		break;
	case LogicalTypeId::UBIGINT:
		parsed = DirectNumber<uint64_t>(value, type, strict, reinterpret_cast<uint64_t *>(data)[row], cast);
		break;
	case LogicalTypeId::HUGEINT:
		parsed = DirectNumber<hugeint_t>(value, type, strict, reinterpret_cast<hugeint_t *>(data)[row], cast);
		break;
	case LogicalTypeId::UHUGEINT:
		parsed = DirectNumber<uhugeint_t>(value, type, strict, reinterpret_cast<uhugeint_t *>(data)[row], cast);
		break;
	case LogicalTypeId::FLOAT:
		parsed = DirectNumber<float>(value, type, strict, reinterpret_cast<float *>(data)[row], cast);
		break;
	case LogicalTypeId::DOUBLE:
		parsed = DirectNumber<double>(value, type, strict, reinterpret_cast<double *>(data)[row], cast);
		break;
	case LogicalTypeId::DECIMAL: {
		const auto width = DecimalType::GetWidth(column_type);
		const auto scale = DecimalType::GetScale(column_type);
		auto &parameters = transform_options.parameters;
		switch (column_type.InternalType()) {
		case PhysicalType::INT16:
			parsed = DirectDecimal<int16_t>(value, type, parameters, width, scale,
			                                reinterpret_cast<int16_t *>(data)[row], cast);
			break;
		case PhysicalType::INT32:
			parsed = DirectDecimal<int32_t>(value, type, parameters, width, scale,
			                                reinterpret_cast<int32_t *>(data)[row], cast);
			break;
		case PhysicalType::INT64:
			parsed = DirectDecimal<int64_t>(value, type, parameters, width, scale,
			                                reinterpret_cast<int64_t *>(data)[row], cast);
			break;
		default:
			parsed = DirectDecimal<hugeint_t>(value, type, parameters, width, scale,
			                                  reinterpret_cast<hugeint_t *>(data)[row], cast);
			break;
		}
		break;
	}
	default:
		return false;
	}
	if (!parsed) {
		return false;
	}
	if (!cast) {
		if (strict) {
			return false;
		}
		direct_validity[column]->SetInvalid(row);
	}
	return true;
}

void JsonScan::NullRow(idx_t row, JsonDirectPass pass, const vector<Vector *> &vectors) {
	for (idx_t column = 0; column < vectors.size(); column++) {
		const bool filtered = global.filtered[column];
		if (!vectors[column] || (pass == JsonDirectPass::FILTERS && !filtered) ||
		    (pass == JsonDirectPass::OTHERS && filtered)) {
			continue;
		}
		const auto kind = global.kinds[column];
		if (kind == JsonDirectKind::TREE) {
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
	memset(direct_found.data(), 0, column_count);
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
		if (direct_found[column] == strength) {
			if (duplicate_error) {
				return FALLBACK;
			}
			continue;
		}
		if (direct_found[column] > strength) {
			continue;
		}
		direct_found[column] = strength;
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
		direct_validity[column]->SetValid(row);
		switch (global.fast[column]) {
		case JsonDirectFast::BIGINT:
			if (!value.get_int64().get(reinterpret_cast<int64_t *>(direct_data[column])[row])) {
				continue;
			}
			break;
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
			if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
				const auto content = token.substr(1, token.size() - 2);
				if (PlainString(content)) {
					reinterpret_cast<string_t *>(direct_data[column])[row] =
					    string_t(content.data(), UnsafeNumericCast<uint32_t>(content.size()));
					continue;
				}
			}
			break;
		}
		default:
			break;
		}
		if (!DirectValue(column, value, *vectors[column], row)) {
			return FALLBACK;
		}
	}
	if (!document.at_end()) {
		return FALLBACK;
	}
	for (idx_t column = 0; column < column_count; column++) {
		if (direct_found[column] || skips[column] || !vectors[column]) {
			continue;
		}
		if (direct_missing_error && pass != JsonDirectPass::OTHERS) {
			return FALLBACK;
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
		const bool strings = global.kinds[column] == JsonDirectKind::STRING;
		if (strings) {
			direct_strings[column].Initialize(VectorDataInitialization::UNINITIALIZED, STANDARD_VECTOR_SIZE);
		}
		auto &target = strings ? direct_strings[column] : *vectors[column];
		direct_data[column] = FlatVector::GetDataMutable(target);
		direct_validity[column] = &FlatVector::ValidityMutable(target);
	}
	direct_unknown_error = transform_options.error_unknown_key;
	direct_duplicate_error = transform_options.error_duplicate_key;
	direct_missing_error = transform_options.error_missing_key;
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
	auto &source = *global.source;
	ResetBatch();
	if (!thread_handle && source.Remote()) {
		thread_handle = source.OpenThreadHandle();
	}
	idx_t found = 0;
	idx_t window_begin = 0;
	idx_t window_size = 0;
	char *window = nullptr;
	for (idx_t i = 0; i < offsets.size(); i++) {
		const auto offset = UnsafeNumericCast<idx_t>(offsets[i]);
		idx_t want = JSON_LOOKUP_BYTES;
		const char *start = nullptr;
		const char *end = nullptr;
		idx_t available = 0;
		while (true) {
			if (!window || offset < window_begin || offset >= window_begin + window_size) {
				window = char_ptr_cast(arena.AllocateAligned(want + JSON_PADDING));
				window_size = source.ReadAt(window, want, offset, thread_handle);
				memset(window + window_size, 0, JSON_PADDING);
				window_begin = offset;
			}
			start = window + (offset - window_begin);
			available = window_size - (offset - window_begin);
			if (available == 0) {
				break;
			}
			end = global.format == JSONFormat::NEWLINE_DELIMITED ? NextNewline(start, available)
			                                                     : NextJSON(start, available);
			const bool eof = window_begin + window_size >= source.Size();
			if (end || eof || want > options.maximum_object_size) {
				if (!end) {
					end = start + available;
				}
				break;
			}
			want *= 2;
			window = nullptr;
		}
		if (!end || end == start) {
			continue;
		}
		JSONParseError error;
		auto root = parser.ParseAppend(start, NumericCast<idx_t>(end - start), available + JSON_PADDING, error);
		if (!root.IsValid()) {
			continue;
		}
		idx_t trimmed = NumericCast<idx_t>(end - start);
		while (trimmed != 0 && StringUtil::CharacterIsSpace(start[trimmed - 1])) {
			trimmed--;
		}
		roots[found] = root.GetIndex();
		found_records[found] = start;
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
			auto strings = FlatVector::Writer<string_t>(output.data[columns.column_ids[0]], found);
			for (idx_t r = 0; r < found; r++) {
				strings.WriteStringRef(string_t(found_records[r], UnsafeNumericCast<uint32_t>(sizes[r])));
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
