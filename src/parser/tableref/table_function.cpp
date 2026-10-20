#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/sql_identifier.hpp"

namespace duckdb {

TableFunctionRef::TableFunctionRef() : TableRef(TableReferenceType::TABLE_FUNCTION) {
}

string TableFunctionRef::ToString() const {
	auto result = function->ToString();
	if (with_ordinality == OrdinalityType::WITH_ORDINALITY) {
		result += " WITH ORDINALITY";
	}
	if (column_types.empty()) {
		return BaseToString(result, column_name_alias);
	}
	result += alias.empty() ? " AS (" : " AS " + SQLIdentifier(alias) + "(";
	for (idx_t i = 0; i < column_types.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLIdentifier(column_name_alias[i]) + " " + column_types[i].ToString();
	}
	result += ")";
	return result + SampleToString();
}

bool TableFunctionRef::Equals(const TableRef &other_p) const {
	if (!TableRef::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<TableFunctionRef>();
	return bind_info == other.bind_info && function->Equals(*other.function) && column_types == other.column_types;
}

const unique_ptr<ParsedExpression> &TableFunctionRef::SerializableFunction() const {
	if (bind_info) {
		throw NotImplementedException("Cannot serialize a table function with process-local bind input");
	}
	return function;
}

unique_ptr<TableRef> TableFunctionRef::Copy() {
	auto copy = make_uniq<TableFunctionRef>();

	copy->function = function->Copy();
	copy->bind_info = bind_info;
	copy->column_name_alias = column_name_alias;
	copy->with_ordinality = with_ordinality;
	copy->column_types = column_types;
	CopyProperties(*copy);

	return std::move(copy);
}

} // namespace duckdb
