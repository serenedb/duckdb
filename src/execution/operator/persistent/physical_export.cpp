#include "duckdb/execution/operator/persistent/physical_export.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"

#include <algorithm>
#include <sstream>

namespace duckdb {

void ReorderTableEntries(catalog_entry_vector_t &tables);

using duckdb::stringstream;

PhysicalExport::PhysicalExport(PhysicalPlan &physical_plan, vector<LogicalType> types, CopyFunction function,
                               unique_ptr<CopyInfo> info, idx_t estimated_cardinality,
                               unique_ptr<BoundExportData> exported_tables)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXPORT, std::move(types), estimated_cardinality),
      function(std::move(function)), info(std::move(info)), exported_tables(std::move(exported_tables)) {
}

static string CommentTarget(CatalogEntry &entry) {
	switch (entry.type) {
	case CatalogType::TABLE_ENTRY:
		return "TABLE";
	case CatalogType::VIEW_ENTRY:
		return "VIEW";
	case CatalogType::SEQUENCE_ENTRY:
		return "SEQUENCE";
	case CatalogType::INDEX_ENTRY:
		return "INDEX";
	case CatalogType::TYPE_ENTRY:
		return "TYPE";
	case CatalogType::MACRO_ENTRY:
		return "MACRO";
	case CatalogType::TABLE_MACRO_ENTRY:
		return "MACRO TABLE";
	default:
		return string();
	}
}

static void WriteComments(stringstream &ss, CatalogEntry &entry) {
	auto target = CommentTarget(entry);
	if (target.empty()) {
		return;
	}
	auto name = QualifiedName(entry.ParentSchemaPath(), entry.name).ToString();
	if (!entry.comment.IsNull()) {
		ss << "COMMENT ON " << target << " " << name << " IS " << entry.comment.ToSQLString() << ";\n";
	}
	if (entry.type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &column : entry.Cast<TableCatalogEntry>().GetColumns().Logical()) {
		if (!column.Comment().IsNull()) {
			ss << "COMMENT ON COLUMN " << name << "." << SQLIdentifier(column.Name()) << " IS "
			   << column.Comment().ToSQLString() << ";\n";
		}
	}
}

static void WriteCatalogEntries(stringstream &ss, catalog_entry_vector_t &entries,
                                const reference_set_t<CatalogEntry> &skip) {
	for (auto &entry : entries) {
		if (entry.get().internal || skip.count(entry.get())) {
			continue;
		}
		auto create_info = entry.get().GetInfo();
		try {
			// the catalog is implied by the database the export is imported into - keep only the schema path
			create_info->StripCatalogQualification();
			auto to_string = create_info->ToString();
			ss << to_string;
		} catch (const NotImplementedException &) {
			ss << entry.get().ToSQL();
		}
		ss << '\n';
		WriteComments(ss, entry.get());
	}
	ss << '\n';
}

static void WriteStringStreamToFile(FileSystem &fs, stringstream &ss, const string &path) {
	auto ss_string = ss.str();
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                                    FileLockType::WRITE_LOCK);
	fs.Write(*handle, (void *)ss_string.c_str(), NumericCast<int64_t>(ss_string.size()));
	handle.reset();
}

static void WriteCopyStatement(FileSystem &fs, stringstream &ss, CopyInfo &info, ExportedTableData &exported_table,
                               TableCatalogEntry &table) {
	ss << "COPY ";

	//! NOTE: The catalog is explicitly not set here
	auto table_name = exported_table.qualified_name;
	table_name.StripCatalog();
	ss << table_name.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	vector<string> columns;
	bool has_generated = false;
	for (auto &column : table.GetColumns().Physical()) {
		if (column.Generated()) {
			has_generated = true;
		} else {
			columns.push_back(KeywordHelper::WriteOptionallyQuoted(column.Name().GetIdentifierName()));
		}
	}
	if (has_generated) {
		ss << " (" << StringUtil::Join(columns, ", ") << ")";
	}
	auto file_path = StringUtil::Replace(exported_table.file_path, "\\", "/");
	ss << StringUtil::Format(" FROM %s (", SQLString(file_path));
	// write the copy options
	ss << "FORMAT '" << info.format << "'";
	if (info.format == "csv") {
		info.options.erase("force_not_null");
		for (auto &not_null_column : exported_table.not_null_columns) {
			info.options["force_not_null"].push_back(not_null_column);
		}
	}
	for (auto &copy_option : info.options) {
		if (copy_option.second.empty()) {
			// empty options are interpreted as TRUE
			copy_option.second.push_back(true);
		}
		ss << ", " << copy_option.first << " ";
		if (copy_option.second.size() == 1) {
			ss << copy_option.second[0].ToSQLString();
		} else {
			// For Lists
			ss << "(";
			for (idx_t i = 0; i < copy_option.second.size(); i++) {
				ss << copy_option.second[i].ToSQLString();
				if (i != copy_option.second.size() - 1) {
					ss << ", ";
				}
			}
			ss << ")";
		}
	}
	ss << ");" << '\n';
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class ExportSourceState : public GlobalSourceState {
public:
	ExportSourceState() : finished(false) {
	}

	bool finished;
};

unique_ptr<GlobalSourceState> PhysicalExport::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<ExportSourceState>();
}

void PhysicalExport::ExtractEntries(ClientContext &context, vector<reference<SchemaCatalogEntry>> &schema_list,
                                    ExportEntries &result) {
	for (auto &schema_p : schema_list) {
		auto &schema = schema_p.get();
		auto &catalog = schema.ParentCatalog();
		if (catalog.IsSystemCatalog() || catalog.IsTemporaryCatalog()) {
			continue;
		}
		if (!schema.internal) {
			result.schemas.push_back(schema);
		}
		schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			if (entry.type != CatalogType::TABLE_ENTRY) {
				result.views.push_back(entry);
			}
			if (entry.type == CatalogType::TABLE_ENTRY) {
				result.tables.push_back(entry);
			}
		});
		schema.Scan(context, CatalogType::SEQUENCE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			result.sequences.push_back(entry);
		});
		schema.Scan(context, CatalogType::TYPE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			result.custom_types.push_back(entry);
		});
		schema.Scan(context, CatalogType::TOKENIZER_ENTRY, [&](CatalogEntry &entry) {
			if (!entry.internal) {
				throw NotImplementedException("EXPORT DATABASE does not support text search dictionaries yet: \"%s\"",
				                              entry.name.GetIdentifierName());
			}
		});
		schema.Scan(context, CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
			if (entry.internal) {
				return;
			}
			result.indexes.push_back(entry);
		});
		schema.Scan(context, CatalogType::MACRO_ENTRY, [&](CatalogEntry &entry) {
			if (!entry.internal && entry.type == CatalogType::MACRO_ENTRY) {
				result.macros.push_back(entry);
			}
		});
		schema.Scan(context, CatalogType::TABLE_MACRO_ENTRY, [&](CatalogEntry &entry) {
			if (!entry.internal && entry.type == CatalogType::TABLE_MACRO_ENTRY) {
				result.macros.push_back(entry);
			}
		});
	}
}

static reference_set_t<CatalogEntry> CreatedByTarget(ClientContext &context, catalog_entry_vector_t &entries) {
	reference_set_t<CatalogEntry> result;
	for (auto &entry : entries) {
		if (entry.get().type == CatalogType::SCHEMA_ENTRY &&
		    entry.get().name == entry.get().ParentCatalog().GetDefaultSchema()) {
			result.insert(entry.get());
			continue;
		}
		if (entry.get().type != CatalogType::TABLE_ENTRY) {
			continue;
		}
		auto &table = entry.get().Cast<TableCatalogEntry>();
		auto dependencies = table.ParentCatalog().GetDependencyManager();
		if (!dependencies) {
			continue;
		}
		for (auto &owned : dependencies->OwnedEntries(table.ParentCatalog().GetCatalogTransaction(context), table)) {
			if (table.NumbersRowsWith(owned.get())) {
				result.insert(owned.get());
			}
		}
	}
	return result;
}

static void AddEntries(catalog_entry_vector_t &all_entries, catalog_entry_vector_t &to_add) {
	for (auto &entry : to_add) {
		all_entries.push_back(entry);
	}
	to_add.clear();
}

catalog_entry_vector_t PhysicalExport::GetNaiveExportOrder(ClientContext &context, Catalog &catalog) {
	// gather all catalog types to export
	ExportEntries entries;
	auto schema_list = catalog.GetSchemas(context);
	PhysicalExport::ExtractEntries(context, schema_list, entries);

	ReorderTableEntries(entries.tables);

	// order macro's by timestamp so nested macro's are imported nicely
	sort(entries.macros.begin(), entries.macros.end(),
	     [](const reference<CatalogEntry> &lhs, const reference<CatalogEntry> &rhs) {
		     return lhs.get().oid < rhs.get().oid;
	     });

	catalog_entry_vector_t catalog_entries;
	idx_t size = 0;
	size += entries.schemas.size();
	size += entries.custom_types.size();
	size += entries.sequences.size();
	size += entries.tables.size();
	size += entries.views.size();
	size += entries.indexes.size();
	size += entries.macros.size();
	catalog_entries.reserve(size);
	AddEntries(catalog_entries, entries.schemas);
	AddEntries(catalog_entries, entries.sequences);
	AddEntries(catalog_entries, entries.custom_types);
	AddEntries(catalog_entries, entries.tables);
	AddEntries(catalog_entries, entries.macros);
	AddEntries(catalog_entries, entries.views);
	AddEntries(catalog_entries, entries.indexes);
	return catalog_entries;
}

SourceResultType PhysicalExport::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	auto &state = input.global_state.Cast<ExportSourceState>();
	if (state.finished) {
		return SourceResultType::FINISHED;
	}

	auto &ccontext = context.client;
	auto &fs = FileSystem::GetFileSystem(ccontext);

	auto &catalog = Catalog::GetCatalog(ccontext, info->GetQualifiedName().Catalog());

	catalog_entry_vector_t catalog_entries;
	catalog_entries = GetNaiveExportOrder(context.client, catalog);
	auto dependency_manager = catalog.GetDependencyManager();
	if (dependency_manager) {
		dependency_manager->ReorderEntries(catalog_entries, ccontext);
	}

	// write the schema.sql file
	stringstream ss;
	WriteCatalogEntries(ss, catalog_entries, CreatedByTarget(ccontext, catalog_entries));
	WriteStringStreamToFile(fs, ss, fs.JoinPath(info->file_path, "schema.sql"));

	// write the load.sql file
	// for every table, we write COPY INTO statement with the specified options
	stringstream load_ss;
	for (idx_t i = 0; i < exported_tables->data.size(); i++) {
		auto exported_table_info = exported_tables->data[i].table_data;
		WriteCopyStatement(fs, load_ss, *info, exported_table_info, exported_tables->data[i].entry);
	}
	WriteStringStreamToFile(fs, load_ss, fs.JoinPath(info->file_path, "load.sql"));
	state.finished = true;

	return SourceResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType PhysicalExport::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	// nop
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalExport::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	// EXPORT has an optional child
	// we only need to schedule child pipelines if there is a child
	auto &state = meta_pipeline.GetState();
	state.SetPipelineSource(current, *this);
	if (children.empty()) {
		return;
	}
	PhysicalOperator::BuildPipelines(current, meta_pipeline);
}

vector<const_reference<PhysicalOperator>> PhysicalExport::GetSources() const {
	return {*this};
}

} // namespace duckdb
