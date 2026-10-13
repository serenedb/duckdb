#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/main/client_context.hpp"

#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/common/enums/dialect_compatibility_mode.hpp"
#include "duckdb/main/settings.hpp"
namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper functions
//===--------------------------------------------------------------------===//

//! LambdaExecuteInfo holds information for executing the lambda expression on an input chunk and
//! a resulting lambda chunk.
struct LambdaExecuteInfo final : public FunctionLocalState {
	LambdaExecuteInfo(ClientContext &context, const Expression &lambda_expr, const BoundFunctionExpression &expr,
	                  const bool has_index)
	    : has_index(has_index), index_vector(LogicalType::BIGINT), child_sel(STANDARD_VECTOR_SIZE),
	      row_sel(STANDARD_VECTOR_SIZE) {
		expr_executor = make_uniq<ExpressionExecutor>(context, lambda_expr);

		// get the input types for the input chunk
		vector<LogicalType> input_types;
		if (has_index) {
			input_types.push_back(LogicalType::BIGINT);
		}
		auto &children = expr.GetChildren();
		input_types.push_back(ListType::GetChildType(children[0]->GetReturnType()));
		for (idx_t i = 1; i < children.size(); i++) {
			if (children[i]->GetReturnType().id() == LogicalTypeId::LAMBDA) {
				// placeholder slot for the lambda argument itself
				continue;
			}
			input_types.push_back(children[i]->GetReturnType());
		}

		// get the result types
		vector<LogicalType> result_types {lambda_expr.GetReturnType()};

		// initialize the data chunks
		input_chunk.InitializeEmpty(input_types);
		lambda_chunk.Initialize(Allocator::DefaultAllocator(), result_types);
		slice_sels.reserve(input_types.size());
		for (idx_t i = 0; i < input_types.size(); i++) {
			slice_sels.emplace_back(STANDARD_VECTOR_SIZE);
		}
		// Spark Compatibility Mode: zero-based index for lambdas
		if (Settings::Get<DialectCompatibilityModeSetting>(context) == DialectCompatibilityMode::SPARK) {
			// Spark's lambda index parameter is 0-based; default SQL is 1-based
			index_offset = 0;
		}
	};

	//! The expression executor that executes the lambda expression
	unique_ptr<ExpressionExecutor> expr_executor;
	//! The input chunk on which we execute the lambda expression
	DataChunk input_chunk;
	//! The chunk holding the result of executing the lambda expression
	DataChunk lambda_chunk;
	//! True, if this lambda expression expects an index vector in the input chunk
	bool has_index;
	//! Added to child_idx to form the value the lambda sees in its index parameter (1 by default).
	idx_t index_offset = 1;
	Vector index_vector;
	SelectionVector child_sel;
	SelectionVector row_sel;
	vector<SelectionVector> slice_sels;
};

struct LambdaBatch {
	void Add(LambdaExecuteInfo &info, const idx_t child_position, const idx_t row_idx) {
		info.child_sel.set_index(count, child_position);
		info.row_sel.set_index(count, row_idx);
		if (count == 0) {
			first_child = child_position;
			first_row = row_idx;
			consecutive_children = true;
			consecutive_rows = true;
		} else {
			consecutive_children = consecutive_children && child_position == first_child + count;
			consecutive_rows = consecutive_rows && row_idx == first_row + count;
		}
		count++;
	}

	idx_t count = 0;
	idx_t first_child = 0;
	idx_t first_row = 0;
	bool consecutive_children = true;
	bool consecutive_rows = true;
};

//! A helper struct with information that is specific to the list_filter function
struct ListFilterInfo {
	//! The new list lengths after filtering out elements
	vector<idx_t> entry_lengths;
	//! The length of the current list
	idx_t length = 0;
	//! The offset of the current list
	idx_t offset = 0;
	//! The current row index
	idx_t row_idx = 0;
	//! The length of the source list
	idx_t src_length = 0;
};

//! ListTransformFunctor contains list_transform specific functionality
struct ListTransformFunctor {
	static void ReserveNewLengths(vector<idx_t> &, const idx_t) {
		// NOP
	}
	static void PushEmptyList(vector<idx_t> &) {
		// NOP
	}
	//! Sets the list entries of the result vector
	static void SetResultEntry(list_entry_t *result_entries, idx_t &offset, const list_entry_t &entry,
	                           const idx_t row_idx, vector<idx_t> &) {
		result_entries[row_idx].offset = offset;
		result_entries[row_idx].length = entry.length;
		offset += entry.length;
	}
	//! Appends the lambda vector to the result's child vector
	static void AppendResult(Vector &result, const Vector &lambda_vector, const idx_t elem_cnt, list_entry_t *,
	                         ListFilterInfo &, LambdaExecuteInfo &) {
		ListVector::Append(result, lambda_vector, elem_cnt);
	}
};

//! ListFilterFunctor contains list_filter specific functionality
struct ListFilterFunctor {
	//! Initializes the entry_lengths vector
	static void ReserveNewLengths(vector<idx_t> &entry_lengths, const idx_t row_count) {
		entry_lengths.reserve(row_count);
	}
	//! Pushes an empty list to the entry_lengths vector
	static void PushEmptyList(vector<idx_t> &entry_lengths) {
		entry_lengths.emplace_back(0);
	}
	//! Pushes the length of the original list to the entry_lengths vector
	static void SetResultEntry(list_entry_t *, idx_t &, const list_entry_t &entry, const idx_t,
	                           vector<idx_t> &entry_lengths) {
		entry_lengths.push_back(entry.length);
	}
	//! Uses the lambda vector to filter the incoming list and to append the filtered list to the result vector
	static void AppendResult(Vector &result, const Vector &lambda_vector, const idx_t elem_cnt,
	                         list_entry_t *result_entries, ListFilterInfo &info, LambdaExecuteInfo &execute_info) {
		idx_t count = 0;
		SelectionVector sel(elem_cnt);

		// compute the new lengths and offsets, and create a selection vector
		for (auto entry : lambda_vector.Values<bool>()) {
			// set length and offset of empty lists
			while (info.row_idx < info.entry_lengths.size() && !info.entry_lengths[info.row_idx]) {
				result_entries[info.row_idx].offset = info.offset;
				result_entries[info.row_idx].length = 0;
				info.row_idx++;
			}

			// found a true value
			if (entry.IsValid() && entry.GetValue()) {
				sel.set_index(count++, entry.GetIndex());
				info.length++;
			}

			info.src_length++;

			// we traversed the entire source list
			if (info.entry_lengths[info.row_idx] == info.src_length) {
				// set the offset and length of the result entry
				result_entries[info.row_idx].offset = info.offset;
				result_entries[info.row_idx].length = info.length;

				// reset all other fields
				info.offset += info.length;
				info.row_idx++;
				info.length = 0;
				info.src_length = 0;
			}
		}

		// set length and offset of all remaining empty lists
		while (info.row_idx < info.entry_lengths.size() && !info.entry_lengths[info.row_idx]) {
			result_entries[info.row_idx].offset = info.offset;
			result_entries[info.row_idx].length = 0;
			info.row_idx++;
		}

		// slice the input chunk's corresponding vector to get the new lists
		// and append them to the result
		idx_t source_list_idx = execute_info.has_index ? 1 : 0;
		Vector result_lists(execute_info.input_chunk.data[source_list_idx], sel, count);
		ListVector::Append(result, result_lists, count);
	}
};

unique_ptr<FunctionLocalState> LambdaFunctions::ListLambdaInitLocalState(ExpressionState &state,
                                                                         const BoundFunctionExpression &expr,
                                                                         FunctionData *bind_data) {
	if (!bind_data) {
		return nullptr;
	}
	auto &info = bind_data->Cast<ListLambdaBindData>();
	if (!info.lambda_expr) {
		return nullptr;
	}
	return make_uniq<LambdaExecuteInfo>(state.GetContext(), *info.lambda_expr, expr, info.has_index);
}

static void SliceBatchInput(Vector &target, Vector &source, const bool consecutive, const idx_t first,
                            const SelectionVector &sel, const idx_t count, SelectionVector &composed_sel) {
	if (consecutive && first == 0 && count == source.size() && source.GetVectorType() != VectorType::CONSTANT_VECTOR) {
		target.Reference(source);
		return;
	}
	if (source.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		auto &dictionary = DictionaryVector::Child(source);
		if (dictionary.GetVectorType() == VectorType::FLAT_VECTOR) {
			auto &dictionary_sel = DictionaryVector::SelVector(source);
			for (idx_t i = 0; i < count; i++) {
				composed_sel.set_index(i, dictionary_sel.get_index(sel.get_index(i)));
			}
			target.Slice(dictionary, composed_sel, count);
			return;
		}
	}
	target.Slice(source, sel, count);
}

static void ExecuteExpression(const LambdaBatch &batch, Vector &child_vector, std::span<Vector> columns,
                              LambdaExecuteInfo &info) {
	idx_t column_idx = 0;
	if (info.has_index) {
		info.input_chunk.data[column_idx++].Reference(info.index_vector);
	}
	SliceBatchInput(info.input_chunk.data[column_idx], child_vector, batch.consecutive_children, batch.first_child,
	                info.child_sel, batch.count, info.slice_sels[column_idx]);
	column_idx++;
	for (auto &column : columns) {
		SliceBatchInput(info.input_chunk.data[column_idx], column, batch.consecutive_rows, batch.first_row,
		                info.row_sel, batch.count, info.slice_sels[column_idx]);
		column_idx++;
	}

	// ensure all input vectors are sized to the chunk cardinality (some references inherit a different size)
	info.input_chunk.SetChildCardinality(batch.count);

	// execute the lambda expression
	info.expr_executor->Execute(info.input_chunk, info.lambda_chunk);
}

//===--------------------------------------------------------------------===//
// ListLambdaBindData
//===--------------------------------------------------------------------===//

void ListLambdaBindData::Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                                   const BoundScalarFunction &) {
	auto &bind_data = bind_data_p->Cast<ListLambdaBindData>();
	serializer.WriteProperty(100, "return_type", bind_data.return_type);
	serializer.WritePropertyWithDefault(101, "lambda_expr", bind_data.lambda_expr, shared_ptr<Expression>());
	serializer.WriteProperty(102, "has_index", bind_data.has_index);
	serializer.WritePropertyWithDefault<bool>(103, "has_initial", bind_data.has_initial, false);
}

unique_ptr<FunctionData> ListLambdaBindData::Deserialize(Deserializer &deserializer, BoundScalarFunction &) {
	auto return_type = deserializer.ReadProperty<LogicalType>(100, "return_type");
	auto lambda_expr = deserializer.ReadPropertyWithExplicitDefault<unique_ptr<Expression>>(101, "lambda_expr",
	                                                                                        unique_ptr<Expression>());
	auto has_index = deserializer.ReadProperty<bool>(102, "has_index");
	auto has_initial = deserializer.ReadPropertyWithExplicitDefault<bool>(103, "has_initial", false);
	return make_uniq<ListLambdaBindData>(return_type, std::move(lambda_expr), has_index, has_initial);
}

//===--------------------------------------------------------------------===//
// LambdaFunctions
//===--------------------------------------------------------------------===//
LogicalType LambdaFunctions::DetermineListChildType(const LogicalType &child_type) {
	if (child_type.id() != LogicalTypeId::SQLNULL && child_type.id() != LogicalTypeId::UNKNOWN) {
		if (child_type.id() == LogicalTypeId::ARRAY) {
			return ArrayType::GetChildType(child_type);
		} else if (child_type.id() == LogicalTypeId::LIST) {
			return ListType::GetChildType(child_type);
		}
		throw InternalException("The first argument must be a list or array type");
	}

	return child_type;
}

LogicalType LambdaFunctions::BindBinaryChildren(const vector<LogicalType> &function_child_types,
                                                const idx_t parameter_idx) {
	auto list_type = DetermineListChildType(function_child_types[0]);

	switch (parameter_idx) {
	case 0:
		return list_type;
	case 1:
		return LogicalType::BIGINT;
	default:
		throw BinderException("This lambda function only supports up to two lambda parameters!");
	}
}

static bool IsSingleElementRun(const LambdaFunctions::LambdaInfo &info) {
	if (info.row_count == 0 || info.row_count > STANDARD_VECTOR_SIZE || info.list_column_format.sel->IsSet() ||
	    !info.list_column_format.validity.AllValid()) {
		return false;
	}
	const auto first_offset = info.list_entries[0].offset;
	bool single = true;
	for (idx_t row_idx = 0; row_idx < info.row_count; row_idx++) {
		const auto &entry = info.list_entries[row_idx];
		single &= entry.length == 1 && entry.offset == first_offset + row_idx;
	}
	return single;
}

template <class FUNCTION_FUNCTOR>
static void ExecuteLambda(DataChunk &args, ExpressionState &state, Vector &result) {
	bool result_is_null = false;
	LambdaFunctions::LambdaInfo info(args, state, result, result_is_null);
	if (result_is_null) {
		return;
	}

	auto result_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &execute_info = ExecuteFunctionState::GetFunctionState(state)->Cast<LambdaExecuteInfo>();
	auto index_data = FlatVector::GetDataMutable<int64_t>(execute_info.index_vector);

	// get list_filter specific info
	ListFilterInfo list_filter_info;
	FUNCTION_FUNCTOR::ReserveNewLengths(list_filter_info.entry_lengths, info.row_count);

	// loop over the child entries and create chunks to be executed by the expression executor
	LambdaBatch batch;
	idx_t offset = 0;
	if constexpr (std::is_same_v<FUNCTION_FUNCTOR, ListTransformFunctor>) {
		if (IsSingleElementRun(info)) {
			const auto first_child = info.list_entries[0].offset;
			for (idx_t row_idx = 0; row_idx < info.row_count; row_idx++) {
				execute_info.child_sel.set_index(row_idx, first_child + row_idx);
				execute_info.row_sel.set_index(row_idx, row_idx);
				result_entries[row_idx] = list_entry_t(row_idx, 1);
			}
			if (info.has_index) {
				for (idx_t row_idx = 0; row_idx < info.row_count; row_idx++) {
					index_data[row_idx] = NumericCast<int64_t>(execute_info.index_offset);
				}
			}
			batch.count = info.row_count;
			batch.first_child = first_child;
			batch.first_row = 0;
			execute_info.lambda_chunk.Reset();
			ExecuteExpression(batch, *info.child_vector, info.columns, execute_info);
			FUNCTION_FUNCTOR::AppendResult(result, execute_info.lambda_chunk.data[0], batch.count, result_entries,
			                               list_filter_info, execute_info);
			if (info.is_all_constant && !info.is_volatile) {
				result.SetVectorType(VectorType::CONSTANT_VECTOR);
			}
			FlatVector::SetSize(result, count_t(info.row_count));
			return;
		}
	}
	for (idx_t row_idx = 0; row_idx < info.row_count; row_idx++) {
		auto list_idx = info.list_column_format.sel->get_index(row_idx);
		const auto &list_entry = info.list_entries[list_idx];

		// set the result to NULL for this row
		if (!info.list_column_format.validity.RowIsValid(list_idx)) {
			info.result_validity->SetInvalid(row_idx);
			FUNCTION_FUNCTOR::PushEmptyList(list_filter_info.entry_lengths);
			continue;
		}

		FUNCTION_FUNCTOR::SetResultEntry(result_entries, offset, list_entry, row_idx, list_filter_info.entry_lengths);

		// empty list, nothing to execute
		if (list_entry.length == 0) {
			continue;
		}

		// iterate the elements of the current list and create the corresponding selection vectors
		for (idx_t child_idx = 0; child_idx < list_entry.length; child_idx++) {
			// reached STANDARD_VECTOR_SIZE elements
			if (batch.count == STANDARD_VECTOR_SIZE) {
				state.GetContext().InterruptCheck();
				execute_info.lambda_chunk.Reset();
				ExecuteExpression(batch, *info.child_vector, info.columns, execute_info);
				auto &lambda_vector = execute_info.lambda_chunk.data[0];

				FUNCTION_FUNCTOR::AppendResult(result, lambda_vector, batch.count, result_entries, list_filter_info,
				                               execute_info);
				batch.count = 0;
			}

			if (info.has_index) {
				index_data[batch.count] = NumericCast<int64_t>(child_idx + execute_info.index_offset);
			}
			batch.Add(execute_info, list_entry.offset + child_idx, row_idx);
		}
	}

	execute_info.lambda_chunk.Reset();
	if (batch.count > 0) {
		ExecuteExpression(batch, *info.child_vector, info.columns, execute_info);
	}
	auto &lambda_vector = execute_info.lambda_chunk.data[0];

	FUNCTION_FUNCTOR::AppendResult(result, lambda_vector, batch.count, result_entries, list_filter_info, execute_info);

	if (info.is_all_constant && !info.is_volatile) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
	FlatVector::SetSize(result, count_t(info.row_count));
}

unique_ptr<FunctionData> LambdaFunctions::ListLambdaPrepareBind(vector<unique_ptr<Expression>> &arguments,
                                                                ClientContext &context,
                                                                BoundScalarFunction &bound_function) {
	// NULL list parameter
	if (arguments[0]->GetReturnType().id() == LogicalTypeId::SQLNULL) {
		bound_function.GetArguments()[0] = LogicalType::SQLNULL;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		return make_uniq<ListLambdaBindData>(bound_function.GetReturnType(), nullptr);
	}
	// prepared statements
	if (arguments[0]->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	D_ASSERT(arguments[0]->GetReturnType().id() == LogicalTypeId::LIST);
	return nullptr;
}

unique_ptr<FunctionData> LambdaFunctions::ListLambdaBind(ClientContext &context, BoundScalarFunction &bound_function,
                                                         vector<unique_ptr<Expression>> &arguments,
                                                         const bool has_index) {
	unique_ptr<FunctionData> bind_data = ListLambdaPrepareBind(arguments, context, bound_function);
	if (bind_data) {
		return bind_data;
	}

	auto &bound_lambda_expr = arguments[1]->Cast<BoundLambdaExpression>();
	auto lambda_bind_data =
	    make_uniq<ListLambdaBindData>(bound_function.GetReturnType(), bound_lambda_expr.SharedLambdaExpr(), has_index);
	if (lambda_bind_data->is_volatile) {
		bound_function.SetVolatile();
	}
	return std::move(lambda_bind_data);
}

void LambdaFunctions::ListTransformFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecuteLambda<ListTransformFunctor>(args, state, result);
}

void LambdaFunctions::ListFilterFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	ExecuteLambda<ListFilterFunctor>(args, state, result);
}

} // namespace duckdb
