#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "json_deserializer.hpp"
#include "json_functions.hpp"
#include "json_serializer.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

struct JsonSerializeBindData : public FunctionData {
	bool skip_if_null = false;
	bool skip_if_empty = false;
	bool skip_if_default = false;
	bool format = false;

	JsonSerializeBindData(bool skip_if_null_p, bool skip_if_empty_p, bool skip_if_default_p, bool format_p)
	    : skip_if_null(skip_if_null_p), skip_if_empty(skip_if_empty_p), skip_if_default(skip_if_default_p),
	      format(format_p) {
	}

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JsonSerializeBindData>(skip_if_null, skip_if_empty, skip_if_default, format);
	}
	bool Equals(const FunctionData &other_p) const override {
		return true;
	}
};

static unique_ptr<FunctionData> JsonSerializeBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.empty()) {
		throw BinderException("json_serialize_sql takes at least one argument");
	}

	if (arguments[0]->GetReturnType() != LogicalType::VARCHAR) {
		throw InvalidTypeException("json_serialize_sql first argument must be a VARCHAR");
	}

	// Optional arguments

	bool skip_if_null = false;
	bool skip_if_empty = false;
	bool skip_if_default = false;
	bool format = false;

	for (idx_t i = 1; i < arguments.size(); i++) {
		auto &arg = arguments[i];
		auto &alias = arg->GetAlias();
		if (alias == "skip_null") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'skip_null' argument must be a boolean");
			}
			skip_if_null = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "skip_empty") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'skip_empty' argument must be a boolean");
			}
			skip_if_empty = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "format") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'format' argument must be a boolean");
			}
			format = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "skip_default") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'skip_default' argument must be a boolean");
			}
			skip_if_default = BooleanValue::Get(input.GetConstant(i));
		} else {
			throw BinderException(StringUtil::Format("json_serialize_sql: Unknown argument %s", alias));
		}
	}
	return make_uniq<JsonSerializeBindData>(skip_if_null, skip_if_empty, skip_if_default, format);
}

static void JsonSerializeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = JSONFunctionLocalState::ResetAndGet(state);
	const auto &inputs = args.data[0];

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JsonSerializeBindData>();

	JSONBuilder builder(local_state.arena);
	JSONStringWriter writer(info.format ? JSONCommon::WRITE_PRETTY_FLAG : JSONCommon::WRITE_FLAG);
	UnaryExecutor::Execute<string_t, string_t>(inputs, result, [&](string_t input) {
		writer.Clear();
		try {
			Parser parser(state.GetContext());
			parser.ParseQuery(input.GetString());

			writer.StartObject();
			writer.Key("error");
			writer.Boolean(false);
			writer.Key("statements");
			writer.StartArray();
			for (auto &statement : parser.statements) {
				if (statement->type != StatementType::SELECT_STATEMENT) {
					throw NotImplementedException("Only SELECT statements can be serialized to json!");
				}
				auto &select = statement->Cast<SelectStatement>();

				auto options = make_uniq<SerializationOptions>();
				options->storage_compatibility = state.GetContext().db->config.options.storage_compatibility;
				auto json = JsonSerializer::Serialize(select, builder, info.skip_if_null, info.skip_if_empty,
				                                      info.skip_if_default, *options);

				writer.Value(*json);
			}
			writer.EndArray();
			writer.EndObject();
		} catch (std::exception &ex) {
			ErrorData error(ex);
			writer.Clear();
			writer.StartObject();
			writer.Key("error");
			writer.Boolean(true);
			writer.Key("error_type");
			writer.String(StringUtil::Lower(Exception::ExceptionTypeToString(error.Type())));
			writer.Key("error_message");
			writer.String(error.RawMessage().c_str());
			// add extra info
			for (auto &entry : error.ExtraInfo()) {
				writer.Key(entry.first.c_str());
				writer.String(entry.second.c_str());
			}
			writer.EndObject();
		}
		return StringVector::AddString(result, writer.View());
	});
}

ScalarFunctionSet JSONFunctions::GetSerializeSqlFunction() {
	ScalarFunctionSet set("json_serialize_sql");

	ScalarFunction func({}, LogicalType::JSON(), JsonSerializeFunction, JsonSerializeBind, nullptr,
	                    JSONFunctionLocalState::Init);
	func.GetProperties().SetRequiresExpressionNames(true);

	func.GetSignature()
	    .AddParameter("sql", LogicalType::VARCHAR)
	    .AddParameter("skip_null", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("skip_empty", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("skip_default", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("format", LogicalType::BOOLEAN, Value::BOOLEAN(false));

	set.AddFunction(std::move(func));

	return set;
}

//----------------------------------------------------------------------
// JSON DESERIALIZE
//----------------------------------------------------------------------
static vector<unique_ptr<SelectStatement>> DeserializeSelectStatement(string_t input, JSONTreeParser &parser) {
	auto &root = JSONCommon::ReadDocument(parser, input);
	auto err = root.Get("error");
	if (err && err->type == JSONValueType::BOOLEAN && err->boolean) {
		auto err_type = root.Get("error_type");
		auto err_msg = root.Get("error_message");
		if (err_type && err_msg && err_type->type == JSONValueType::STRING && err_msg->type == JSONValueType::STRING) {
			throw ParserException("Error parsing json: %s: %s", string(err_type->GetString()),
			                      string(err_msg->GetString()));
		}
		throw ParserException(
		    "Error parsing json, expected error property to contain 'error_type' and 'error_message'");
	}

	auto statements = root.Get("statements");
	if (!statements || statements->type != JSONValueType::ARRAY) {
		throw ParserException("Error parsing json: no statements array");
	}
	if (statements->size == 0) {
		throw ParserException("Error parsing json: no statements");
	}

	vector<unique_ptr<SelectStatement>> result;

	for (auto &stmt_json : statements->Elements()) {
		JsonDeserializer deserializer(&stmt_json);
		auto stmt = SelectStatement::Deserialize(deserializer);
		if (!stmt->node) {
			throw ParserException("Error parsing json: no select node found in json");
		}
		ParsedExpressionIterator::EnumerateQueryNodeChildren(*stmt->node, [](unique_ptr<ParsedExpression> &child) {
			if (!child) {
				throw ParserException("Error parsing json: null expression found in json");
			}
		});
		result.push_back(std::move(stmt));
	}

	return result;
}

//----------------------------------------------------------------------
// JSON DESERIALIZE SQL FUNCTION
//----------------------------------------------------------------------
static void JsonDeserializeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = JSONFunctionLocalState::ResetAndGet(state);
	const auto &inputs = args.data[0];

	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(inputs, result, [&](string_t input) {
		auto stmts = DeserializeSelectStatement(input, local_state.parser);
		// Combine all statements into a single semicolon separated string
		string str;
		for (idx_t i = 0; i < stmts.size(); i++) {
			if (i > 0) {
				str += "; ";
			}
			str += stmts[i]->ToString();
		}

		return heap.AddString(str);
	});
}

ScalarFunctionSet JSONFunctions::GetDeserializeSqlFunction() {
	ScalarFunctionSet set("json_deserialize_sql");
	auto function = ScalarFunction({}, LogicalType::VARCHAR, JsonDeserializeFunction, nullptr, nullptr,
	                               JSONFunctionLocalState::Init);
	function.GetSignature().AddParameter("json", LogicalType::JSON());
	function.SetFallible();
	set.AddFunction(std::move(function));
	return set;
}

//----------------------------------------------------------------------
// JSON EXECUTE SERIALIZED SQL (PRAGMA)
//----------------------------------------------------------------------
static string ExecuteJsonSerializedSqlPragmaFunction(ClientContext &, const FunctionParameters &parameters) {
	JSONTreeParser parser(JSONCommon::READ_FLAG);

	if (parameters.values[0].IsNull()) {
		throw BinderException("json_execute_serialized_sql cannot execute NULL plan");
	}

	auto input = parameters.values[0].GetValueUnsafe<string_t>();
	auto stmts = DeserializeSelectStatement(input, parser);
	if (stmts.size() != 1) {
		throw BinderException("json_execute_serialized_sql pragma expects exactly one statement");
	}
	return stmts[0]->ToString();
}

PragmaFunctionSet JSONFunctions::GetExecuteJsonSerializedSqlPragmaFunction() {
	return PragmaFunctionSet(
	    PragmaFunction::PragmaCall("json_execute_serialized_sql", ExecuteJsonSerializedSqlPragmaFunction,
	                               FunctionSignature().AddPositionalOnly("serialized_sql", LogicalType::VARCHAR)));
}

//----------------------------------------------------------------------
// JSON EXECUTE SERIALIZED SQL (TABLE FUNCTION)
//----------------------------------------------------------------------
static unique_ptr<TableRef> ExecuteJsonSerializedSqlBindReplace(ClientContext &, TableFunctionBindInput &input) {
	JSONTreeParser parser(JSONCommon::READ_FLAG);

	if (input.inputs[0].IsNull()) {
		throw BinderException("json_execute_serialized_sql cannot execute NULL plan");
	}
	auto serialized = input.inputs[0].GetValueUnsafe<string>();
	auto stmts = DeserializeSelectStatement(serialized, parser);
	if (stmts.size() != 1) {
		throw BinderException("json_execute_serialized_sql expects exactly one statement");
	}
	return make_uniq<SubqueryRef>(std::move(stmts[0]));
}

TableFunctionSet JSONFunctions::GetExecuteJsonSerializedSqlFunction() {
	TableFunction func("json_execute_serialized_sql",
	                   FunctionSignature().AddPositionalOnly("serialized_sql", LogicalType::VARCHAR), nullptr, nullptr);
	func.bind_replace = ExecuteJsonSerializedSqlBindReplace;
	return TableFunctionSet(func);
}

} // namespace duckdb
