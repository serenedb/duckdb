#include "duckdb/parser/column_list.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

ColumnList::ColumnList(bool allow_duplicate_names, bool case_sensitive)
    : name_map(0, IdentifierHashFunction(case_sensitive), IdentifierEquality(case_sensitive)),
      allow_duplicate_names(allow_duplicate_names), case_sensitive(case_sensitive) {
}

ColumnList::ColumnList(vector<ColumnDefinition> columns, bool allow_duplicate_names, bool case_sensitive)
    : ColumnList(allow_duplicate_names, case_sensitive) {
	for (auto &col : columns) {
		AddColumn(std::move(col));
	}
}

void ColumnList::SetCaseSensitive(bool case_sensitive_p) {
	if (case_sensitive == case_sensitive_p) {
		return;
	}
	case_sensitive = case_sensitive_p;
	identifier_map_t<column_t> rekeyed(0, IdentifierHashFunction(case_sensitive), IdentifierEquality(case_sensitive));
	for (auto &entry : name_map) {
		if (!rekeyed.emplace(entry.first, entry.second).second && !allow_duplicate_names) {
			throw CatalogException("Column with name %s already exists!", entry.first);
		}
	}
	name_map = std::move(rekeyed);
}

void ColumnList::AddColumn(ColumnDefinition column) {
	auto oid = columns.size();
	if (column.Category() != TableColumnType::GENERATED_VIRTUAL) {
		column.SetStorageOid(physical_columns.size());
		physical_columns.push_back(oid);
	} else {
		column.SetStorageOid(DConstants::INVALID_INDEX);
	}
	column.SetOid(columns.size());
	AddToNameMap(column);
	columns.push_back(std::move(column));
}

void ColumnList::Finalize() {
	// add the "rowid" alias, if there is no rowid column specified in the table
	if (name_map.find("rowid") == name_map.end()) {
		name_map["rowid"] = COLUMN_IDENTIFIER_ROW_ID;
	}
}

void ColumnList::AddToNameMap(ColumnDefinition &col) {
	if (allow_duplicate_names) {
		idx_t index = 1;
		string base_name = col.Name().GetIdentifierName();
		while (name_map.find(col.Name()) != name_map.end()) {
			col.SetName(Identifier(base_name + "_" + to_string(index++)));
		}
	} else {
		if (name_map.find(col.Name()) != name_map.end()) {
			throw CatalogException("Column with name %s already exists!", col.Name());
		}
	}
	name_map[col.Name()] = col.Oid();
}

ColumnDefinition &ColumnList::GetColumnMutable(LogicalIndex logical) {
	if (logical.index >= columns.size()) {
		throw InternalException("Logical column index %lld out of range", logical.index);
	}
	return columns[logical.index];
}

ColumnDefinition &ColumnList::GetColumnMutable(PhysicalIndex physical) {
	if (physical.index >= physical_columns.size()) {
		throw InternalException("Physical column index %lld out of range", physical.index);
	}
	auto logical_index = physical_columns[physical.index];
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

ColumnDefinition &ColumnList::GetColumnMutable(const Identifier &name) {
	auto entry = name_map.find(name);
	if (entry == name_map.end()) {
		throw InternalException("Column with name %s does not exist", name);
	}
	auto logical_index = entry->second;
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

const ColumnDefinition &ColumnList::GetColumn(LogicalIndex logical) const {
	if (logical.index >= columns.size()) {
		throw InternalException("Logical column index %lld out of range", logical.index);
	}
	return columns[logical.index];
}

const ColumnDefinition &ColumnList::GetColumn(PhysicalIndex physical) const {
	if (physical.index >= physical_columns.size()) {
		throw InternalException("Physical column index %lld out of range", physical.index);
	}
	auto logical_index = physical_columns[physical.index];
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

const ColumnDefinition &ColumnList::GetColumn(const Identifier &name) const {
	auto entry = name_map.find(name);
	if (entry == name_map.end()) {
		throw InternalException("Column with name %s does not exist", name);
	}
	auto logical_index = entry->second;
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

vector<string> ColumnList::GetColumnNames() const {
	vector<string> names;
	names.reserve(columns.size());
	for (auto &column : columns) {
		names.emplace_back(column.Name());
	}
	return names;
}

vector<LogicalType> ColumnList::GetColumnTypes() const {
	vector<LogicalType> types;
	types.reserve(columns.size());
	for (auto &column : columns) {
		types.push_back(column.Type());
	}
	return types;
}

bool ColumnList::ColumnExists(const Identifier &name) const {
	auto entry = name_map.find(name);
	return entry != name_map.end();
}

unique_ptr<ParsedExpression> ColumnList::SharedGeneratedExpression(const ColumnDefinition &column) const {
	auto expression = column.GeneratedExpression().Copy();
	vector<idx_t> uses(columns.size(), 0);
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(*expression, [&](const ColumnRefExpression &colref) {
		auto &referenced = GetColumn(colref.GetColumnName());
		if (referenced.Generated()) {
			uses[referenced.Logical().index]++;
		}
	});
	vector<string> parameters;
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.emplace_back();
	identifier_map_t<Identifier> renamed;
	for (auto &referenced : Logical()) {
		if (uses[referenced.Logical().index] < 2) {
			continue;
		}
		Identifier parameter(SHARED_GENERATED_PREFIX + referenced.Name().GetIdentifierName());
		parameters.push_back(parameter.GetIdentifierName());
		renamed.emplace(referenced.Name(), std::move(parameter));
		arguments.push_back(make_uniq<ColumnRefExpression>(referenced.Name()));
	}
	if (parameters.empty()) {
		return expression;
	}
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    *expression, [&](ColumnRefExpression &colref) {
		    auto entry = renamed.find(colref.GetColumnName());
		    if (entry != renamed.end()) {
			    colref.ColumnNamesMutable() = vector<Identifier> {entry->second};
		    }
	    });
	arguments[0] = make_uniq<LambdaExpression>(std::move(parameters), std::move(expression));
	auto invoke = make_uniq<FunctionExpression>("invoke", std::move(arguments));
	invoke->BuiltinMutable() = true;
	return std::move(invoke);
}

PhysicalIndex ColumnList::LogicalToPhysical(LogicalIndex logical) const {
	auto &column = GetColumn(logical);
	if (column.Category() == TableColumnType::GENERATED_VIRTUAL) {
		throw InternalException("Column at position %d is not a physical column", logical.index);
	}
	return column.Physical();
}

LogicalIndex ColumnList::PhysicalToLogical(PhysicalIndex index) const {
	auto &column = GetColumn(index);
	return column.Logical();
}

LogicalIndex ColumnList::GetColumnIndex(Identifier &column_name) const {
	auto entry = name_map.find(column_name);
	if (entry == name_map.end()) {
		return LogicalIndex(DConstants::INVALID_INDEX);
	}
	if (entry->second == COLUMN_IDENTIFIER_ROW_ID) {
		column_name = "rowid";
		return LogicalIndex(COLUMN_IDENTIFIER_ROW_ID);
	}
	column_name = columns[entry->second].Name();
	return LogicalIndex(entry->second);
}

ColumnList ColumnList::Copy() const {
	ColumnList result(allow_duplicate_names, case_sensitive);
	for (auto &col : columns) {
		result.AddColumn(col.Copy());
	}
	return result;
}

ColumnList::ColumnListIterator ColumnList::Logical() const {
	return ColumnListIterator(*this, false);
}

ColumnList::ColumnListIterator ColumnList::Physical() const {
	return ColumnListIterator(*this, true);
}

} // namespace duckdb
