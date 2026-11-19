//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_engine.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/json_document.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "json_scan.hpp"
#include "json_source.hpp"
#include "json_transform.hpp"
#include "simdjson.h"

namespace duckdb {

enum class JsonUnitMode : uint8_t { RANGES, STREAM, SEQUENTIAL };

enum class JsonDirectKind : uint8_t { VALUE, STRING, TREE };

enum class JsonDirectPass : uint8_t { ALL, FILTERS, OTHERS };

struct JsonKeyHint {
	string key;
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
	bool Claim(idx_t &unit);
	bool ClaimChunk(idx_t &unit, AllocatedData &buffer, idx_t &size, idx_t &file_offset, bool &last);
	void Finish(idx_t unit, idx_t records);
	void AddError(idx_t unit, idx_t record, string message);
	bool Failed() const;
	[[noreturn]] void ThrowObjectSizeError(idx_t object_size) const;
	idx_t RangeBegin(idx_t unit) const {
		return MinValue(unit * range_bytes, source->Size());
	}
	idx_t RangeEnd(idx_t unit) const {
		return MinValue((unit + 1) * range_bytes, source->Size());
	}
	idx_t BufferCapacity() const;

public:
	ClientContext &context;
	const JSONScanData &bind;
	const JSONReaderOptions &options;
	shared_ptr<JsonSource> source;
	JsonScanColumns columns;
	JSONFormat format = JSONFormat::AUTO_DETECT;
	JSONRecordType detected_record_type = JSONRecordType::AUTO_DETECT;
	bool features_prefix = false;
	JsonUnitMode mode = JsonUnitMode::SEQUENTIAL;
	idx_t range_bytes = 0;
	idx_t range_count = 0;
	JSONReadFlags read_flags;
	bool validate_only = false;
	bool direct = false;
	vector<JsonDirectKind> kinds;
	vector<bool> filtered;
	vector<JsonPathPredicate> walk;
	vector<bool> walked;
	unordered_map<string, idx_t> keys;
	case_insensitive_map_t<idx_t> folded_keys;

private:
	void Detect();
	void PlanDirect();
	void ThrowIfPossible();

private:
	mutable mutex lock;
	idx_t next_unit = 0;
	bool stream_done = false;
	AllocatedData carry;
	idx_t carry_size = 0;
	idx_t stream_offset = 0;
	vector<int64_t> unit_records;
	optional_idx error_unit;
	idx_t error_record = 0;
	string error_message;
};

class JsonScan {
public:
	explicit JsonScan(JsonScanGlobal &global);

	bool Claim();
	bool Claimed() const {
		return claimed;
	}
	void Release();
	idx_t Scan(DataChunk &output);
	idx_t Sample(const JSONNode **&values, idx_t &bytes);
	idx_t Lookup(std::span<const int64_t> offsets, std::span<idx_t> survivors, DataChunk &output);

private:
	void Load();
	void Begin();
	bool ReadMore();
	bool NextRecords();
	void SkipSpace();
	bool FrameRecord(idx_t &record_end);
	bool AfterArrayRecord(idx_t record_size);
	void SkipArrayStart();
	bool Parse(idx_t index, idx_t record, const char *data, idx_t size, idx_t readable);
	bool ParseBatch();
	bool DecodeDirect(DataChunk &output, idx_t &rows);
	JsonRecordResult DirectRecord(idx_t record, idx_t row, JsonDirectPass pass, const vector<Vector *> &vectors);
	bool WalkRejects(idx_t column, std::string_view raw);
	void NullRow(idx_t row, JsonDirectPass pass, const vector<Vector *> &vectors);
	bool DirectValue(idx_t column, simdjson::ondemand::value &value, Vector &vector, idx_t row);
	bool FinishDirect(idx_t rows, JsonDirectPass pass, const vector<Vector *> &vectors);
	idx_t Decode(DataChunk &output, idx_t count);
	idx_t DecodeObjects(DataChunk &output, idx_t count);
	idx_t ApplyFilters(DataChunk &output, idx_t count, bool filtered_columns_only,
	                   optional_idx candidate_count = optional_idx());
	void FillRowNumbers(DataChunk &output, idx_t count);
	void ParseError(idx_t record, const JSONParseError &error, const string &extra);
	void TransformError(idx_t record, const string &message);
	void ResetBatch();
	void EnsureCapacity(idx_t capacity);

private:
	JsonScanGlobal &global;
	const JSONReaderOptions &options;
	AllocatedData buffer;
	char *data = nullptr;
	idx_t capacity = 0;
	idx_t size = 0;
	idx_t cursor = 0;
	idx_t line_start = 0;
	idx_t limit = 0;
	idx_t file_offset = 0;
	idx_t unit = 0;
	idx_t unit_records = 0;
	bool claimed = false;
	bool loaded = false;
	bool sampling = false;
	bool first = false;
	bool last = false;
	bool done = false;
	bool stopped = false;
	unique_ptr<FileHandle> thread_handle;
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
	const char *found_records[STANDARD_VECTOR_SIZE];
	JsonParserMemory direct_memory;
	simdjson::ondemand::parser direct_parser;
	simdjson::ondemand::parser walk_parser;
	SelectionVector candidates;
	vector<Vector> direct_strings;
	vector<vector<std::string_view>> direct_raw;
	vector<uint8_t> direct_found;
	vector<JsonKeyHint> key_hints;
	bool direct_unknown_error = false;
	bool direct_duplicate_error = false;
	bool direct_missing_error = false;
};

JSONFormat JsonDetectFormat(const char *data, idx_t size, JSONTreeParser &parser, JSONRecordType &record_type);
bool JsonWalkPushdown(const Expression &expr);

} // namespace duckdb
