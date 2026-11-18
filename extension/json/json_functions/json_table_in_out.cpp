#include "json_common.hpp"
#include "json_functions.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

enum class JSONTableInOutType { EACH, TREE };

static unique_ptr<FunctionData> JSONTableInOutBind(ClientContext &, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	const child_list_t<LogicalType> schema {
	    {"key", LogicalType::VARCHAR},     {"value", LogicalType::JSON()}, {"type", LogicalType::VARCHAR},
	    {"atom", LogicalType::JSON()},     {"id", LogicalType::UBIGINT},   {"parent", LogicalType::UBIGINT},
	    {"fullkey", LogicalType::VARCHAR}, {"path", LogicalType::VARCHAR},
	};

	// Add all default columns
	names.reserve(schema.size());
	return_types.reserve(schema.size());
	for (const auto &col : schema) {
		names.emplace_back(col.first);
		return_types.emplace_back(col.second);
	}

	return nullptr;
}

struct JSONTableInOutGlobalState : GlobalTableFunctionState {
	JSONTableInOutGlobalState() {
	}

	//! Regular columns
	optional_idx key_column_index;
	optional_idx value_column_index;
	optional_idx type_column_index;
	optional_idx atom_column_index;
	optional_idx id_column_index;
	optional_idx parent_column_index;
	optional_idx fullkey_column_index;
	optional_idx path_column_index;

	//! Virtual columns
	optional_idx json_column_index;
	optional_idx root_column_index;
	optional_idx empty_column_idex;
	optional_idx rowid_column_index;

	static constexpr idx_t JSON_COLUMN_OFFSET = 0;
	static constexpr idx_t ROOT_COLUMN_OFFSET = 1;

	//! Progress when used as a source (i.e. with a single input row) - the rows that will be produced, and the rows
	//! that were produced
	atomic<idx_t> total_rows {0};
	atomic<idx_t> produced_rows {0};
};

static unique_ptr<GlobalTableFunctionState> JSONTableInOutInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto result = make_uniq<JSONTableInOutGlobalState>();
	for (idx_t i = 0; i < input.column_indexes.size(); i++) {
		const auto &col_idx = input.column_indexes[i];
		if (!col_idx.IsVirtualColumn()) {
			switch (col_idx.GetPrimaryIndex()) {
			case 0:
				result->key_column_index = i;
				break;
			case 1:
				result->value_column_index = i;
				break;
			case 2:
				result->type_column_index = i;
				break;
			case 3:
				result->atom_column_index = i;
				break;
			case 4:
				result->id_column_index = i;
				break;
			case 5:
				result->parent_column_index = i;
				break;
			case 6:
				result->fullkey_column_index = i;
				break;
			case 7:
				result->path_column_index = i;
				break;
			default:
				throw NotImplementedException("Column %llu for json_each/json_tree", col_idx.GetPrimaryIndex());
			}
		} else {
			if (col_idx.GetPrimaryIndex() == VIRTUAL_COLUMN_START + JSONTableInOutGlobalState::JSON_COLUMN_OFFSET) {
				result->json_column_index = i;
			} else if (col_idx.GetPrimaryIndex() ==
			           VIRTUAL_COLUMN_START + JSONTableInOutGlobalState::ROOT_COLUMN_OFFSET) {
				result->root_column_index = i;
			} else if (col_idx.IsEmptyColumn()) {
				result->empty_column_idex = i;
			} else if (col_idx.IsRowIdColumn()) {
				result->rowid_column_index = i;
			} else {
				throw NotImplementedException("Virtual column %llu for json_each/json_tree", col_idx.GetPrimaryIndex());
			}
		}
	}
	return std::move(result);
}

//! Whether an object key can appear unquoted in a JSON path (as in "$.key"). Mirrors SQLite's json_each/json_tree:
//! only keys consisting of an ASCII letter followed by ASCII alphanumerics/underscores go unquoted
static bool JSONPathKeyNeedsQuoting(const char *data, const idx_t len) {
	if (len == 0 || !StringUtil::CharacterIsAlpha(data[0])) {
		return true;
	}
	for (idx_t i = 1; i < len; i++) {
		if (!StringUtil::CharacterIsAlphaNumeric(data[i]) && data[i] != '_') {
			return true;
		}
	}
	return false;
}

//! Appends ".key" to the path, quoting the key (as in "$.\"a.b\"") when needed so that the resulting path
//! round-trips through JSON path extraction (issue #23148)
static void AppendObjectPathElement(const JSONNode &vkey, string &path) {
	const auto data = vkey.str;
	const idx_t len = vkey.size;
	path += '.';
	if (!JSONPathKeyNeedsQuoting(data, len)) {
		path.append(data, len);
		return;
	}
	path += '"';
	for (idx_t i = 0; i < len; i++) {
		if (data[i] == '"' || data[i] == '\\') {
			path += '\\';
		}
		path += data[i];
	}
	path += '"';
}

struct JSONTableInOutRecursionNode {
	JSONTableInOutRecursionNode(string path_p, const JSONNode *parent_val_p)
	    : path(std::move(path_p)), parent_val(parent_val_p), child(parent_val_p->First()), child_index(0) {
	}

	string path;
	const JSONNode *parent_val;
	const JSONNode *child;
	idx_t child_index;
};

struct JSONTableInOutLocalState : LocalTableFunctionState {
	explicit JSONTableInOutLocalState(JSONTableInOutGlobalState &gstate)
	    : gstate(gstate), parser(JSONCommon::READ_FLAG), writer(JSONCommon::WRITE_FLAG), len(DConstants::INVALID_INDEX),
	      doc_root(nullptr), initialized(false), total_count(0) {
	}

	string GetPath() const {
		auto result = path;
		for (const auto &ri : recursion_nodes) {
			result += ri.path;
		}
		return result;
	}

	void AddRecursionNode(const JSONNode *val, optional_ptr<const JSONNode> vkey, const optional_idx arr_index) {
		string str;
		if (vkey) {
			AppendObjectPathElement(*vkey, str);
		} else if (arr_index.IsValid()) {
			str = "[" + to_string(arr_index.GetIndex()) + "]";
		}
		recursion_nodes.emplace_back(str, val);
	}

	JSONTableInOutGlobalState &gstate;
	JSONTreeParser parser;
	JSONStringWriter writer;
	//! The rows the current input row is estimated to produce, and the rows it produced so far
	idx_t estimated_rows = 0;
	idx_t produced_rows = 0;

	string path;
	idx_t len;
	const JSONNode *doc_root;
	bool initialized;

	idx_t total_count;
	vector<JSONTableInOutRecursionNode> recursion_nodes;
};

static unique_ptr<LocalTableFunctionState> JSONTableInOutInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                                   GlobalTableFunctionState *global_state) {
	return make_uniq<JSONTableInOutLocalState>(global_state->Cast<JSONTableInOutGlobalState>());
}

static double JSONTableInOutProgress(ClientContext &, const FunctionData *,
                                     const GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<JSONTableInOutGlobalState>();
	auto total_rows = gstate.total_rows.load(std::memory_order_relaxed);
	if (total_rows == 0) {
		return 0;
	}
	auto produced_rows = MinValue<idx_t>(gstate.produced_rows.load(std::memory_order_relaxed), total_rows);
	return 100.0 * static_cast<double>(produced_rows) / static_cast<double>(total_rows);
}

template <class T>
struct JSONTableInOutResultVector {
	explicit JSONTableInOutResultVector(DataChunk &output, const optional_idx &output_column_index)
	    : enabled(output_column_index.IsValid()), vector(output.data[enabled ? output_column_index.GetIndex() : 0]),
	      data(enabled ? FlatVector::GetDataMutable<T>(vector) : nullptr),
	      validity(FlatVector::ValidityMutable(vector)) {
	}
	const bool enabled;
	Vector &vector;
	T *data;
	ValidityMask &validity;
};

struct JSONTableInOutResult {
	explicit JSONTableInOutResult(const JSONTableInOutGlobalState &gstate, DataChunk &output)
	    : count(0), key(output, gstate.key_column_index), value(output, gstate.value_column_index),
	      type(output, gstate.type_column_index), atom(output, gstate.atom_column_index),
	      id(output, gstate.id_column_index), parent(output, gstate.parent_column_index),
	      fullkey(output, gstate.fullkey_column_index), path(output, gstate.path_column_index),
	      rowid(output, gstate.rowid_column_index) {
	}

	template <JSONTableInOutType TYPE>
	void AddRow(JSONTableInOutLocalState &lstate, optional_ptr<const JSONNode> vkey, const JSONNode *val) {
		const auto &recursion_nodes = lstate.recursion_nodes;
		const auto arr_el = !recursion_nodes.empty() && recursion_nodes.back().parent_val->type == JSONValueType::ARRAY;
		if (key.enabled) {
			if (vkey) { // Object field
				key.data[count] = string_t(vkey->str, vkey->size);
			} else if (arr_el) { // Array element
				key.data[count] = StringVector::AddString(key.vector, to_string(recursion_nodes.back().child_index));
			} else { // Other
				key.validity.SetInvalid(count);
			}
		}
		if (value.enabled) {
			value.data[count] = JSONCommon::WriteVal(*val, lstate.writer, value.vector);
		}
		if (type.enabled) {
			type.data[count] = JSONCommon::ValTypeToStringT(*val);
		}
		if (atom.enabled) {
			if (val->type == JSONValueType::JSON_NULL || val->IsContainer()) {
				atom.validity.SetInvalid(count);
			} else {
				atom.data[count] = JSONCommon::WriteVal(*val, lstate.writer, atom.vector);
			}
		}
		if (id.enabled) {
			id.data[count] = NumericCast<idx_t>(val - lstate.doc_root);
		}
		if (parent.enabled) {
			if (TYPE == JSONTableInOutType::EACH || recursion_nodes.empty()) {
				parent.validity.SetInvalid(count);
			} else {
				parent.data[count] = NumericCast<uint64_t>(recursion_nodes.back().parent_val - lstate.doc_root);
			}
		}
		const auto path_str = lstate.GetPath();
		if (fullkey.enabled) {
			if (vkey) { // Object field
				auto fullkey_str = path_str;
				AppendObjectPathElement(*vkey, fullkey_str);
				fullkey.data[count] = StringVector::AddString(fullkey.vector, fullkey_str);
			} else if (arr_el) { // Array element
				const auto arr_path = "[" + to_string(recursion_nodes.back().child_index) + "]";
				fullkey.data[count] = StringVector::AddString(fullkey.vector, path_str + arr_path);
			} else { // Other
				fullkey.data[count] = StringVector::AddString(fullkey.vector, path_str);
			}
		}
		if (path.enabled) {
			path.data[count] = StringVector::AddString(path.vector, path_str);
		}
		if (rowid.enabled) {
			rowid.data[count] = NumericCast<int64_t>(lstate.total_count++);
		}
		count++;
	}

	idx_t count;
	JSONTableInOutResultVector<string_t> key;
	JSONTableInOutResultVector<string_t> value;
	JSONTableInOutResultVector<string_t> type;
	JSONTableInOutResultVector<string_t> atom;
	JSONTableInOutResultVector<uint64_t> id;
	JSONTableInOutResultVector<uint64_t> parent;
	JSONTableInOutResultVector<string_t> fullkey;
	JSONTableInOutResultVector<string_t> path;
	JSONTableInOutResultVector<int64_t> rowid;
};

template <JSONTableInOutType TYPE>
static void InitializeLocalState(JSONTableInOutLocalState &lstate, DataChunk &input, JSONTableInOutResult &result) {
	lstate.total_count = 0;

	// Parse path, default to root if not given
	Value path_value("$");
	if (input.data.size() > 1) {
		const auto &path_vector = input.data[1];
		if (ConstantVector::IsNull(path_vector)) {
			return;
		}
		path_value = ConstantVector::GetData<string_t>(path_vector)[0];
	}

	if (JSONReadFunctionData::CheckPath(path_value, lstate.path, lstate.len) == JSONCommon::JSONPathType::WILDCARD) {
		throw BinderException("Wildcard JSON path not supported in json_each/json_tree");
	}

	if (lstate.path.c_str()[0] != '$') {
		throw BinderException("JSON path must start with '$' for json_each/json_tree");
	}

	// Parse document and get the value at the supplied path
	const auto &input_vector = input.data[0];
	if (ConstantVector::IsNull(input_vector)) {
		return;
	}
	const auto &input_data = ConstantVector::GetData<string_t>(input_vector)[0];
	lstate.doc_root = &JSONCommon::ReadDocument(lstate.parser, input_data);
	const auto root = JSONCommon::GetUnsafe(lstate.doc_root, lstate.path.c_str(), lstate.len);

	if (!root) {
		return;
	}

	const auto is_container = root->IsContainer();
	// json_each produces a row per element of the root, json_tree a row per value (the value count is an upper bound)
	if (TYPE == JSONTableInOutType::TREE) {
		lstate.estimated_rows = lstate.parser.NodeCount();
	} else {
		lstate.estimated_rows = is_container ? root->size : 1;
	}
	lstate.produced_rows = 0;
	lstate.gstate.total_rows.fetch_add(lstate.estimated_rows, std::memory_order_relaxed);
	if (!is_container || TYPE == JSONTableInOutType::TREE) {
		result.AddRow<TYPE>(lstate, nullptr, root);
	}
	if (is_container) {
		lstate.AddRecursionNode(root, nullptr, optional_idx());
	}
}

template <JSONTableInOutType TYPE>
static OperatorResultType JSONTableInOutFunction(ExecutionContext &, TableFunctionInput &data_p, DataChunk &input,
                                                 DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<JSONTableInOutGlobalState>();
	auto &lstate = data_p.local_state->Cast<JSONTableInOutLocalState>();

	JSONTableInOutResult result(gstate, output);
	if (!lstate.initialized) {
		InitializeLocalState<TYPE>(lstate, input, result);
		lstate.initialized = true;
	}

	// Traverse the JSON (keeping a stack to avoid recursion and save progress across calls)
	auto &recursion_nodes = lstate.recursion_nodes;
	while (!recursion_nodes.empty() && result.count != STANDARD_VECTOR_SIZE) {
		auto &node = recursion_nodes.back();
		if (node.child_index == node.parent_val->size) {
			recursion_nodes.pop_back(); // Array/object is done, remove
			continue;
		}
		const auto child_key = node.parent_val->type == JSONValueType::OBJECT ? node.child : nullptr;
		const auto child_val = child_key ? child_key + 1 : node.child;
		result.AddRow<TYPE>(lstate, child_key, child_val);
		const auto child_index = node.child_index++; // We finished processing the array element
		node.child = child_val->Next();
		if (TYPE == JSONTableInOutType::TREE && child_val->IsContainer()) {
			lstate.AddRecursionNode(child_val, child_key, child_index); // Go depth-first
		}
	}
	output.SetChildCardinality(result.count);
	lstate.produced_rows += result.count;
	gstate.produced_rows.fetch_add(result.count, std::memory_order_relaxed);

	// Set constant virtual columns ("json", "root", and "empty")
	if (gstate.json_column_index.IsValid()) {
		auto &json_vector = output.data[gstate.json_column_index.GetIndex()];
		json_vector.Reference(input.data[0]);
	}
	if (gstate.root_column_index.IsValid()) {
		auto &root_vector = output.data[gstate.root_column_index.GetIndex()];
		root_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::GetData<string_t>(root_vector)[0] = string_t(lstate.path.c_str(), lstate.len);
		FlatVector::SetSize(root_vector, count_t(result.count));
	}
	if (gstate.empty_column_idex.IsValid()) {
		auto &empty_vector = output.data[gstate.empty_column_idex.GetIndex()];
		ConstantVector::SetNull(empty_vector, count_t(result.count));
	}

	if (output.size() == 0) {
		D_ASSERT(recursion_nodes.empty());
		// the input row is done - correct the estimate to the rows that were actually produced
		if (lstate.estimated_rows > lstate.produced_rows) {
			gstate.total_rows.fetch_sub(lstate.estimated_rows - lstate.produced_rows, std::memory_order_relaxed);
		} else {
			gstate.total_rows.fetch_add(lstate.produced_rows - lstate.estimated_rows, std::memory_order_relaxed);
		}
		lstate.estimated_rows = 0;
		lstate.produced_rows = 0;
		lstate.initialized = false;
		return OperatorResultType::NEED_MORE_INPUT;
	}
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

virtual_column_map_t GetJSONTableInOutVirtualColumns(ClientContext &, optional_ptr<FunctionData>) {
	virtual_column_map_t result;
	result.insert(make_pair(VIRTUAL_COLUMN_START + JSONTableInOutGlobalState::JSON_COLUMN_OFFSET,
	                        TableColumn("json", LogicalType::JSON())));
	result.insert(make_pair(VIRTUAL_COLUMN_START + JSONTableInOutGlobalState::ROOT_COLUMN_OFFSET,
	                        TableColumn("root", LogicalType::VARCHAR)));
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::BIGINT)));
	return result;
}

template <JSONTableInOutType TYPE>
TableFunction GetJSONTableInOutFunction(const LogicalType &input_type, const bool &has_path_param) {
	FunctionSignature signature;
	signature.AddPositionalOnly("json", input_type);
	if (has_path_param) {
		signature.AddPositionalOnly("path", LogicalType::VARCHAR);
	}
	TableFunction function(std::move(signature), nullptr, JSONTableInOutBind, JSONTableInOutInitGlobal,
	                       JSONTableInOutInitLocal);
	function.in_out_function = JSONTableInOutFunction<TYPE>;
	function.table_scan_progress = JSONTableInOutProgress;
	function.get_virtual_columns = GetJSONTableInOutVirtualColumns;
	function.projection_pushdown = true;
	return function;
}

TableFunctionSet JSONFunctions::GetJSONEachFunction() {
	TableFunctionSet set("json_each");
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::EACH>(LogicalType::VARCHAR, false));
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::EACH>(LogicalType::VARCHAR, true));
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::EACH>(LogicalType::JSON(), false));
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::EACH>(LogicalType::JSON(), true));
	return set;
}

TableFunctionSet JSONFunctions::GetJSONTreeFunction() {
	TableFunctionSet set("json_tree");
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::TREE>(LogicalType::VARCHAR, false));
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::TREE>(LogicalType::VARCHAR, true));
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::TREE>(LogicalType::JSON(), false));
	set.AddFunction(GetJSONTableInOutFunction<JSONTableInOutType::TREE>(LogicalType::JSON(), true));
	return set;
}

} // namespace duckdb
