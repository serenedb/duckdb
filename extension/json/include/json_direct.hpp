//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_direct.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <absl/base/optimization.h>

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/json_document.hpp"
#include "json_transform.hpp"
#include "simdjson.h"

namespace duckdb {

enum class JsonDirectKind : uint8_t { VALUE, STRING, TREE, STRUCT, LIST, MAP };

enum class JsonDirectFast : uint8_t { NONE, BIGINT, DOUBLE, STRING, INTEGER };

struct JsonKeyHint {
	string key;
	uint64_t words[2] = {};
	idx_t column = DConstants::INVALID_INDEX;
	uint8_t strength = 0;
};

struct JsonDirectNode {
	JsonDirectKind kind = JsonDirectKind::TREE;
	JsonDirectFast fast = JsonDirectFast::NONE;
	optional_ptr<const ColumnIndex> column_index;
	vector<idx_t> children;
	vector<idx_t> slots;
	vector<idx_t> unprojected;
	unordered_map<string, idx_t> keys;
	case_insensitive_map_t<idx_t> folded_keys;
	bool unknown_key_error = false;
	idx_t child = DConstants::INVALID_INDEX;
};

class JsonFlags {
public:
	void Resize(idx_t count) {
		storage.assign(count + 2 * ABSL_CACHELINE_SIZE, 0);
		size = count;
	}
	uint8_t *Data() {
		return storage.data() + ABSL_CACHELINE_SIZE;
	}
	idx_t Size() const {
		return size;
	}

private:
	vector<uint8_t> storage;
	idx_t size = 0;
};

struct JsonDirectNodeState {
	Vector *target = nullptr;
	data_ptr_t data = nullptr;
	ValidityMask *validity = nullptr;
	string_t *keys = nullptr;
	unique_ptr<Vector> strings;
	vector<std::string_view> raw;
	idx_t list_size = 0;
	idx_t list_capacity = 0;
	JsonFlags found;
	vector<JsonKeyHint> hints;
};

struct JsonParserMemory {
	explicit JsonParserMemory(Allocator &allocator_p) : allocator(allocator_p), callbacks {Allocate, Deallocate, this} {
	}

	static void *Allocate(void *context, size_t size) noexcept;
	static void Deallocate(void *context, void *pointer, size_t size) noexcept;

	Allocator &allocator;
	simdjson::buffer_allocator callbacks;
	ErrorData error;
};

JsonDirectKind DirectKind(const LogicalType &type);
JsonDirectKind DirectKind(const LogicalType &type, const ColumnIndex &index);
JsonDirectFast DirectFast(JsonDirectKind kind, LogicalTypeId id);
std::string_view TrimToken(std::string_view token);
bool PlainString(std::string_view content);
void KeyWords(std::string_view key, uint64_t words[2]);
bool SameKey(const JsonKeyHint &hint, std::string_view key);
idx_t PlanJsonNode(vector<JsonDirectNode> &nodes, const LogicalType &type, optional_ptr<const ColumnIndex> index);

class JsonDirectDecoder {
public:
	JsonDirectDecoder(const vector<JsonDirectNode> &nodes, JSONTransformOptions &options, JSONTreeParser &parser,
	                  JSONStringWriter &writer);

	void Bind(idx_t id, Vector &vector, idx_t capacity);
	bool Decode(idx_t id, simdjson::ondemand::value &value, idx_t row);
	void Null(idx_t id, idx_t row);
	bool Finish(idx_t id, idx_t count, JSONTransformScratch &scratch, const char *readable_end);
	bool Value(JsonDirectKind kind, data_ptr_t data, ValidityMask &validity, Vector &strings, Vector &vector,
	           simdjson::ondemand::value &value, idx_t row);
	bool WriteJsonValues(const std::string_view *raws, idx_t count, Vector &target);

public:
	bool skipped = false;

private:
	void Refresh(idx_t id, idx_t from, idx_t capacity);
	bool Struct(idx_t id, simdjson::ondemand::value &value, idx_t row);
	bool List(idx_t id, simdjson::ondemand::value &value, idx_t row);
	bool Map(idx_t id, simdjson::ondemand::value &value, idx_t row);
	bool Leaf(idx_t id, simdjson::ondemand::value &value, idx_t row);
	void Grow(idx_t id);

private:
	const vector<JsonDirectNode> &nodes;
	JSONTransformOptions &options;
	JSONTreeParser &parser;
	JSONStringWriter &writer;
	vector<JsonDirectNodeState> states;
	vector<idx_t> roots;
	vector<const JSONNode *> values;
};

} // namespace duckdb
