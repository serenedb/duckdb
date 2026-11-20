#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "json_common.hpp"
#include "json_functions.hpp"

namespace duckdb {

enum class JSONModifyType : uint8_t { SET, INSERT, REPLACE, REMOVE };

//! Parse a JSONPath or bare key into path elements (throws on malformed paths and wildcards)
static vector<JSONPathElement> ParseModifyPath(const char *ptr, idx_t len, bool binder) {
	vector<JSONPathElement> elements;
	if (len == 0) {
		return elements; // The empty path addresses the whole document
	}
	if (*ptr == '$') {
		elements = JSONCommon::ParsePathElements(ptr, len, binder);
		for (const auto &element : elements) {
			if (element.type == JSONPathElementType::WILDCARD ||
			    element.type == JSONPathElementType::RECURSIVE_WILDCARD) {
				throw InvalidInputException("JSON path wildcards are not supported in JSON modification functions");
			}
		}
	} else {
		// Bare key name
		JSONPathElement element;
		element.type = JSONPathElementType::KEY;
		element.key = string(ptr, len);
		elements.push_back(std::move(element));
	}
	return elements;
}

//! Bind data for the JSON modification functions, holds the path elements of a constant path
struct JSONModifyFunctionData : public FunctionData {
public:
	JSONModifyFunctionData(bool constant_p, string path_p) : constant(constant_p), path(std::move(path_p)) {
		if (constant && (path.empty() || path[0] != '/')) {
			// JSONPath, bare key, or empty path: parse it once so execution can skip tokenizing it per row
			try {
				elements = ParseModifyPath(path.c_str(), path.size(), true);
				use_elements = true;
			} catch (const std::exception &) {
				// Invalid path: parse per row instead, so the error surfaces only if a row is actually
				// evaluated, keeping it catchable by TRY and silent in unreachable branches
			}
		}
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JSONModifyFunctionData>(constant, path);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JSONModifyFunctionData>();
		return constant == other.constant && path == other.path;
	}
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input) {
		bool constant = false;
		string path;
		auto path_val = input.TryGetConstant(1);
		if (path_val && !path_val->IsNull()) {
			constant = true;
			path = path_val->DefaultCastAs(LogicalType::VARCHAR).GetValue<string>();
		}
		return make_uniq<JSONModifyFunctionData>(constant, std::move(path));
	}

public:
	const bool constant;
	const string path;
	vector<JSONPathElement> elements;
	bool use_elements = false;
};

//! Resolve an array path element to a position, returns false if it is out of range
static bool ResolveArrayPosition(idx_t arr_len, const JSONPathElement &element, idx_t &pos) {
	switch (element.type) {
	case JSONPathElementType::INDEX:
		pos = element.index;
		break;
	case JSONPathElementType::REVERSE_INDEX:
		if (element.index > arr_len) {
			return false;
		}
		pos = arr_len - element.index;
		break;
	case JSONPathElementType::APPEND:
		pos = arr_len;
		break;
	default:
		return false;
	}
	return pos <= arr_len;
}

static bool ParsePointerIndex(const string &token, idx_t &index) {
	if (token.empty() || token.size() > 19) {
		return false;
	}
	if (token == "-") {
		index = NumericLimits<idx_t>::Maximum();
		return true;
	}
	if (token[0] == '0' && token.size() > 1) {
		return false;
	}
	index = 0;
	for (auto c : token) {
		if (!StringUtil::CharacterIsDigit(c)) {
			return false;
		}
		index = index * 10 + static_cast<idx_t>(c - '0');
	}
	return true;
}

struct JSONModifyContainer {
	bool object;
	std::string_view key;
};

class JSONModifier {
public:
	JSONModifier(JSONModifyType type_p, const JSONModifyFunctionData &info_p) : type(type_p), info(info_p) {
	}

	//! Apply a modification at a JSONPath or JSON Pointer, using the bind data's path elements when available
	bool Write(JSONStringWriter &writer, const JSONNode &root, const string_t &path_str, const JSONNode *new_val) {
		if (info.use_elements) {
			return WriteAtElements(writer, root, info.elements, new_val);
		}
		if (info.constant && !info.path.empty() && info.path[0] == '/') {
			WriteAtPointer(writer, root, info.path.c_str(), info.path.size(), new_val);
			return true;
		}
		// Non constant path, or a constant path that did not parse at bind time (the error surfaces here)
		auto ptr = path_str.GetData();
		auto len = path_str.GetSize();
		if (len != 0 && *ptr == '/') {
			WriteAtPointer(writer, root, ptr, len, new_val);
			return true;
		}
		return WriteAtElements(writer, root, ParseModifyPath(ptr, len, false), new_val);
	}

private:
	bool WriteAtElements(JSONStringWriter &writer, const JSONNode &root, const vector<JSONPathElement> &elements,
	                     const JSONNode *new_val) {
		if (elements.empty()) {
			// Modify at the document root
			if (type == JSONModifyType::REMOVE) {
				return false;
			}
			// The root always exists, never overwritten by an insert
			writer.Value(type == JSONModifyType::INSERT ? root : *new_val);
			return true;
		}
		Reset(root);
		if (ResolveElements(elements)) {
			WriteModified(writer, 0, new_val);
		} else {
			writer.Value(root);
		}
		return true;
	}

	void WriteAtPointer(JSONStringWriter &writer, const JSONNode &root, const char *ptr, idx_t len,
	                    const JSONNode *new_val) {
		Reset(root);
		if (ParsePointer(ptr, len) && ResolvePointer()) {
			WriteModified(writer, 0, new_val);
		} else {
			writer.Value(root);
		}
	}

	void Reset(const JSONNode &root) {
		path.clear();
		path.push_back(&root);
		created.clear();
		remove = false;
	}

	//! Resolve the parent of the final path element. When creating, missing containers are created like SQLite does
	bool ResolveElements(const vector<JSONPathElement> &elements) {
		const auto create = type == JSONModifyType::SET || type == JSONModifyType::INSERT;
		for (idx_t i = 0; i + 1 < elements.size(); i++) {
			auto &val = *path.back();
			auto &element = elements[i];
			const JSONNode *child;
			if (element.type == JSONPathElementType::KEY) {
				if (val.type != JSONValueType::OBJECT) {
					return false;
				}
				key = element.key;
				child = val.Get(key);
			} else {
				if (val.type != JSONValueType::ARRAY || !ResolveArrayPosition(val.size, element, position)) {
					return false;
				}
				child = val.Get(position);
			}
			if (child) {
				path.push_back(child);
				continue;
			}
			if (!create) {
				return false;
			}
			// Create the missing containers based on the next path elements
			for (idx_t j = i + 1; j < elements.size(); j++) {
				auto &next = elements[j];
				idx_t next_position;
				if (next.type == JSONPathElementType::KEY) {
					created.push_back({true, next.key});
				} else if (ResolveArrayPosition(0, next, next_position)) {
					created.push_back({false, std::string_view()});
				} else {
					return false;
				}
			}
			return true;
		}
		auto &val = *path.back();
		auto &element = elements.back();
		bool exists;
		if (element.type == JSONPathElementType::KEY) {
			if (val.type != JSONValueType::OBJECT) {
				return false;
			}
			key = element.key;
			exists = val.Get(key) != nullptr;
		} else {
			if (val.type != JSONValueType::ARRAY || !ResolveArrayPosition(val.size, element, position)) {
				return false;
			}
			exists = position < val.size;
		}
		if (type == JSONModifyType::INSERT && exists) {
			return false; // Existing values are never overwritten
		}
		if ((type == JSONModifyType::REPLACE || type == JSONModifyType::REMOVE) && !exists) {
			return false; // Missing paths are never created
		}
		remove = type == JSONModifyType::REMOVE;
		return true;
	}

	bool ParsePointer(const char *ptr, idx_t len) {
		tokens.clear();
		const auto end = ptr + len;
		while (ptr < end) {
			ptr++;
			tokens.emplace_back();
			auto &token = tokens.back();
			while (ptr < end && *ptr != '/') {
				if (*ptr == '~') {
					if (ptr + 1 == end || (ptr[1] != '0' && ptr[1] != '1')) {
						return false;
					}
					token += ptr[1] == '0' ? '~' : '/';
					ptr += 2;
					continue;
				}
				token += *ptr++;
			}
		}
		return true;
	}

	bool ResolvePointer() {
		for (idx_t i = 0; i < tokens.size(); i++) {
			auto &val = *path.back();
			const JSONNode *child = nullptr;
			bool append = false;
			if (val.type == JSONValueType::OBJECT) {
				key = tokens[i];
				child = val.Get(key);
			} else if (val.type == JSONValueType::ARRAY) {
				idx_t index;
				if (ParsePointerIndex(tokens[i], index)) {
					append = index == val.size || index == NumericLimits<idx_t>::Maximum();
					position = append ? val.size : index;
					child = val.Get(index);
				}
			} else {
				return false;
			}
			if (child && i + 1 < tokens.size()) {
				path.push_back(child);
				continue;
			}
			if (child) {
				if (type == JSONModifyType::INSERT) {
					return false;
				}
				remove = type == JSONModifyType::REMOVE;
				return true;
			}
			if (type == JSONModifyType::REPLACE || type == JSONModifyType::REMOVE ||
			    (val.type == JSONValueType::ARRAY && !append)) {
				return false;
			}
			for (idx_t j = i + 1; j < tokens.size(); j++) {
				created.push_back({true, tokens[j]});
			}
			return true;
		}
		return false;
	}

	void WriteNewValue(JSONStringWriter &writer, const JSONNode &new_val) {
		for (auto &container : created) {
			if (container.object) {
				writer.StartObject();
				writer.Key(container.key);
			} else {
				writer.StartArray();
			}
		}
		writer.Value(new_val);
		for (idx_t i = created.size(); i > 0; i--) {
			if (created[i - 1].object) {
				writer.EndObject();
			} else {
				writer.EndArray();
			}
		}
	}

	void WriteModified(JSONStringWriter &writer, idx_t depth, const JSONNode *new_val) {
		auto &val = *path[depth];
		const auto modified = depth + 1 == path.size();
		const auto next = modified ? nullptr : path[depth + 1];
		if (val.type == JSONValueType::OBJECT) {
			writer.StartObject();
			bool written = false;
			for (auto &member_key : val.Keys()) {
				auto &member = *(&member_key + 1);
				if (modified && member_key.GetString() == key) {
					if (!remove && !written) {
						writer.Key(member_key.GetString());
						WriteNewValue(writer, *new_val);
						written = true;
					}
					continue;
				}
				writer.Key(member_key.GetString());
				if (&member == next) {
					WriteModified(writer, depth + 1, new_val);
				} else {
					writer.Value(member);
				}
			}
			if (modified && !remove && !written) {
				writer.Key(key);
				WriteNewValue(writer, *new_val);
			}
			writer.EndObject();
			return;
		}
		writer.StartArray();
		idx_t index = 0;
		for (auto &element : val.Elements()) {
			if (modified && index == position) {
				if (!remove) {
					WriteNewValue(writer, *new_val);
				}
			} else if (&element == next) {
				WriteModified(writer, depth + 1, new_val);
			} else {
				writer.Value(element);
			}
			index++;
		}
		if (modified && !remove && position == val.size) {
			WriteNewValue(writer, *new_val);
		}
		writer.EndArray();
	}

private:
	const JSONModifyType type;
	const JSONModifyFunctionData &info;
	vector<const JSONNode *> path;
	std::string_view key;
	idx_t position = 0;
	bool remove = false;
	vector<JSONModifyContainer> created;
	vector<string> tokens;
};

//! Shared implementation of json_set, json_insert, and json_replace
static void ModifyFunction(JSONModifyType type, DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONModifyFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	JSONModifier modifier(type, info);

	TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, [&](string_t doc_str, string_t path_str, string_t val_str) {
		    auto &root = JSONCommon::ReadDocument(lstate.parser, doc_str);
		    auto &new_val = JSONCommon::ReadDocument(lstate.second_parser, val_str);

		    lstate.writer.Clear();
		    modifier.Write(lstate.writer, root, path_str, &new_val);
		    return StringVector::AddString(result, lstate.writer.View());
	    });
}

//! Set a value at a path in a JSON document (create if missing, overwrite if exists)
static void JsonSetFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ModifyFunction(JSONModifyType::SET, args, state, result);
}

//! Insert a value at a path in a JSON document (no-op if a value already exists at the path)
static void JsonInsertFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ModifyFunction(JSONModifyType::INSERT, args, state, result);
}

//! Replace the value at a path in a JSON document (no-op if the path does not exist)
static void JsonReplaceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ModifyFunction(JSONModifyType::REPLACE, args, state, result);
}

//! Remove the value at a path in a JSON document (no-op if the path does not exist)
static void JsonRemoveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONModifyFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	JSONModifier modifier(JSONModifyType::REMOVE, info);

	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, [&](string_t doc_str, string_t path_str) -> optional<string_t> {
		    auto &root = JSONCommon::ReadDocument(lstate.parser, doc_str);

		    lstate.writer.Clear();
		    if (!modifier.Write(lstate.writer, root, path_str, nullptr)) {
			    return nullopt; // Removing the root leaves no document behind
		    }
		    return StringVector::AddString(result, lstate.writer.View());
	    });
}

ScalarFunctionSet JSONFunctions::GetSetFunction() {
	ScalarFunction fun("json_set", {}, LogicalType::JSON(), JsonSetFunction, JSONModifyFunctionData::Bind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature()
	    .AddParameter("json", LogicalType::JSON())
	    .AddParameter("path", LogicalType::VARCHAR)
	    .AddParameter("value", LogicalType::JSON());
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetInsertFunction() {
	ScalarFunction fun("json_insert", {}, LogicalType::JSON(), JsonInsertFunction, JSONModifyFunctionData::Bind,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature()
	    .AddParameter("json", LogicalType::JSON())
	    .AddParameter("path", LogicalType::VARCHAR)
	    .AddParameter("value", LogicalType::JSON());
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetReplaceFunction() {
	ScalarFunction fun("json_replace", {}, LogicalType::JSON(), JsonReplaceFunction, JSONModifyFunctionData::Bind,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature()
	    .AddParameter("json", LogicalType::JSON())
	    .AddParameter("path", LogicalType::VARCHAR)
	    .AddParameter("value", LogicalType::JSON());
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetRemoveFunction() {
	ScalarFunction fun("json_remove", {}, LogicalType::JSON(), JsonRemoveFunction, JSONModifyFunctionData::Bind,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", LogicalType::JSON()).AddParameter("path", LogicalType::VARCHAR);
	// throws for invalid JSON paths
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

} // namespace duckdb
