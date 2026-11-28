#include "json_direct.hpp"

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/swar.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/formats/value_parsers.hpp"

#include <cmath>

namespace duckdb {

namespace {

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

void InvalidateRows(ValidityMask &validity, idx_t from, idx_t capacity) {
	const idx_t aligned = MinValue(AlignValue<idx_t, ValidityMask::BITS_PER_VALUE>(from), capacity);
	for (idx_t row = from; row < aligned; row++) {
		validity.SetInvalid(row);
	}
	if (aligned < capacity) {
		validity.SetRangeInvalid(capacity, aligned / ValidityMask::BITS_PER_VALUE, ValidityMask::EntryCount(capacity));
	}
}

} // namespace

JsonDirectKind DirectKind(const LogicalType &type) {
	if (type.HasAlias()) {
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

JsonDirectKind DirectKind(const LogicalType &type, const ColumnIndex &index) {
	if (index.ChildIndexCount() != 0) {
		return JsonDirectKind::TREE;
	}
	return DirectKind(type);
}

JsonDirectFast DirectFast(JsonDirectKind kind, LogicalTypeId id) {
	if (kind == JsonDirectKind::STRING || (kind == JsonDirectKind::VALUE && id == LogicalTypeId::VARCHAR)) {
		return JsonDirectFast::STRING;
	}
	if (kind == JsonDirectKind::VALUE && id == LogicalTypeId::BIGINT) {
		return JsonDirectFast::BIGINT;
	}
	if (kind == JsonDirectKind::VALUE && id == LogicalTypeId::INTEGER) {
		return JsonDirectFast::INTEGER;
	}
	if (kind == JsonDirectKind::VALUE && id == LogicalTypeId::DOUBLE) {
		return JsonDirectFast::DOUBLE;
	}
	return JsonDirectFast::NONE;
}

std::string_view TrimToken(std::string_view token) {
	while (!token.empty() && StringUtil::CharacterIsSpace(token.back())) {
		token.remove_suffix(1);
	}
	return token;
}

bool PlainString(std::string_view content) {
	const char *bytes = content.data();
	idx_t remaining = content.size();
	while (true) {
		uint64_t word;
		memcpy(&word, bytes, sizeof(word));
		const uint64_t found =
		    SwarWord::MaybeZeroBytes(word ^ SwarWord::Repeat('\\')) | SwarWord::MaybeBelow(word, 0x20);
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

idx_t PlanJsonNode(vector<JsonDirectNode> &nodes, const LogicalType &type, optional_ptr<const ColumnIndex> index) {
	if (index && index->ChildIndexCount() == 0) {
		index = nullptr;
	}
	JsonDirectNode node;
	node.column_index = index;
	if (type.HasAlias()) {
		node.kind = JsonDirectKind::TREE;
	} else if (type.id() == LogicalTypeId::STRUCT && StructType::GetChildCount(type) != 0) {
		node.kind = JsonDirectKind::STRUCT;
		const idx_t child_count = StructType::GetChildCount(type);
		const idx_t projected = index ? index->ChildIndexCount() : child_count;
		vector<bool> kept(child_count, false);
		for (idx_t i = 0; i < projected; i++) {
			const idx_t slot = index ? index->GetChildIndex(i).GetPrimaryIndex() : i;
			const auto &name = StructType::GetChildName(type, slot);
			kept[slot] = true;
			node.slots.push_back(slot);
			node.keys.emplace(name, i);
			node.folded_keys.emplace(name, i);
			node.children.push_back(
			    PlanJsonNode(nodes, StructType::GetChildType(type, slot), index ? &index->GetChildIndex(i) : nullptr));
		}
		for (idx_t slot = 0; slot < child_count; slot++) {
			if (!kept[slot]) {
				node.unprojected.push_back(slot);
			}
		}
		node.unknown_key_error = projected == child_count;
	} else if (type.id() == LogicalTypeId::LIST) {
		node.kind = JsonDirectKind::LIST;
		node.child = PlanJsonNode(nodes, ListType::GetChildType(type), nullptr);
	} else if (type.id() == LogicalTypeId::MAP && MapType::KeyType(type).id() == LogicalTypeId::VARCHAR &&
	           !MapType::KeyType(type).HasAlias()) {
		node.kind = JsonDirectKind::MAP;
		node.child = PlanJsonNode(nodes, MapType::ValueType(type), nullptr);
	} else {
		node.kind = DirectKind(type);
		node.fast = DirectFast(node.kind, type.id());
	}
	nodes.push_back(std::move(node));
	return nodes.size() - 1;
}

JsonDirectDecoder::JsonDirectDecoder(const vector<JsonDirectNode> &nodes_p, JSONTransformOptions &options_p,
                                     JSONTreeParser &parser_p, JSONStringWriter &writer_p)
    : nodes(nodes_p), options(options_p), parser(parser_p), writer(writer_p), states(nodes_p.size()) {
}

void JsonDirectDecoder::Bind(idx_t id, Vector &vector, idx_t capacity) {
	auto &node = nodes[id];
	auto &state = states[id];
	state.target = &vector;
	switch (node.kind) {
	case JsonDirectKind::STRING:
		if (!state.strings) {
			state.strings = make_uniq<Vector>(LogicalType::VARCHAR, capacity);
		} else {
			state.strings->Initialize(VectorDataInitialization::UNINITIALIZED, capacity);
		}
		break;
	case JsonDirectKind::STRUCT: {
		auto &entries = StructVector::GetEntries(vector);
		state.found.Resize(node.children.size());
		for (idx_t i = 0; i < node.children.size(); i++) {
			Bind(node.children[i], entries[node.slots[i]], capacity);
		}
		state.validity = &FlatVector::ValidityMutable(vector);
		InvalidateRows(*state.validity, 0, capacity);
		return;
	}
	case JsonDirectKind::LIST:
		state.list_size = 0;
		state.list_capacity = ListVector::GetListCapacity(vector);
		Bind(node.child, ListVector::GetChildMutable(vector), state.list_capacity);
		break;
	case JsonDirectKind::MAP:
		state.list_size = 0;
		state.list_capacity = ListVector::GetListCapacity(vector);
		Bind(node.child, MapVector::GetValues(vector), state.list_capacity);
		state.keys = FlatVector::GetDataMutable<string_t>(MapVector::GetKeys(vector));
		break;
	default:
		break;
	}
	Refresh(id, 0, capacity);
}

void JsonDirectDecoder::Grow(idx_t id) {
	auto &node = nodes[id];
	auto &state = states[id];
	ListVector::Reserve(*state.target, MaxValue<idx_t>(2 * state.list_capacity, STANDARD_VECTOR_SIZE));
	const idx_t from = state.list_capacity;
	state.list_capacity = ListVector::GetListCapacity(*state.target);
	Refresh(node.child, from, state.list_capacity);
	if (node.kind == JsonDirectKind::MAP) {
		state.keys = FlatVector::GetDataMutable<string_t>(MapVector::GetKeys(*state.target));
	}
}

void JsonDirectDecoder::Refresh(idx_t id, idx_t from, idx_t capacity) {
	auto &node = nodes[id];
	auto &state = states[id];
	switch (node.kind) {
	case JsonDirectKind::TREE:
		state.raw.resize(MaxValue<idx_t>(state.raw.size(), capacity));
		std::fill(state.raw.begin() + NumericCast<int64_t>(from), state.raw.begin() + NumericCast<int64_t>(capacity),
		          std::string_view());
		return;
	case JsonDirectKind::STRING:
		state.strings->Reserve(capacity);
		state.data = FlatVector::GetDataMutable(*state.strings);
		state.validity = &FlatVector::ValidityMutable(*state.strings);
		break;
	case JsonDirectKind::STRUCT:
		state.validity = &FlatVector::ValidityMutable(*state.target);
		for (auto child : node.children) {
			Refresh(child, from, capacity);
		}
		break;
	default:
		state.data = FlatVector::GetDataMutable(*state.target);
		state.validity = &FlatVector::ValidityMutable(*state.target);
		break;
	}
	InvalidateRows(*state.validity, from, capacity);
}

bool JsonDirectDecoder::Decode(idx_t id, simdjson::ondemand::value &value, idx_t row) {
	switch (nodes[id].kind) {
	case JsonDirectKind::STRUCT:
		return Struct(id, value, row);
	case JsonDirectKind::LIST:
		return List(id, value, row);
	case JsonDirectKind::MAP:
		return Map(id, value, row);
	default:
		return Leaf(id, value, row);
	}
}

bool JsonDirectDecoder::Struct(idx_t id, simdjson::ondemand::value &value, idx_t row) {
	namespace ondemand = simdjson::ondemand;
	auto &node = nodes[id];
	auto &state = states[id];
	ondemand::json_type type;
	if (value.type().get(type)) {
		return false;
	}
	if (type == ondemand::json_type::null) {
		bool is_null = false;
		return !value.is_null().get(is_null) && is_null;
	}
	ondemand::object object;
	if (type != ondemand::json_type::object || value.get_object().get(object)) {
		return false;
	}
	state.validity->SetValid(row);
	auto found = state.found.Data();
	memset(found, 0, state.found.Size());
	idx_t position = 0;
	for (auto field_result : object) {
		ondemand::field field;
		if (std::move(field_result).get(field)) {
			return false;
		}
		const auto key = field.escaped_key();
		if (position == state.hints.size()) {
			state.hints.emplace_back();
		}
		auto &hint = state.hints[position++];
		if (hint.strength == 0 || !SameKey(hint, key)) {
			if (memchr(key.data(), '\\', key.size())) {
				return false;
			}
			hint.key.assign(key.data(), key.size());
			KeyWords(key, hint.words);
			hint.column = DConstants::INVALID_INDEX;
			hint.strength = 3;
			auto exact = node.keys.find(key);
			if (exact != node.keys.end()) {
				hint.column = exact->second;
				hint.strength = 2;
			} else if (node.folded_keys.find(key) != node.folded_keys.end()) {
				hint.strength = 1;
			}
		}
		if (hint.strength == 1) {
			return false;
		}
		if (hint.column == DConstants::INVALID_INDEX) {
			if (node.unknown_key_error && options.error_unknown_key) {
				return false;
			}
			skipped = true;
			continue;
		}
		if (found[hint.column]) {
			if (options.error_duplicate_key) {
				return false;
			}
			skipped = true;
			continue;
		}
		found[hint.column] = 1;
		auto child = std::move(field).value();
		if (!Decode(node.children[hint.column], child, row)) {
			return false;
		}
	}
	if (options.error_missing_key) {
		for (idx_t i = 0; i < node.children.size(); i++) {
			if (!found[i]) {
				return false;
			}
		}
	}
	return true;
}

bool JsonDirectDecoder::List(idx_t id, simdjson::ondemand::value &value, idx_t row) {
	namespace ondemand = simdjson::ondemand;
	auto &node = nodes[id];
	auto &state = states[id];
	ondemand::json_type type;
	if (value.type().get(type)) {
		return false;
	}
	if (type == ondemand::json_type::null) {
		bool is_null = false;
		if (value.is_null().get(is_null) || !is_null) {
			return false;
		}
		state.validity->SetInvalid(row);
		return true;
	}
	if (type != ondemand::json_type::array) {
		if (options.strict_cast) {
			return false;
		}
		state.validity->SetInvalid(row);
		return true;
	}
	ondemand::array array;
	if (value.get_array().get(array)) {
		return false;
	}
	state.validity->SetValid(row);
	const idx_t offset = state.list_size;
	for (auto element_result : array) {
		ondemand::value element;
		if (std::move(element_result).get(element)) {
			return false;
		}
		if (state.list_size == state.list_capacity) {
			Grow(id);
		}
		if (!Decode(node.child, element, state.list_size)) {
			return false;
		}
		state.list_size++;
	}
	auto &entry = reinterpret_cast<list_entry_t *>(state.data)[row];
	entry.offset = offset;
	entry.length = state.list_size - offset;
	return true;
}

bool JsonDirectDecoder::Map(idx_t id, simdjson::ondemand::value &value, idx_t row) {
	namespace ondemand = simdjson::ondemand;
	auto &node = nodes[id];
	auto &state = states[id];
	ondemand::json_type type;
	if (value.type().get(type)) {
		return false;
	}
	if (type == ondemand::json_type::null) {
		bool is_null = false;
		if (value.is_null().get(is_null) || !is_null) {
			return false;
		}
		state.validity->SetInvalid(row);
		return true;
	}
	if (type != ondemand::json_type::object) {
		if (options.strict_cast) {
			return false;
		}
		state.validity->SetInvalid(row);
		return true;
	}
	ondemand::object object;
	if (value.get_object().get(object)) {
		return false;
	}
	state.validity->SetValid(row);
	const idx_t offset = state.list_size;
	auto &keys = MapVector::GetKeys(*state.target);
	for (auto field_result : object) {
		ondemand::field field;
		if (std::move(field_result).get(field)) {
			return false;
		}
		std::string_view key = field.escaped_key();
		if (!PlainString(key) && field.unescaped_key().get(key)) {
			return false;
		}
		if (state.list_size == state.list_capacity) {
			Grow(id);
		}
		state.keys[state.list_size] = key.size() <= string_t::INLINE_LENGTH
		                                  ? string_t(key.data(), UnsafeNumericCast<uint32_t>(key.size()))
		                                  : StringVector::AddString(keys, key);
		auto child = std::move(field).value();
		if (!Decode(node.child, child, state.list_size)) {
			return false;
		}
		state.list_size++;
	}
	auto &entry = reinterpret_cast<list_entry_t *>(state.data)[row];
	entry.offset = offset;
	entry.length = state.list_size - offset;
	return true;
}

bool JsonDirectDecoder::Leaf(idx_t id, simdjson::ondemand::value &value, idx_t row) {
	auto &node = nodes[id];
	auto &state = states[id];
	if (node.kind == JsonDirectKind::TREE) {
		std::string_view raw;
		if (value.raw_json().get(raw)) {
			return false;
		}
		state.raw[row] = raw;
		return true;
	}
	state.validity->SetValid(row);
	switch (node.fast) {
	case JsonDirectFast::BIGINT:
		if (!value.get_int64().get(reinterpret_cast<int64_t *>(state.data)[row])) {
			return true;
		}
		break;
	case JsonDirectFast::INTEGER: {
		int64_t integer;
		if (!value.get_int64().get(integer) && integer >= NumericLimits<int32_t>::Minimum() &&
		    integer <= NumericLimits<int32_t>::Maximum()) {
			reinterpret_cast<int32_t *>(state.data)[row] = static_cast<int32_t>(integer);
			return true;
		}
		break;
	}
	case JsonDirectFast::DOUBLE: {
		const auto token = value.raw_json_token();
		auto &result = reinterpret_cast<double *>(state.data)[row];
		if (!value.get_double().get(result)) {
			if (result == 0 && std::signbit(result) &&
			    TrimToken(token).find_first_of(".eE") == std::string_view::npos) {
				result = 0;
			}
			return true;
		}
		break;
	}
	case JsonDirectFast::STRING: {
		const auto token = TrimToken(value.raw_json_token());
		if (token.size() >= 2 && token.front() == '"') {
			const auto content = token.substr(1, token.size() - 2);
			if (PlainString(content)) {
				MakeString(content.data(), content.size(), readable_end, reinterpret_cast<string_t *>(state.data)[row]);
				return true;
			}
		}
		break;
	}
	default:
		break;
	}
	auto &strings = state.strings ? *state.strings : *state.target;
	return Value(node.kind, state.data, *state.validity, strings, *state.target, value, row);
}

void JsonDirectDecoder::Null(idx_t id, idx_t row) {
	auto &node = nodes[id];
	auto &state = states[id];
	switch (node.kind) {
	case JsonDirectKind::TREE:
		state.raw[row] = std::string_view();
		return;
	case JsonDirectKind::STRUCT:
		state.validity->SetInvalid(row);
		for (auto child : node.children) {
			Null(child, row);
		}
		return;
	default:
		state.validity->SetInvalid(row);
		return;
	}
}

bool JsonDirectDecoder::Finish(idx_t id, idx_t count, JSONTransformScratch &scratch, const char *readable_end) {
	auto &node = nodes[id];
	auto &state = states[id];
	switch (node.kind) {
	case JsonDirectKind::STRING:
		return JSONTransform::TransformStrings(*state.strings, *state.target, count, options);
	case JsonDirectKind::TREE: {
		if (state.target->GetType().IsJSONType()) {
			return WriteJsonValues(state.raw.data(), count, *state.target);
		}
		roots.resize(MaxValue<idx_t>(roots.size(), count));
		values.resize(MaxValue<idx_t>(values.size(), count));
		for (idx_t row = 0; row < count; row++) {
			auto raw = state.raw[row];
			if (raw.empty()) {
				roots[row] = DConstants::INVALID_INDEX;
				continue;
			}
			JSONParseError error;
			const idx_t readable = NumericCast<idx_t>(readable_end - raw.data());
			auto root = parser.ParseAppend(raw.data(), raw.size(), readable, error);
			if (!root.IsValid()) {
				return false;
			}
			roots[row] = root.GetIndex();
		}
		for (idx_t row = 0; row < count; row++) {
			values[row] = roots[row] == DConstants::INVALID_INDEX ? nullptr : parser.Node(roots[row]);
		}
		return JSONTransform::Transform(values.data(), scratch, *state.target, count, options, node.column_index);
	}
	case JsonDirectKind::STRUCT: {
		auto &entries = StructVector::GetEntries(*state.target);
		for (auto slot : node.unprojected) {
			ConstantVector::SetNull(entries[slot], count_t(count));
		}
		for (auto child : node.children) {
			if (!Finish(child, count, scratch, readable_end)) {
				return false;
			}
		}
		return true;
	}
	case JsonDirectKind::LIST:
	case JsonDirectKind::MAP:
		ListVector::SetListSize(*state.target, state.list_size);
		return Finish(node.child, state.list_size, scratch, readable_end);
	default:
		return true;
	}
}

bool JsonDirectDecoder::WriteJsonValues(const std::string_view *raws, idx_t count, Vector &target) {
	auto values = FlatVector::GetDataMutable<string_t>(target);
	auto &validity = FlatVector::ValidityMutable(target);
	for (idx_t row = 0; row < count; row++) {
		const auto raw = raws[row];
		if (raw.empty() || raw == "null") {
			validity.SetInvalid(row);
			continue;
		}
		JSONParseError error;
		if (!parser.Write(raw.data(), raw.size(), writer, error)) {
			return false;
		}
		const auto text = writer.View();
		values[row] = StringVector::AddString(target, text.data(), text.size());
	}
	return true;
}

bool JsonDirectDecoder::Value(JsonDirectKind kind, data_ptr_t data, ValidityMask &validity, Vector &string_vector,
                              Vector &vector, simdjson::ondemand::value &value, idx_t row) {
	namespace ondemand = simdjson::ondemand;
	ondemand::json_type type;
	if (value.type().get(type) || type == ondemand::json_type::unknown) {
		return false;
	}
	if (type == ondemand::json_type::null) {
		bool is_null = false;
		if (value.is_null().get(is_null) || !is_null) {
			return false;
		}
		validity.SetInvalid(row);
		return true;
	}
	const bool strict = options.strict_cast;
	if (kind == JsonDirectKind::STRING || vector.GetType().id() == LogicalTypeId::VARCHAR) {
		auto &target = kind == JsonDirectKind::STRING ? string_vector : vector;
		auto strings = reinterpret_cast<string_t *>(data);
		if (type == ondemand::json_type::string) {
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
			validity.SetInvalid(row);
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
		auto &parameters = options.parameters;
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
		validity.SetInvalid(row);
	}
	return true;
}

} // namespace duckdb
