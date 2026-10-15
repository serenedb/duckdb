#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types.hpp"
#include "core_functions/scalar/map_functions.hpp"

#include <absl/container/flat_hash_map.h>

namespace duckdb {

namespace {

struct MapConcatInput {
	explicit MapConcatInput(const Vector &map) : map(map), entries(map.Values<list_entry_t>()) {
	}

	const VectorIterator<string_t> &SortKeys() {
		if (!sort_keys) {
			auto &keys = MapVector::GetKeys(map);
			sort_key_data = make_uniq<Vector>(LogicalType::BLOB, keys.size());
			MapVector::CreateKeySortKeys(keys, *sort_key_data);
			sort_keys = make_uniq<VectorIterator<string_t>>(*sort_key_data);
		}
		return *sort_keys;
	}

	const Vector &map;
	VectorIterator<list_entry_t> entries;
	unique_ptr<Vector> sort_key_data;
	unique_ptr<VectorIterator<string_t>> sort_keys;
};

struct MapConcatSource {
	idx_t input;
	idx_t index;
};

template <class GET_COMPONENT>
void GatherMapComponent(Vector &target, const vector<MapConcatInput> &inputs, const vector<MapConcatSource> &sources,
                        GET_COMPONENT get_component) {
	const auto total = sources.size();
	vector<idx_t> starts(inputs.size() + 1, 0);
	for (auto &source : sources) {
		starts[source.input + 1]++;
	}
	idx_t used_inputs = 0;
	for (idx_t i = 0; i < inputs.size(); i++) {
		used_inputs += starts[i + 1] != 0;
		starts[i + 1] += starts[i];
	}
	SelectionVector sel(total);
	if (used_inputs == 1) {
		for (idx_t i = 0; i < total; i++) {
			sel.set_index(i, sources[i].index);
		}
		target.Slice(get_component(inputs[sources[0].input].map), sel, total);
		target.Flatten();
		return;
	}
	vector<idx_t> next(starts.begin(), starts.end() - 1);
	SelectionVector input_sel(total);
	for (idx_t i = 0; i < total; i++) {
		auto position = next[sources[i].input]++;
		input_sel.set_index(position, sources[i].index);
		sel.set_index(i, position);
	}
	Vector staged(target.GetType(), total);
	for (idx_t i = 0; i < inputs.size(); i++) {
		const auto length = starts[i + 1] - starts[i];
		if (length == 0) {
			continue;
		}
		SelectionVector segment(input_sel.data() + starts[i], length);
		staged.Append(get_component(inputs[i].map), segment, length, VectorAppendMode::ALLOW_RESIZE);
	}
	target.Slice(staged, sel, total);
	target.Flatten();
}

void MapConcatFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	if (result.GetType().id() == LogicalTypeId::SQLNULL) {
		// All inputs are NULL, just return NULL
		auto &validity = FlatVector::ValidityMutable(result);
		validity.SetInvalid(0);
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		return;
	}
	D_ASSERT(result.GetType().id() == LogicalTypeId::MAP);
	auto count = args.size();

	vector<MapConcatInput> inputs;
	inputs.reserve(args.ColumnCount());
	for (auto &map : args.data) {
		if (map.GetType().id() != LogicalTypeId::SQLNULL) {
			inputs.emplace_back(map);
		}
	}
	vector<MapConcatSource> key_sources;
	vector<MapConcatSource> value_sources;
	absl::flat_hash_map<string_t, idx_t, StringHash, StringEquality> positions;
	auto result_data = FlatVector::Writer<list_entry_t>(result, count);
	for (idx_t row = 0; row < count; row++) {
		const auto row_start = key_sources.size();
		optional_idx first;
		bool all_null = true;
		bool indexed = false;
		for (idx_t i = 0; i < inputs.size(); i++) {
			auto entry = inputs[i].entries[row];
			if (!entry.IsValid()) {
				continue;
			}
			all_null = false;
			auto &list = entry.GetValue();
			if (list.length == 0) {
				continue;
			}
			if (!first.IsValid()) {
				first = i;
				for (idx_t k = 0; k < list.length; k++) {
					key_sources.push_back({i, list.offset + k});
					value_sources.push_back({i, list.offset + k});
				}
				continue;
			}
			if (!indexed) {
				indexed = true;
				auto &first_keys = inputs[first.GetIndex()].SortKeys();
				for (idx_t position = row_start; position < key_sources.size(); position++) {
					positions.emplace(first_keys[key_sources[position].index].GetValue(), position);
				}
			}
			auto &keys = inputs[i].SortKeys();
			for (idx_t k = 0; k < list.length; k++) {
				const auto index = list.offset + k;
				auto inserted = positions.emplace(keys[index].GetValue(), key_sources.size());
				if (inserted.second) {
					key_sources.push_back({i, index});
					value_sources.push_back({i, index});
				} else {
					value_sources[inserted.first->second] = {i, index};
				}
			}
		}
		if (indexed) {
			positions.clear();
		}
		if (all_null) {
			result_data.WriteNull();
			continue;
		}
		result_data.WriteValue(list_entry_t(row_start, key_sources.size() - row_start));
	}
	const auto total = key_sources.size();
	if (total == 0) {
		return;
	}
	ListVector::SetListSize(result, total);
	GatherMapComponent(MapVector::GetKeys(result), inputs, key_sources,
	                   [](const Vector &map) -> const Vector & { return MapVector::GetKeys(map); });
	GatherMapComponent(MapVector::GetValues(result), inputs, value_sources,
	                   [](const Vector &map) -> const Vector & { return MapVector::GetValues(map); });
	FlatVector::ValidityMutable(ListVector::GetChildMutable(result)).Resize(total);
}

bool IsEmptyMap(const LogicalType &map) {
	D_ASSERT(map.id() == LogicalTypeId::MAP);
	auto &key_type = MapType::KeyType(map);
	auto &value_type = MapType::ValueType(map);
	return key_type.id() == LogicalType::SQLNULL && value_type.id() == LogicalType::SQLNULL;
}

unique_ptr<FunctionData> MapConcatBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto arg_count = arguments.size();
	if (arg_count < 2) {
		throw InvalidInputException("The provided amount of arguments is incorrect, please provide 2 or more maps");
	}

	if (arguments[0]->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		// Prepared statement
		bound_function.GetArguments().emplace_back(LogicalTypeId::UNKNOWN);
		bound_function.SetReturnType(LogicalTypeId::SQLNULL);
		return nullptr;
	}

	LogicalType expected = LogicalType::SQLNULL;

	bool is_null = true;
	// Check and verify that all the maps are of the same type
	for (idx_t i = 0; i < arg_count; i++) {
		auto &arg = arguments[i];
		auto &map = arg->GetReturnType();
		if (map.id() == LogicalTypeId::UNKNOWN) {
			// Prepared statement
			bound_function.GetArguments().emplace_back(LogicalTypeId::UNKNOWN);
			bound_function.SetReturnType(LogicalTypeId::SQLNULL);
			return nullptr;
		}
		if (map.id() == LogicalTypeId::SQLNULL) {
			// The maps are allowed to be NULL
			continue;
		}
		if (map.id() != LogicalTypeId::MAP) {
			throw InvalidInputException("MAP_CONCAT only takes map arguments");
		}
		is_null = false;
		if (IsEmptyMap(map)) {
			// Map is allowed to be empty
			continue;
		}

		if (expected.id() == LogicalTypeId::SQLNULL) {
			expected = map;
		} else if (map != expected) {
			throw InvalidInputException(
			    "'value' type of map differs between arguments, expected '%s', found '%s' instead", expected.ToString(),
			    map.ToString());
		}
	}

	if (expected.id() == LogicalTypeId::SQLNULL && is_null == false) {
		expected = LogicalType::MAP(LogicalType::SQLNULL, LogicalType::SQLNULL);
	}
	bound_function.SetReturnType(expected);
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

} // namespace

ScalarFunction MapConcatFun::GetFunction() {
	//! the arguments and return types are actually set in the binder function
	ScalarFunction fun("map_concat", {}, LogicalTypeId::LIST, MapConcatFunction, MapConcatBind);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	return fun;
}

} // namespace duckdb
