#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/common/sql_identifier.hpp"

#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

CreateIndexInfo::CreateIndexInfo() : CreateInfo(CatalogType::INDEX_ENTRY, Identifier::InvalidSchema()) {
}

CreateIndexInfo::CreateIndexInfo(const duckdb::CreateIndexInfo &info)
    : CreateInfo(CatalogType::INDEX_ENTRY), table(info.table), options(info.options), index_type(info.index_type),
      constraint_type(info.constraint_type), column_ids(info.column_ids), column_opclasses(info.column_opclasses),
      column_opclass_options(info.column_opclass_options), scan_types(info.scan_types), names(info.names) {
	SetQualifiedName(info.GetQualifiedName());
}

static void RemoveTableQualificationRecursive(unique_ptr<ParsedExpression> &root_expr, const Identifier &table_name) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    *root_expr, [&](ColumnRefExpression &col_ref) {
		    auto &col_names = col_ref.ColumnNamesMutable();
		    // the table qualifier is the component directly before the column name
		    if (col_ref.IsQualified() && col_names[col_names.size() - 2] == table_name) {
			    col_names.erase(col_names.begin());
		    }
	    });
}

vector<string> CreateIndexInfo::ExpressionsToList() const {
	vector<string> list;

	for (idx_t i = 0; i < parsed_expressions.size(); i++) {
		auto &expr = parsed_expressions[i];
		auto copy = expr->Copy();

		// Column reference expressions are qualified with the table name.
		// We need to remove them to reproduce the original query.
		RemoveTableQualificationRecursive(copy, table);
		bool add_parenthesis = true;
		if (copy->GetExpressionType() == ExpressionType::COLUMN_REF) {
			auto &column_ref = copy->Cast<ColumnRefExpression>();
			if (!column_ref.IsQualified()) {
				// Only not qualified references like (col1, col2) don't need parenthesis.
				add_parenthesis = false;
			}
		}

		string entry;
		if (add_parenthesis) {
			entry = StringUtil::Format("(%s)", copy->ToString());
		} else {
			entry = copy->ToString();
		}
		if (i < column_opclasses.size() && !column_opclasses[i].empty()) {
			entry += " " + column_opclasses[i];
		}
		list.push_back(std::move(entry));
	}
	return list;
}

string CreateIndexInfo::ExpressionsToString() const {
	auto list = ExpressionsToList();
	return StringUtil::Join(list, ", ");
}

vector<string> CreateIndexInfo::GetOpclassesForSerialization() const {
	for (auto &opclass : column_opclasses) {
		if (!opclass.empty()) {
			return column_opclasses;
		}
	}
	return {};
}

vector<std::optional<case_insensitive_map_t<Value>>> CreateIndexInfo::GetOpclassOptionsForSerialization() const {
	for (auto &opclass_options : column_opclass_options) {
		if (opclass_options) {
			return column_opclass_options;
		}
	}
	return {};
}

void CreateIndexInfo::FinalizeDeserialization() {
	if (column_opclasses.empty()) {
		column_opclasses.resize(parsed_expressions.size());
	}
	if (column_opclass_options.empty()) {
		column_opclass_options.resize(parsed_expressions.size());
	}
}

string CreateIndexInfo::ToString() const {
	string result;

	result += "CREATE";
	D_ASSERT(constraint_type == IndexConstraintType::UNIQUE || constraint_type == IndexConstraintType::NONE);
	if (constraint_type == IndexConstraintType::UNIQUE) {
		result += " UNIQUE";
	}
	result += " INDEX ";
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		result += "IF NOT EXISTS ";
	}
	result += SQLIdentifier(GetIndexName());
	result += " ON ";
	// the index lives in the same (possibly nested) schema as the table it is created on
	auto table_name = GetQualifiedName().WithName(table);
	if (temporary) {
		table_name.StripCatalog();
	}
	result += table_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (index_type != "ART") {
		result += " USING ";
		result += SQLIdentifier(index_type);
		result += " ";
	}
	result += "(";
	result += ExpressionsToString();
	result += ")";
	string rendered_options;
	for (auto &opt : options) {
		if (opt.second.type().id() == LogicalTypeId::BLOB) {
			continue;
		}
		if (!rendered_options.empty()) {
			rendered_options += ", ";
		}
		rendered_options += SQLIdentifier::ToString(opt.first);
		if (opt.second.IsNull()) {
			continue;
		}
		if (opt.second.type().id() == LogicalTypeId::VARCHAR) {
			rendered_options += " = " + opt.second.ToSQLString();
		} else {
			rendered_options += " = " + opt.second.ToString();
		}
	}
	if (!rendered_options.empty()) {
		result += " WITH (" + rendered_options + " )";
	}
	if (where_clause) {
		result += " WHERE ";
		result += where_clause->ToString();
	}
	result += ";";
	return result;
}

unique_ptr<CreateInfo> CreateIndexInfo::Copy() const {
	auto result = make_uniq<CreateIndexInfo>(*this);
	CopyProperties(*result);

	for (auto &expr : expressions) {
		result->expressions.push_back(expr->Copy());
	}
	for (auto &expr : parsed_expressions) {
		result->parsed_expressions.push_back(expr->Copy());
	}
	if (where_clause) {
		result->where_clause = where_clause->Copy();
	}
	return std::move(result);
}

} // namespace duckdb
