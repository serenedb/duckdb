#include "duckdb/common/type_visitor.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/planner/expression_binder.hpp"

#include <algorithm>

namespace duckdb {

struct ListSortBindData : public FunctionData {
	ListSortBindData(OrderType order_type_p, OrderByNullType null_order_p, bool is_grade_up,
	                 const LogicalType &return_type_p, const LogicalType &child_type_p, ClientContext &context_p,
	                 unique_ptr<Expression> sort_key_p = nullptr, optional<string> default_collation_p = {});
	~ListSortBindData() override;

	OrderType order_type;
	OrderByNullType null_order;
	LogicalType return_type;
	LogicalType child_type;
	bool is_grade_up;

	vector<LogicalType> types;

	ClientContext &context;
	unique_ptr<Expression> sort_key;
	optional<string> default_collation;

public:
	bool Equals(const FunctionData &other_p) const override;
	unique_ptr<FunctionData> Copy() const override;
	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                      const BoundScalarFunction &);
	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundScalarFunction &);
};

ListSortBindData::ListSortBindData(OrderType order_type_p, OrderByNullType null_order_p, bool is_grade_up_p,
                                   const LogicalType &return_type_p, const LogicalType &child_type_p,
                                   ClientContext &context_p, unique_ptr<Expression> sort_key_p,
                                   optional<string> default_collation_p)
    : order_type(order_type_p), null_order(null_order_p), return_type(return_type_p), child_type(child_type_p),
      is_grade_up(is_grade_up_p), context(context_p), default_collation(std::move(default_collation_p)) {
	// get the vector types
	types.emplace_back(LogicalType::USMALLINT);
	types.emplace_back(child_type);
	types.emplace_back(LogicalType::UINTEGER);
	D_ASSERT(types.size() == 3);

	if (!sort_key_p) {
		default_collation = Settings::Get<DefaultCollationSetting>(context);
		sort_key_p = make_uniq_base<Expression, BoundReferenceExpression>(child_type, 1U);
		// Normalize the sort key without changing the sorted values (#25108): push the
		// type's collation (for INTERVAL this wraps the expression in
		// normalized_interval(...), the same normalization comparison operators, ORDER BY
		// and aggregates apply) onto the key expression BEFORE constructing its
		// BoundOrderByNode. The key then byte-compares like ORDER BY, while the list
		// payload keeps the original (non-normalized) values.
		ExpressionBinder::PushCollation(context, sort_key_p, child_type);
	}
	sort_key = std::move(sort_key_p);
}

unique_ptr<FunctionData> ListSortBindData::Copy() const {
	return make_uniq<ListSortBindData>(order_type, null_order, is_grade_up, return_type, child_type, context,
	                                   sort_key->Copy(), default_collation);
}

bool ListSortBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<ListSortBindData>();
	return order_type == other.order_type && null_order == other.null_order && return_type == other.return_type &&
	       child_type == other.child_type && is_grade_up == other.is_grade_up &&
	       Expression::Equals(sort_key, other.sort_key) && default_collation == other.default_collation;
}

void ListSortBindData::Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                 const BoundScalarFunction &) {
	auto &data = bind_data->Cast<ListSortBindData>();
	serializer.WriteProperty(100, "order_type", data.order_type);
	serializer.WriteProperty(101, "null_order", data.null_order);
	serializer.WriteProperty(102, "return_type", data.return_type);
	serializer.WriteProperty(103, "child_type", data.child_type);
	serializer.WriteProperty(104, "is_grade_up", data.is_grade_up);
	serializer.WriteProperty(105, "sort_key", data.sort_key);
	serializer.WritePropertyWithDefault(106, "default_collation", data.default_collation);
}

unique_ptr<FunctionData> ListSortBindData::Deserialize(Deserializer &deserializer, BoundScalarFunction &) {
	auto order_type = deserializer.ReadProperty<OrderType>(100, "order_type");
	auto null_order = deserializer.ReadProperty<OrderByNullType>(101, "null_order");
	auto return_type = deserializer.ReadProperty<LogicalType>(102, "return_type");
	auto child_type = deserializer.ReadProperty<LogicalType>(103, "child_type");
	auto is_grade_up = deserializer.ReadProperty<bool>(104, "is_grade_up");
	auto sort_key = deserializer.ReadProperty<unique_ptr<Expression>>(105, "sort_key");
	auto default_collation = deserializer.ReadPropertyWithDefault<optional<string>>(106, "default_collation");
	auto &context = deserializer.Get<ClientContext &>();
	return make_uniq<ListSortBindData>(order_type, null_order, is_grade_up, return_type, child_type, context,
	                                   std::move(sort_key), std::move(default_collation));
}

ListSortBindData::~ListSortBindData() {
}

struct ListSortEntry {
	static constexpr idx_t PREFIX_SIZE = 2 * sizeof(uint64_t);

	ListSortEntry() = default;
	ListSortEntry(const string_t &key, sel_t index_p)
	    : length(UnsafeNumericCast<uint32_t>(key.GetSize())), index(index_p) {
		data_t prefix[PREFIX_SIZE] = {};
		memcpy(prefix, key.GetData(), MinValue<idx_t>(length, PREFIX_SIZE));
		high = BSwapIfLE(Load<uint64_t>(prefix));
		low = BSwapIfLE(Load<uint64_t>(prefix + sizeof(uint64_t)));
	}

	uint64_t high;
	uint64_t low;
	uint32_t length;
	sel_t index;
};

static bool ListSortLess(const ListSortEntry &l, const ListSortEntry &r, const vector<string_t> &keys) {
	if (l.high != r.high) {
		return l.high < r.high;
	}
	if (l.low != r.low) {
		return l.low < r.low;
	}
	if (l.length > ListSortEntry::PREFIX_SIZE && r.length > ListSortEntry::PREFIX_SIZE) {
		const auto rest = MinValue(l.length, r.length) - ListSortEntry::PREFIX_SIZE;
		const auto cmp = memcmp(keys[l.index].GetData() + ListSortEntry::PREFIX_SIZE,
		                        keys[r.index].GetData() + ListSortEntry::PREFIX_SIZE, rest);
		if (cmp != 0) {
			return cmp < 0;
		}
	}
	if (l.length != r.length) {
		return l.length < r.length;
	}
	return l.index < r.index;
}

struct ListSortLocalState : public FunctionLocalState {
	ListSortLocalState(ClientContext &context, const ListSortBindData &info)
	    : key_is_element(info.sort_key->GetExpressionClass() == ExpressionClass::BOUND_REF), key_executor(context) {
		if (!key_is_element) {
			key_executor.AddExpression(*info.sort_key);
			key_input.InitializeEmpty(info.types);
		}
	}

	bool key_is_element;
	ExpressionExecutor key_executor;
	DataChunk key_input;
	vector<sel_t> positions;
	vector<string_t> keys;
	vector<Vector> key_vectors;
	vector<ListSortEntry> entries;
	vector<sel_t> order;
};

static unique_ptr<FunctionLocalState>
ListSortInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	return make_uniq<ListSortLocalState>(state.GetContext(), bind_data->Cast<ListSortBindData>());
}

static void CreateListSortKeys(ListSortLocalState &lstate, const ListSortBindData &info, const Vector &child) {
	const OrderModifiers modifiers(info.order_type, info.null_order);
	const auto total = lstate.positions.size();
	lstate.keys.resize(total);
	lstate.key_vectors.clear();
	for (idx_t start = 0; start < total; start += STANDARD_VECTOR_SIZE) {
		const auto batch = MinValue<idx_t>(STANDARD_VECTOR_SIZE, total - start);
		const SelectionVector batch_sel(lstate.positions.data() + start, batch);
		Vector elements(child, batch_sel, batch);
		auto &batch_keys = lstate.key_vectors.emplace_back(LogicalType::BLOB, batch);
		if (lstate.key_is_element) {
			CreateSortKeyHelpers::CreateSortKey(elements, batch, modifiers, batch_keys);
		} else {
			auto &key_input = lstate.key_input;
			key_input.Reset();
			key_input.data[0].Reference(Value(info.types[0]), count_t(batch));
			key_input.data[1].Reference(elements);
			key_input.data[2].Reference(Value(info.types[2]), count_t(batch));
			key_input.SetCardinality(batch);
			Vector key_values(info.sort_key->GetReturnType(), batch);
			lstate.key_executor.ExecuteExpression(key_input, key_values);
			CreateSortKeyHelpers::CreateSortKey(key_values, batch, modifiers, batch_keys);
		}
		const auto batch_values = batch_keys.Values<string_t>();
		for (idx_t i = 0; i < batch; i++) {
			lstate.keys[start + i] = batch_values[i].GetValue();
		}
	}
}

static void ListSortFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() >= 1 && args.ColumnCount() <= 3);
	auto count = args.size();
	const Vector &input_lists = args.data[0];

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::ValidityMutable(result);

	if (input_lists.GetType().id() == LogicalTypeId::SQLNULL) {
		result_validity.SetInvalid(0);
		return;
	}

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ListSortBindData>();
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<ListSortLocalState>();
	auto &child = ListVector::GetChild(input_lists);
	auto list_entries = input_lists.Values<list_entry_t>();

	auto &positions = lstate.positions;
	positions.clear();
	for (idx_t i = 0; i < count; i++) {
		auto entry = list_entries[i];
		if (!entry.IsValid()) {
			continue;
		}
		const auto &list = entry.GetValue();
		for (idx_t k = 0; k < list.length; k++) {
			positions.push_back(UnsafeNumericCast<sel_t>(list.offset + k));
		}
	}
	const auto total = positions.size();
	CreateListSortKeys(lstate, info, child);

	const auto &keys = lstate.keys;
	auto &entries = lstate.entries;
	entries.resize(total);
	for (idx_t k = 0; k < total; k++) {
		entries[k] = ListSortEntry(keys[k], UnsafeNumericCast<sel_t>(k));
	}
	idx_t list_start = 0;
	for (idx_t i = 0; i < count; i++) {
		auto entry = list_entries[i];
		if (!entry.IsValid()) {
			continue;
		}
		const auto length = entry.GetValue().length;
		const auto begin = entries.begin() + NumericCast<int64_t>(list_start);
		std::sort(begin, begin + NumericCast<int64_t>(length),
		          [&](const ListSortEntry &l, const ListSortEntry &r) { return ListSortLess(l, r, keys); });
		list_start += length;
	}
	auto &order = lstate.order;
	order.resize(total);
	for (idx_t k = 0; k < total; k++) {
		order[k] = entries[k].index;
	}

	const auto result_offset = ListVector::GetListSize(result);
	if (info.is_grade_up) {
		ListVector::Reserve(result, result_offset + total);
		auto &result_child = ListVector::GetChildMutable(result);
		auto grades = FlatVector::Writer<int64_t>(result_child, total, result_offset);
		auto result_lists = FlatVector::Writer<list_entry_t>(result, count);
		list_start = 0;
		for (idx_t i = 0; i < count; i++) {
			auto entry = list_entries[i];
			if (!entry.IsValid()) {
				result_lists.WriteNull();
				continue;
			}
			const auto length = entry.GetValue().length;
			for (idx_t k = 0; k < length; k++) {
				grades.WriteValue(UnsafeNumericCast<int64_t>(order[list_start + k] - list_start + 1));
			}
			result_lists.WriteValue(list_entry_t(result_offset + list_start, length));
			list_start += length;
		}
		return;
	}

	for (auto &position : order) {
		position = positions[position];
	}
	ListVector::Append(result, child, SelectionVector(order.data(), total), total);
	auto result_lists = FlatVector::Writer<list_entry_t>(result, count);
	list_start = 0;
	for (idx_t i = 0; i < count; i++) {
		auto entry = list_entries[i];
		if (!entry.IsValid()) {
			result_lists.WriteNull();
			continue;
		}
		const auto length = entry.GetValue().length;
		result_lists.WriteValue(list_entry_t(result_offset + list_start, length));
		list_start += length;
	}
}

static unique_ptr<FunctionData> ListSortBind(ClientContext &context, BoundScalarFunction &bound_function,
                                             vector<unique_ptr<Expression>> &arguments, OrderType &order,
                                             OrderByNullType &null_order) {
	LogicalType child_type;
	if (arguments[0]->GetReturnType() == LogicalTypeId::UNKNOWN) {
		bound_function.GetArguments()[0] = LogicalTypeId::UNKNOWN;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		child_type = bound_function.GetReturnType();
		return make_uniq<ListSortBindData>(order, null_order, false, bound_function.GetReturnType(), child_type,
		                                   context);
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::LIST) {
		throw BinderException("list_sort can only operate on LISTs");
	}
	child_type = ListType::GetChildType(arguments[0]->GetReturnType());

	bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
	bound_function.SetReturnType(arguments[0]->GetReturnType());

	return make_uniq<ListSortBindData>(order, null_order, false, bound_function.GetReturnType(), child_type, context);
}

template <class T>
static T GetOrder(const Value &order_value) {
	auto order_name = StringUtil::Upper(order_value.ToString());
	return EnumUtil::FromString<T>(order_name);
}

static unique_ptr<FunctionData> ListGradeUpBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(!arguments.empty() && arguments.size() <= 3);
	auto order = OrderType::ORDER_DEFAULT;
	auto null_order = OrderByNullType::ORDER_DEFAULT;

	// get the sorting order
	if (arguments.size() >= 2) {
		order = GetOrder<OrderType>(input.GetConstant(1));
	}
	// get the null sorting order
	if (arguments.size() == 3) {
		null_order = GetOrder<OrderByNullType>(input.GetConstant(2));
	}
	auto &config = DBConfig::GetConfig(context);
	order = config.ResolveOrder(context, order);
	null_order = config.ResolveNullOrder(context, order, null_order);

	if (arguments[0]->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		bound_function.GetArguments()[0] = LogicalTypeId::UNKNOWN;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		return make_uniq<ListSortBindData>(order, null_order, true, bound_function.GetReturnType(),
		                                   bound_function.GetReturnType(), context);
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::LIST) {
		throw BinderException("list_grade_up can only operate on LISTs");
	}

	bound_function.GetArguments()[0] = arguments[0]->GetReturnType();
	bound_function.SetReturnType(LogicalType::LIST(LogicalTypeId::BIGINT));
	auto child_type = ListType::GetChildType(arguments[0]->GetReturnType());
	return make_uniq<ListSortBindData>(order, null_order, true, bound_function.GetReturnType(), child_type, context);
}

static unique_ptr<FunctionData> ListNormalSortBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(!arguments.empty() && arguments.size() <= 3);
	auto order = OrderType::ORDER_DEFAULT;
	auto null_order = OrderByNullType::ORDER_DEFAULT;

	// get the sorting order
	if (arguments.size() >= 2) {
		order = GetOrder<OrderType>(input.GetConstant(1));
	}
	// get the null sorting order
	if (arguments.size() == 3) {
		null_order = GetOrder<OrderByNullType>(input.GetConstant(2));
	}
	auto &config = DBConfig::GetConfig(context);
	order = config.ResolveOrder(context, order);
	null_order = config.ResolveNullOrder(context, order, null_order);
	return ListSortBind(context, bound_function, arguments, order, null_order);
}

static unique_ptr<FunctionData> ListReverseSortBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto order = OrderType::ORDER_DEFAULT;
	auto null_order = OrderByNullType::ORDER_DEFAULT;

	if (arguments.size() == 2) {
		null_order = GetOrder<OrderByNullType>(input.GetConstant(1));
	}
	auto &config = DBConfig::GetConfig(context);
	order = config.ResolveOrder(context, order);
	switch (order) {
	case OrderType::ASCENDING:
		order = OrderType::DESCENDING;
		break;
	case OrderType::DESCENDING:
		order = OrderType::ASCENDING;
		break;
	default:
		throw InternalException("Unexpected order type in list reverse sort");
	}
	null_order = config.ResolveNullOrder(context, order, null_order);
	return ListSortBind(context, bound_function, arguments, order, null_order);
}

static unique_ptr<ParsedExpression> ListSortUnbind(FunctionUnbindInput &input) {
	if (input.children.empty() || !input.expression.BindInfo()) {
		return nullptr;
	}
	auto &data = input.expression.BindInfo()->Cast<ListSortBindData>();
	if (!data.default_collation ||
	    TypeVisitor::Contains(data.child_type, [](const LogicalType &type) { return type.HasAlias(); })) {
		return nullptr;
	}
	auto type = TypeVisitor::VisitReplace(data.child_type, [&](const LogicalType &child) {
		if (child.id() == LogicalTypeId::VARCHAR && StringType::GetCollation(child).empty()) {
			return LogicalType::VARCHAR_COLLATION(data.default_collation->empty() ? "binary" : *data.default_collation);
		}
		return child;
	});
	auto witness = BoundExpressionSQLExporter::Export(BoundConstantExpression(Value(LogicalType::LIST(type))), {});
	if (witness.HasError()) {
		return nullptr;
	}
	vector<unique_ptr<ParsedExpression>> cast_children;
	cast_children.push_back(std::move(input.children[0]));
	cast_children.push_back(std::move(witness.GetValue()));
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(
	    make_uniq<FunctionExpression>(QualifiedName("system", "main", "cast_to_type"), std::move(cast_children)));
	children.push_back(ConstantExpression::String(data.order_type == OrderType::ASCENDING ? "ASC" : "DESC"));
	children.push_back(
	    ConstantExpression::String(data.null_order == OrderByNullType::NULLS_FIRST ? "NULLS FIRST" : "NULLS LAST"));
	auto result_type = BoundExpressionSQLExporter::Export(BoundConstantExpression(Value(data.return_type)), {});
	if (result_type.HasError()) {
		return nullptr;
	}
	vector<unique_ptr<ParsedExpression>> result;
	result.push_back(make_uniq<FunctionExpression>(
	    QualifiedName("system", "main", data.is_grade_up ? "list_grade_up" : "list_sort"), std::move(children)));
	result.push_back(std::move(result_type.GetValue()));
	return make_uniq<FunctionExpression>(QualifiedName("system", "main", "cast_to_type"), std::move(result));
}

static void SetListSortCallbacks(ScalarFunction &function) {
	function.SetInitStateCallback(ListSortInitLocalState);
	function.SetUnbindCallback(ListSortUnbind);
	function.SetSerializeCallback(ListSortBindData::Serialize);
	function.SetDeserializeCallback(ListSortBindData::Deserialize);
}

ScalarFunctionSet ListSortFun::GetFunctions() {
	// one parameter: list
	ScalarFunction sort({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListNormalSortBind);
	sort.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));

	// two parameters: list, order
	ScalarFunction sort_order({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListNormalSortBind);
	sort_order.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR);

	// three parameters: list, order, null order
	ScalarFunction sort_orders({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListNormalSortBind);
	sort_orders.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR)
	    .AddParameter("null_order", LogicalType::VARCHAR);
	SetListSortCallbacks(sort);
	SetListSortCallbacks(sort_order);
	SetListSortCallbacks(sort_orders);

	ScalarFunctionSet list_sort;
	list_sort.AddFunction(sort);
	list_sort.AddFunction(sort_order);
	list_sort.AddFunction(sort_orders);
	return list_sort;
}

ScalarFunctionSet ListGradeUpFun::GetFunctions() {
	// one parameter: list
	ScalarFunction sort({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListGradeUpBind);
	sort.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));

	// two parameters: list, order
	ScalarFunction sort_order({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListGradeUpBind);
	sort_order.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR);

	// three parameters: list, order, null order
	ScalarFunction sort_orders({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListGradeUpBind);
	sort_orders.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("sort_order", LogicalType::VARCHAR)
	    .AddParameter("null_order", LogicalType::VARCHAR);
	SetListSortCallbacks(sort);
	SetListSortCallbacks(sort_order);
	SetListSortCallbacks(sort_orders);

	ScalarFunctionSet list_grade_up;
	list_grade_up.AddFunction(sort);
	list_grade_up.AddFunction(sort_order);
	list_grade_up.AddFunction(sort_orders);
	return list_grade_up;
}

ScalarFunctionSet ListReverseSortFun::GetFunctions() {
	// one parameter: list
	ScalarFunction sort_reverse({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction, ListReverseSortBind);
	sort_reverse.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));

	// two parameters: list, null order
	ScalarFunction sort_reverse_null_order({}, LogicalType::LIST(LogicalType::ANY), ListSortFunction,
	                                       ListReverseSortBind);
	sort_reverse_null_order.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("null_order", LogicalType::VARCHAR);
	SetListSortCallbacks(sort_reverse);
	SetListSortCallbacks(sort_reverse_null_order);

	ScalarFunctionSet list_reverse_sort;
	list_reverse_sort.AddFunction(sort_reverse);
	list_reverse_sort.AddFunction(sort_reverse_null_order);
	return list_reverse_sort;
}

} // namespace duckdb
