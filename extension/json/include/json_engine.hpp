//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_engine.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <absl/base/optimization.h>

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/formats/json_ranges.hpp"
#include "duckdb/formats/range_lines.hpp"
#include "duckdb/formats/row_chain.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "json_scan.hpp"
#include "json_source.hpp"
#include "json_transform.hpp"
#include "simdjson.h"

namespace duckdb {

enum class JsonDirectKind : uint8_t { VALUE, STRING, TREE, STRUCT, LIST };

enum class JsonDirectPass : uint8_t { ALL, FILTERS, OTHERS };

enum class JsonDirectFast : uint8_t { NONE, BIGINT, DOUBLE, STRING };

struct JsonKeyHint {
	string key;
	uint64_t words[2] = {};
	idx_t column = DConstants::INVALID_INDEX;
	uint8_t strength = 0;
};

enum class JsonPathCheck : uint8_t { COMPARE, IN_LIST, IS_NULL, IS_NOT_NULL };

struct JsonPathPredicate {
	idx_t column;
	vector<string> path;
	bool text;
	JsonPathCheck check;
	ExpressionType compare = ExpressionType::INVALID;
	vector<Value> constants;
	LogicalTypeId leaf = LogicalTypeId::INVALID;
	int64_t lower = 0;
	int64_t upper = 0;
};

enum class JsonRecordResult : uint8_t { DECODED, REJECTED, FALLBACK };

//! A STRUCT or LIST column, or a value inside one, decoded straight into its vector
struct JsonDirectNode {
	JsonDirectKind kind = JsonDirectKind::TREE;
	JsonDirectFast fast = JsonDirectFast::NONE;
	//! TREE: the projection passed to JSONTransform
	optional_ptr<const ColumnIndex> column_index;
	//! STRUCT: the projected children, their entries in the struct vector and the entries projected out
	vector<idx_t> children;
	vector<idx_t> slots;
	vector<idx_t> unprojected;
	unordered_map<string, idx_t> keys;
	case_insensitive_map_t<idx_t> folded_keys;
	bool unknown_key_error = false;
	//! LIST: the element
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

struct JsonScanFilter {
	idx_t output_column;
	optional_idx name;
	reference<const TableFilter> filter;
};

struct JsonScanColumns {
	vector<string> names;
	vector<LogicalType> types;
	vector<column_t> column_ids;
	vector<ColumnIndex> column_indices;
	vector<JSONFeatureColumn> feature_columns;
	idx_t file_row_number = DConstants::INVALID_INDEX;
	vector<JsonScanFilter> filters;
	JSONTransformOptions transform_options;
};

class JsonScanGlobal {
public:
	JsonScanGlobal(ClientContext &context, const JSONScanData &bind, shared_ptr<JsonSource> source,
	               JsonScanColumns columns, bool parallel);

	idx_t MaxThreads() const;
	double Progress() const {
		return source->Progress();
	}
	bool Claim(idx_t &unit, idx_t &begin);
	void Finish(idx_t unit, idx_t records);
	void AddError(idx_t unit, idx_t record, string message);
	[[noreturn]] void ThrowObjectSizeError(idx_t object_size) const;
	idx_t BufferCapacity() const;
	void Computing(idx_t range, bool value);
	void Publish(idx_t range, JsonRangeOrigin origin, const JsonRangeEntry &entry);
	JsonRangeStart StartState(idx_t range);

public:
	ClientContext &context;
	const JSONScanData &bind;
	const JSONReaderOptions &options;
	shared_ptr<JsonSource> source;
	JsonScanColumns columns;
	JSONFormat format = JSONFormat::AUTO_DETECT;
	bool features_prefix = false;
	bool parallel_values = false;
	idx_t stream_bytes = 0;
	unique_ptr<RowRanges> ranges;
	JSONReadFlags read_flags;
	bool validate_only = false;
	bool direct = false;
	vector<JsonDirectKind> kinds;
	vector<JsonDirectFast> fast;
	vector<bool> filtered;
	vector<uint8_t> pass_skips;
	vector<JsonPathPredicate> walk;
	vector<bool> walked;
	unordered_map<string, idx_t> keys;
	case_insensitive_map_t<idx_t> folded_keys;
	vector<JsonDirectNode> nodes;
	vector<idx_t> column_nodes;

private:
	void Detect();
	void PlanDirect();
	idx_t PlanNode(const LogicalType &type, optional_ptr<const ColumnIndex> index);
	void ThrowIfPossible();

private:
	mutable mutex lock;
	RowChain<JsonRangeStart, JsonRangeEntry, JSON_RANGE_ORIGINS> chain;
	RangeLines lines;
	optional_idx error_unit;
	idx_t error_record = 0;
	string error_message;
};

class JsonScan {
public:
	explicit JsonScan(JsonScanGlobal &global);
	~JsonScan();

	bool Claim();
	bool Claimed() const {
		return claimed;
	}
	void Release();
	idx_t Scan(DataChunk &output);
	idx_t Sample(const JSONNode **&values, idx_t &bytes);
	idx_t Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output);

private:
	idx_t ScanUnit(DataChunk &output);
	void Load();
	void LoadValueRange();
	idx_t CutRecords(idx_t &searched_to);
	void Begin();
	bool Last() const {
		return window.read_to >= global.ranges->Size();
	}
	bool ReadMore(bool may_idle = false);
	bool ReadWindow(idx_t keep, idx_t piece);
	void Fill(idx_t bytes);
	void SkipBlank();
	void HoldWindow(Vector &vector);
	bool NextRecords();
	void SkipSpace(bool may_idle = false);
	bool FrameRecord(idx_t &record_end, bool may_idle);
	bool AfterArrayRecord(idx_t record_size);
	void SkipArrayStart();
	bool Parse(idx_t index, idx_t record, const char *data, idx_t size, idx_t readable);
	bool ParseBatch();
	bool DecodeDirect(DataChunk &output, idx_t &rows);
	JsonRecordResult DirectRecord(idx_t record, idx_t row, JsonDirectPass pass, const vector<Vector *> &vectors);
	bool WalkRejects(idx_t column, std::string_view raw);
	void NullRow(idx_t row, JsonDirectPass pass, const vector<Vector *> &vectors);
	bool DirectValue(JsonDirectKind kind, data_ptr_t data, ValidityMask &validity, Vector &strings, Vector &vector,
	                 simdjson::ondemand::value &value, idx_t row);
	bool FinishDirect(idx_t rows, JsonDirectPass pass, const vector<Vector *> &vectors);
	void BindNode(idx_t id, Vector &vector, idx_t capacity);
	void RefreshNode(idx_t id, idx_t from, idx_t capacity);
	bool DirectNode(idx_t id, simdjson::ondemand::value &value, idx_t row);
	bool DirectStruct(idx_t id, simdjson::ondemand::value &value, idx_t row);
	bool DirectList(idx_t id, simdjson::ondemand::value &value, idx_t row);
	bool DirectLeaf(idx_t id, simdjson::ondemand::value &value, idx_t row);
	void NullNode(idx_t id, idx_t row);
	bool FinishNode(idx_t id, idx_t count, JSONTransformScratch &scratch);
	idx_t Decode(DataChunk &output, idx_t count);
	idx_t DecodeObjects(DataChunk &output, idx_t count);
	idx_t ApplyFilters(DataChunk &output, idx_t count, bool filtered_columns_only,
	                   optional_idx candidate_count = optional_idx());
	void FillRowNumbers(DataChunk &output, idx_t count);
	void ParseError(idx_t record, const JSONParseError &error, const string &extra);
	void TransformError(idx_t record, const string &message);
	void ResetBatch();

private:
	JsonScanGlobal &global;
	const JSONReaderOptions &options;
	RowWindow window;
	const char *data = nullptr;
	idx_t size = 0;
	idx_t cursor = 0;
	idx_t line_start = 0;
	idx_t limit = 0;
	idx_t file_offset = 0;
	idx_t unit = 0;
	idx_t begin = 0;
	idx_t unit_records = 0;
	bool claimed = false;
	bool loaded = false;
	bool sampling = false;
	bool first = false;
	bool done = false;
	bool stopped = false;
	bool idle = false;
	ArenaAllocator arena;
	JSONTreeParser parser;
	JSONStringWriter writer;
	JSONTransformOptions transform_options;
	vector<unique_ptr<TableFilterState>> filter_states;
	SelectionVector filter_selection;
	idx_t count = 0;
	idx_t batch_bytes = 0;
	idx_t starts[STANDARD_VECTOR_SIZE];
	idx_t sizes[STANDARD_VECTOR_SIZE];
	idx_t lengths[STANDARD_VECTOR_SIZE];
	idx_t roots[STANDARD_VECTOR_SIZE];
	const JSONNode *values[STANDARD_VECTOR_SIZE];
	JsonParserMemory direct_memory;
	simdjson::ondemand::parser direct_parser;
	simdjson::ondemand::parser walk_parser;
	SelectionVector candidates;
	vector<Vector> direct_strings;
	vector<data_ptr_t> direct_data;
	vector<ValidityMask *> direct_validity;
	vector<vector<std::string_view>> direct_raw;
	JsonFlags direct_found;
	vector<JsonKeyHint> key_hints;
	vector<JsonDirectNodeState> node_states;
	vector<idx_t> node_roots;
	vector<const JSONNode *> node_values;
	bool direct_unknown_error = false;
	bool direct_duplicate_error = false;
};

bool JsonDetectFormat(const char *data, idx_t size, bool complete, JSONTreeParser &parser, JSONFormat &format,
                      JSONRecordType &record_type);
bool JsonWalkPushdown(const Expression &expr);

} // namespace duckdb
