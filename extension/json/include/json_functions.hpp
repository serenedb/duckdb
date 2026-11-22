//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "json_common.hpp"

namespace duckdb {

class TableRef;
class Expression;
class ClientContext;
struct ReplacementScanData;
class CastFunctionSet;
struct CastParameters;
struct CastLocalStateParameters;
struct JSONScanInfo;
class BuiltinFunctions;

// Scalar function stuff
struct JSONReadFunctionData : public FunctionData {
public:
	JSONReadFunctionData(bool constant, string path_p, idx_t len, JSONCommon::JSONPathType path_type);
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;
	static JSONCommon::JSONPathType CheckPath(const Value &path_val, string &path, idx_t &len);
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input);

public:
	const bool constant;
	const string path;
	const JSONCommon::JSONPathType path_type;
	const char *ptr;
	const idx_t len;
	//! Path elements parsed once at bind time (constant regular '$' paths only)
	vector<JSONPathElement> elements;
	bool use_elements = false;
	//! The elements as steps that the parser follows while it parses, when they have no reverse index
	vector<JSONPathStep> steps;
	bool use_steps = false;
	//! Whether JSONTreeParser::WritePath can write the value at the path (steps, or none for the root)
	bool writable = false;
	//! For a wildcard path that starts with keys and indexes: those as steps, and the rest of the path after '$'
	bool use_wildcard_steps = false;
	string wildcard_rest;
};

struct JSONReadManyFunctionData : public FunctionData {
public:
	JSONReadManyFunctionData(vector<string> paths_p, vector<idx_t> lens_p);
	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input);

public:
	const vector<string> paths;
	vector<const char *> ptrs;
	const vector<idx_t> lens;
	//! Per-path elements parsed once at bind time (regular '$' paths only)
	vector<vector<JSONPathElement>> elements;
	vector<bool> use_elements;
	//! The paths as steps that the parser follows while it parses, when every path can be
	vector<vector<JSONPathStep>> steps;
	bool use_steps = false;
};

struct JSONFunctionLocalState : public FunctionLocalState {
public:
	explicit JSONFunctionLocalState(Allocator &allocator, JSONReadFlags read_flags = JSONCommon::READ_FLAG);
	explicit JSONFunctionLocalState(ClientContext &context);
	static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
	                                           FunctionData *bind_data);
	static unique_ptr<FunctionLocalState> InitCastLocalState(CastLocalStateParameters &parameters);
	static JSONFunctionLocalState &ResetAndGet(ExpressionState &state);

public:
	//! Scratch memory of the current chunk
	ArenaAllocator arena;
	//! Parses the documents of the function's input, one at a time
	JSONTreeParser parser;
	//! Parses a second document per row, for the functions that take two
	JSONTreeParser second_parser;
	//! Writes the JSON text of the function's results
	JSONStringWriter writer;
};

class JSONFunctions {
public:
	static vector<ScalarFunctionSet> GetScalarFunctions();
	static vector<PragmaFunctionSet> GetPragmaFunctions();
	static vector<TableFunctionSet> GetTableFunctions();
	static unique_ptr<TableRef> ReadJSONReplacement(ClientContext &context, ReplacementScanInput &input,
	                                                optional_ptr<ReplacementScanData> data);
	static TableFunction GetJSONTableFunction(Identifier name, shared_ptr<JSONScanInfo> function_info);
	static CopyFunction GetJSONCopyFunction();
	static ScalarFunction GetJSONCopyToJSONFunction();
	static ScalarFunction GetJSONCopyToGeoJSONFunction();
	static CopyFunction GetGeoJSONCopyFunction();
	static unique_ptr<Expression> CreateJSONCopyToJSONExpression(ClientContext &context, unique_ptr<Expression> payload,
	                                                             unique_ptr<Expression> date_format,
	                                                             unique_ptr<Expression> timestamp_format);
	//! Validation callback for the json_geometry_format setting, so an invalid value is rejected by SET rather
	//! than by every subsequent conversion
	static void ValidateGeometryFormat(ClientContext &context, SetScope scope, Value &parameter);
	static void RegisterSimpleCastFunctions(ExtensionLoader &loader);
	static void RegisterJSONCreateCastFunctions(ExtensionLoader &loader);
	static void RegisterJSONTransformCastFunctions(ExtensionLoader &loader);

private:
	// Scalar functions
	static ScalarFunctionSet GetExtractFunction();
	static ScalarFunctionSet GetExtractStringFunction();

	static ScalarFunctionSet GetArrayFunction();
	static ScalarFunctionSet GetObjectFunction();
	static ScalarFunctionSet GetToJSONFunction();
	static ScalarFunctionSet GetArrayToJSONFunction();
	static ScalarFunctionSet GetRowToJSONFunction();
	static ScalarFunctionSet GetMergePatchFunction();
	static ScalarFunctionSet GetMergePatchDiffFunction();
	static ScalarFunctionSet GetDeepMergeFunction();

	static ScalarFunctionSet GetStructureFunction();
	static ScalarFunctionSet GetTransformFunction();
	static ScalarFunctionSet GetTransformStrictFunction();

	static ScalarFunctionSet GetAsGeoJSONFunction();
	static ScalarFunctionSet GetGeomFromGeoJSONFunction();

	static ScalarFunctionSet GetArrayLengthFunction();
	static ScalarFunctionSet GetContainsFunction();
	static ScalarFunctionSet GetExistsFunction();
	static ScalarFunctionSet GetKeysFunction();
	static ScalarFunctionSet GetTypeFunction();
	static ScalarFunctionSet GetValidFunction();
	static ScalarFunctionSet GetValueFunction();
	static ScalarFunctionSet GetSerializeSqlFunction();
	static ScalarFunctionSet GetDeserializeSqlFunction();
	static ScalarFunctionSet GetSerializePlanFunction();

	static ScalarFunctionSet GetPrettyPrintFunction();
	static ScalarFunctionSet GetNormalizeFunction();
	static ScalarFunctionSet GetStripNullsFunction();
	static ScalarFunctionSet GetInsertFunction();
	static ScalarFunctionSet GetRemoveFunction();
	static ScalarFunctionSet GetReplaceFunction();
	static ScalarFunctionSet GetSetFunction();

	static PragmaFunctionSet GetExecuteJsonSerializedSqlPragmaFunction();

	template <class FUNCTION_INFO>
	static void AddAliases(const vector<string> &names, FUNCTION_INFO fun, vector<FUNCTION_INFO> &functions) {
		for (auto &name : names) {
			fun.name = Identifier(name);
			functions.push_back(fun);
		}
	}

private:
	// Table functions
	static TableFunctionSet GetReadJSONObjectsFunction();
	static TableFunctionSet GetReadNDJSONObjectsFunction();
	static TableFunctionSet GetReadJSONObjectsAutoFunction();

	static TableFunctionSet GetReadJSONFunction();
	static TableFunctionSet GetReadNDJSONFunction();
	static TableFunctionSet GetReadJSONAutoFunction();
	static TableFunctionSet GetReadNDJSONAutoFunction();
	static TableFunction GetReadSingleJSONFileTableFunction(shared_ptr<JSONScanInfo> function_info);
	static TableFunctionSet GetReadSingleJSONFileFunction();

	static TableFunctionSet GetJSONEachFunction();
	static TableFunctionSet GetJSONTreeFunction();

	static TableFunctionSet GetExecuteJsonSerializedSqlFunction();
};

} // namespace duckdb
