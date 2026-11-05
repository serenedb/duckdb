//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/function_serialization.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {
class ClientContext;

class FunctionSerializer {
private:
	static QualifiedName DeserializeQualifiedName(Deserializer &deserializer) {
		return deserializer.ReadProperty<QualifiedName>(507, "qname");
	}

	static CatalogEntry &LookupFunction(ClientContext &context, CatalogType type, const QualifiedName &name) {
		auto &system = Catalog::GetSystemCatalog(context);
		auto &catalog = name.Catalog();
		if (!catalog.empty() && catalog != Identifier::SystemCatalog() && catalog != system.GetName()) {
			return Catalog::GetEntry(context, type, name);
		}
		auto &schema = name.Schema();
		return system.GetEntry(
		    context, type,
		    schema.empty() || schema == Identifier::DefaultSchema() ? *system.GetDefaultSchema() : schema, name.Name());
	}

	class DeserializeContext {
	public:
		DeserializeContext(Deserializer &deserializer_p, const LogicalType &return_type,
		                   const const_expression_list_t &children)
		    : deserializer(deserializer_p) {
			deserializer.Set<const LogicalType &>(return_type);
			try {
				deserializer.Set<const const_expression_list_t &>(children);
			} catch (...) {
				deserializer.Unset<LogicalType>();
				throw;
			}
		}
		~DeserializeContext() { // NOLINT(bugprone-exception-escape): Unset only throws if the stack invariant is
			                    // broken.
			deserializer.Unset<const_expression_list_t>();
			deserializer.Unset<LogicalType>();
		}
		DeserializeContext(const DeserializeContext &) = delete;
		DeserializeContext &operator=(const DeserializeContext &) = delete;

	private:
		Deserializer &deserializer;
	};

	template <class FUNC>
	static void RestoreLogicalSignature(FUNC &function, const vector<unique_ptr<Expression>> &children,
	                                    const LogicalType &return_type) {
		auto arguments = function.GetArguments();
		for (idx_t index = 0; index < arguments.size() && index < children.size(); index++) {
			if (!arguments[index].IsComplete()) {
				arguments[index] = children[index]->GetReturnType();
			}
		}
		function.SetLogicalArguments(std::move(arguments));
		function.SetLogicalReturnType(return_type.IsAggregateState() ? function.GetReturnType() : return_type);
	}

public:
	template <class FUNC>
	static void Serialize(Serializer &serializer, const FUNC &function, optional_ptr<FunctionData> bind_info) {
		D_ASSERT(!function.GetName().empty());
		serializer.WriteProperty(501, "arguments", function.GetArguments());
		serializer.WriteProperty(507, "qname", function.GetQualifiedName());

		bool has_serialize = function.HasSerializationCallbacks();
		serializer.WriteProperty(503, "has_serialize", has_serialize);
		if constexpr (std::is_base_of_v<BoundSimpleFunction, FUNC>) {
			SerializeNamedArguments(serializer, function);
		}
		if (has_serialize) {
			serializer.WriteObject(504, "function_data",
			                       [&](Serializer &obj) { function.GetSerializeCallback()(obj, bind_info, function); });
			D_ASSERT(function.GetDeserializeCallback());
		}
	}

	//! Only written if the function was called with keyword-only or "**kwargs" arguments
	template <class FUNC>
	static void SerializeNamedArguments(Serializer &serializer, const FUNC &function) {
		auto &named_arguments = function.GetNamedArguments();
		if (named_arguments.empty()) {
			return;
		}
		serializer.WriteProperty(508, "positional_arguments", function.GetPositionalArgumentCount());
		serializer.WriteProperty(509, "named_arguments", named_arguments);
	}

	//! Splits the argument types a plan recorded into those passed by position and those passed by name
	static void SplitArgumentTypes(const QualifiedName &qualified_name, const vector<LogicalType> &arguments,
	                               idx_t positional_count, const vector<Identifier> &named_arguments,
	                               vector<LogicalType> &positional_types,
	                               vector<pair<Identifier, LogicalType>> &keyword_types) {
		if (positional_count + named_arguments.size() != arguments.size()) {
			throw SerializationException("Function %s has %llu argument types, but %llu positional and %llu named "
			                             "arguments",
			                             qualified_name.ToString(), arguments.size(), positional_count,
			                             named_arguments.size());
		}
		for (idx_t arg_idx = 0; arg_idx < arguments.size(); arg_idx++) {
			if (arg_idx < positional_count) {
				positional_types.push_back(arguments[arg_idx]);
			} else {
				keyword_types.emplace_back(named_arguments[arg_idx - positional_count], arguments[arg_idx]);
			}
		}
	}

	//! Deserializes a table function, and whether the plan holds its bind data. Without it, the caller binds again
	static pair<BoundTableFunction, bool> DeserializeTableFunction(Deserializer &deserializer) {
		auto &context = deserializer.Get<ClientContext &>();
		auto arguments = deserializer.ReadProperty<vector<LogicalType>>(501, "arguments");
		auto qualified_name = DeserializeQualifiedName(deserializer);
		auto has_serialize = deserializer.ReadProperty<bool>(503, "has_serialize");
		auto positional_count = deserializer.ReadPropertyWithDefault<idx_t>(508, "positional_arguments");
		auto named_arguments = deserializer.ReadPropertyWithDefault<vector<Identifier>>(509, "named_arguments");

		if (named_arguments.empty()) {
			positional_count = arguments.size();
		}

		auto &func_catalog = LookupFunction(context, CatalogType::TABLE_FUNCTION_ENTRY, qualified_name);
		if (func_catalog.type != CatalogType::TABLE_FUNCTION_ENTRY) {
			throw InternalException("DeserializeTableFunction - cant find catalog entry for function %s",
			                        qualified_name.Name().GetIdentifierName());
		}
		auto &functions = func_catalog.Cast<TableFunctionCatalogEntry>().functions;

		// the named arguments take part in selecting the overload, exactly as they did when the call was bound
		vector<LogicalType> positional_types;
		vector<pair<Identifier, LogicalType>> keyword_types;
		SplitArgumentTypes(qualified_name, arguments, positional_count, named_arguments, positional_types,
		                   keyword_types);
		FunctionBinder binder(context);
		ErrorData error;
		auto func_idx = binder.BindFunction(qualified_name.Name(), functions, positional_types, keyword_types, error);
		if (!func_idx.IsValid()) {
			throw SerializationException("Failed to find function %s(%s)\n%s", qualified_name.ToString(),
			                             StringUtil::ToString(arguments, ","), error.RawMessage());
		}

		BoundTableFunction function(functions.GetFunctionByOffset(func_idx.GetIndex()));
		function.GetArguments() = std::move(arguments);
		function.SetNamedArguments(positional_count, std::move(named_arguments));
		return make_pair(std::move(function), has_serialize);
	}

	template <class FUNC>
	static unique_ptr<FunctionData> FunctionDeserialize(Deserializer &deserializer, FUNC &function) {
		if (!function.GetDeserializeCallback()) {
			throw SerializationException("Function requires deserialization but no deserialization function for %s",
			                             function.GetName());
		}
		unique_ptr<FunctionData> result;
		deserializer.ReadObject(504, "function_data",
		                        [&](Deserializer &obj) { result = function.GetDeserializeCallback()(obj, function); });
		return result;
	}

	static bool TypeRequiresAssignment(const LogicalType &type) {
		switch (type.id()) {
		case LogicalTypeId::SQLNULL:
		case LogicalTypeId::ANY:
		case LogicalTypeId::INVALID:
			return true;
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::UNION:
		case LogicalTypeId::VARIANT:
		case LogicalTypeId::MAP:
			if (!type.HasParameters()) {
				return true;
			}
			return false;
		case LogicalTypeId::LIST:
			if (!type.HasParameters()) {
				return true;
			}
			return TypeRequiresAssignment(ListType::GetChildType(type));
		case LogicalTypeId::ARRAY:
			if (!type.HasParameters()) {
				return true;
			}
			return TypeRequiresAssignment(ArrayType::GetChildType(type));
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::TUPLE:
			if (!type.HasParameters()) {
				return true;
			}
			if (StructType::GetChildCount(type) == 0) {
				return true;
			}
			return false;
		default:
			return false;
		}
	}

	template <class FUNC, class CATALOG_ENTRY>
	static pair<FUNC, unique_ptr<FunctionData>> Deserialize(Deserializer &deserializer, CatalogType catalog_type,
	                                                        vector<unique_ptr<Expression>> &children,
	                                                        LogicalType return_type) { // NOLINT: clang-tidy bug
		auto &context = deserializer.Get<ClientContext &>();

		auto arguments = deserializer.ReadProperty<vector<LogicalType>>(501, "arguments");
		auto qualified_name = DeserializeQualifiedName(deserializer);
		auto has_serialize = deserializer.ReadProperty<bool>(503, "has_serialize");
		auto positional_count = deserializer.ReadPropertyWithDefault<idx_t>(508, "positional_arguments");
		auto named_arguments = deserializer.ReadPropertyWithDefault<vector<Identifier>>(509, "named_arguments");

		if (arguments.empty() && !children.empty()) {
			// The function is specified as having no arguments, but somehow expressions were passed anyway
			// Assume this is a "varargs" function and use the types of the expressions as the arguments
			// This can happen when we change a function that used to take varargs, to no longer do so.
			arguments.reserve(children.size());
			for (auto &child : children) {
				arguments.push_back(child->GetReturnType());
			}
		}

		// Now lookup the function in the catalog.
		auto &func_catalog = LookupFunction(context, catalog_type, qualified_name);

		if (func_catalog.type != catalog_type) {
			throw InternalException("DeserializeFunction - cant find catalog entry for function %s",
			                        qualified_name.Name().GetIdentifierName());
		}

		auto &functions = func_catalog.Cast<CATALOG_ENTRY>().functions;

		// If there are no argument names serialized, treat all arguments as positional
		if (named_arguments.empty()) {
			positional_count = arguments.size();
		}

		// Sanity check: The named arguments are the last arguments, so the number of arguments has to add up
		const auto has_valid_count = positional_count + named_arguments.size() == arguments.size();
		if (!has_valid_count || (!named_arguments.empty() && children.size() != arguments.size())) {
			throw SerializationException(
			    "Function %s has %llu argument types and %llu arguments, but %llu positional and %llu named arguments",
			    qualified_name.ToString(), arguments.size(), children.size(), positional_count, named_arguments.size());
		}

		// Split types by positional and keyword arguments
		vector<LogicalType> positional_types;
		vector<pair<Identifier, LogicalType>> keyword_types;

		for (idx_t arg_idx = 0; arg_idx < arguments.size(); arg_idx++) {
			if (arg_idx < positional_count) {
				positional_types.push_back(arguments[arg_idx]);
			} else {
				const auto kw_idx = arg_idx - positional_count;
				keyword_types.emplace_back(named_arguments[kw_idx], arguments[arg_idx]);
			}
		}

		// Lookup the function (and the correct overload)
		FunctionBinder binder(context);
		ErrorData error;
		const auto func_idx =
		    binder.BindFunction(qualified_name.Name(), functions, positional_types, keyword_types, error);

		// Ensure the function overload was found
		if (!func_idx.IsValid()) {
			throw SerializationException("Failed to find function %s(%s)\n%s", qualified_name.ToString(),
			                             StringUtil::ToString(arguments, ","), error.RawMessage());
		}

		const auto function = functions.GetFunctionByOffset(func_idx.GetIndex());

		// Does this function support serializing its bound data?
		if (!has_serialize) {
			// No, then just rebind the function
			try {
				// Split children into positional and keyword args expression
				vector<pair<Identifier, unique_ptr<Expression>>> keyword_args;
				if (!named_arguments.empty()) {
					for (idx_t i = positional_count; i < children.size(); i++) {
						keyword_args.emplace_back(named_arguments[i - positional_count], std::move(children[i]));
					}
					children.resize(positional_count);
				}

				// Resolve function
				auto [bound_function, bound_data] = binder.ResolveFunction(function, children, keyword_args);

				if (TypeRequiresAssignment(bound_function.GetReturnType())) {
					bound_function.SetReturnType(std::move(return_type));
				}

				return make_pair(std::move(bound_function), std::move(bound_data));
			} catch (std::exception &ex) {
				ErrorData inner_error(ex);
				throw SerializationException("Error during bind of function in deserialization: %s",
				                             inner_error.RawMessage());
			}
		}

		// Otherwise, construct the bound function from its parts
		FUNC bound_function(function);
		bound_function.GetArguments() = std::move(arguments);

		bound_function.SetNamedArguments(positional_count, std::move(named_arguments));
		RestoreLogicalSignature(bound_function, children, return_type);

		// Invoke deserialization function
		const_expression_list_t child_references;
		for (auto &child : children) {
			child_references.emplace_back(*child);
		}
		unique_ptr<FunctionData> bound_data;
		{
			DeserializeContext scope(deserializer, return_type, child_references);
			bound_data = FunctionDeserialize(deserializer, bound_function);
		}

		if (TypeRequiresAssignment(bound_function.GetReturnType())) {
			bound_function.SetReturnType(std::move(return_type));
		}

		return make_pair(std::move(bound_function), std::move(bound_data));
	}
};

} // namespace duckdb
