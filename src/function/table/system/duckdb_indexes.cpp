#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/client_data.hpp"

namespace duckdb {

Value GetIndexExpressions(IndexCatalogEntry &index) {
	auto create_info = index.GetInfo();
	auto &create_index_info = create_info->Cast<CreateIndexInfo>();

	auto vec = create_index_info.ExpressionsToList();

	vector<Value> content;
	content.reserve(vec.size());
	for (auto &item : vec) {
		content.push_back(Value(item));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(content));
}

struct ListedIndex {
	ListedIndex(ClientContext &context, IndexCatalogEntry &index)
	    : index(index), schema_name(index.ParentSchemaName(CatalogTransaction(index.ParentCatalog(), context))),
	      expressions(GetIndexExpressions(index).ToString()) {
		auto table_entry = index.GetRelation(index.catalog.GetCatalogTransaction(context));
		table_name = Value(table_entry ? table_entry->name : index.GetTableName());
		if (table_entry && table_entry->type == CatalogType::TABLE_ENTRY) {
			table_oid = Value::BIGINT(NumericCast<int64_t>(table_entry->oid));
		}
		auto index_sql = index.ToSQL();
		if (!index_sql.empty()) {
			sql = Value(std::move(index_sql));
		}
	}

	IndexCatalogEntry &index;
	Value schema_name;
	Value table_name;
	Value table_oid;
	Value expressions;
	Value sql;
};

struct DuckDBIndexesData : public GlobalTableFunctionState {
	DuckDBIndexesData() : offset(0) {
	}

	vector<ListedIndex> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBIndexesBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("index_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("index_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("table_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("table_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("is_unique");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("is_primary");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("expressions");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);

	auto result = make_uniq<DuckDBSystemIncludeHiddenBindData>();
	result->include_hidden = DuckDBSystemIncludeHiddenBindData::ReadParameter(input);
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> DuckDBIndexesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBIndexesData>();
	auto &bind_data = input.bind_data->Cast<DuckDBSystemIncludeHiddenBindData>();

	// scan all the schemas for tables and collect them
	auto schemas = Catalog::GetAllSchemas(context, bind_data.include_hidden, bind_data.DatabaseFilter(context));
	for (auto &schema : schemas) {
		Catalog::ScanListedEntries(context, schema.get(), CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
			result->entries.emplace_back(context, entry.Cast<IndexCatalogEntry>());
		});
	};
	return std::move(result);
}

void DuckDBIndexesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBIndexesData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// database_name, VARCHAR
	auto &database_name = output.data[0];
	// database_oid, BIGINT
	auto &database_oid = output.data[1];
	// schema_name, VARCHAR
	auto &schema_name = output.data[2];
	// schema_oid, BIGINT
	auto &schema_oid = output.data[3];
	// index_name, VARCHAR
	auto &index_name = output.data[4];
	// index_oid, BIGINT
	auto &index_oid = output.data[5];
	// table_name, VARCHAR
	auto &table_name = output.data[6];
	// table_oid, BIGINT
	auto &table_oid = output.data[7];
	// comment, VARCHAR
	auto &comment = output.data[8];
	// tags, MAP
	auto &tags = output.data[9];
	// is_unique, BOOLEAN
	auto &is_unique = output.data[10];
	// is_primary, BOOLEAN
	auto &is_primary = output.data[11];
	// expressions, VARCHAR
	auto &expressions = output.data[12];
	// sql, VARCHAR
	auto &sql_vec = output.data[13];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		auto &index = entry.index;

		database_name.Append(Value(index.catalog.GetName()));
		database_oid.Append(Value::BIGINT(NumericCast<int64_t>(index.catalog.GetOid())));
		schema_name.Append(entry.schema_name);
		schema_oid.Append(Value::BIGINT(NumericCast<int64_t>(index.ParentSchemaOid())));
		index_name.Append(Value(index.name));
		index_oid.Append(Value::BIGINT(NumericCast<int64_t>(index.oid)));
		table_name.Append(entry.table_name);
		table_oid.Append(entry.table_oid);
		comment.Append(Value(index.comment));
		tags.Append(Value::MAP(index.tags));
		is_unique.Append(Value::BOOLEAN(index.IsUnique()));
		is_primary.Append(Value::BOOLEAN(index.IsPrimary()));
		expressions.Append(entry.expressions);
		sql_vec.Append(entry.sql);

		count++;
	}
}

void DuckDBIndexesFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction fn("duckdb_indexes", {}, DuckDBIndexesFunction, DuckDBIndexesBind, DuckDBIndexesInit);
	fn.pushdown_complex_filter = DuckDBSystemIncludeHiddenBindData::PushdownDatabaseFilters;
	fn.GetSignature().AddKeywordOnly("include_hidden", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	set.AddFunction(fn);
}

} // namespace duckdb
