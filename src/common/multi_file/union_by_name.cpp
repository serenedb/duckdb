#include "duckdb/common/multi_file/union_by_name.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"

namespace duckdb {

class UnionByReaderTask : public BaseExecutorTask {
public:
	UnionByReaderTask(TaskExecutor &executor, ClientContext &context, const vector<OpenFileInfo> &files,
	                  atomic<idx_t> &next_file, vector<shared_ptr<BaseUnionData>> &readers,
	                  BaseFileReaderOptions &options, MultiFileOptions &file_options,
	                  MultiFileReader &multi_file_reader, MultiFileReaderInterface &interface)
	    : BaseExecutorTask(executor), context(context), files(files), next_file(next_file), readers(readers),
	      options(options), file_options(file_options), multi_file_reader(multi_file_reader), interface(interface) {
	}

	void ExecuteTask() override {
		while (!executor.HasError()) {
			const auto file_idx = next_file++;
			if (file_idx >= files.size()) {
				break;
			}
			auto reader = multi_file_reader.CreateReader(context, files[file_idx], options, file_options, interface);
			readers[file_idx] = reader->GetUnionData(file_idx);
		}
	}

	string TaskType() const override {
		return "UnionByReaderTask";
	}

private:
	ClientContext &context;
	const vector<OpenFileInfo> &files;
	atomic<idx_t> &next_file;
	vector<shared_ptr<BaseUnionData>> &readers;
	BaseFileReaderOptions &options;
	MultiFileOptions &file_options;
	MultiFileReader &multi_file_reader;
	MultiFileReaderInterface &interface;
};

vector<shared_ptr<BaseUnionData>> UnionByName::UnionCols(ClientContext &context, const vector<OpenFileInfo> &files,
                                                         vector<LogicalType> &union_col_types,
                                                         vector<Identifier> &union_col_names,
                                                         BaseFileReaderOptions &options, MultiFileOptions &file_options,
                                                         MultiFileReader &multi_file_reader,
                                                         MultiFileReaderInterface &interface) {
	vector<shared_ptr<BaseUnionData>> union_readers;
	union_readers.resize(files.size());

	atomic<idx_t> next_file {0};
	const auto num_tasks = MinValue<idx_t>(files.size(), TaskScheduler::QueryThreads(context));
	TaskExecutor executor(context);
	// schedule tasks for all files
	for (idx_t task_idx = 0; task_idx < num_tasks; task_idx++) {
		auto task = make_uniq<UnionByReaderTask>(executor, context, files, next_file, union_readers, options,
		                                         file_options, multi_file_reader, interface);
		executor.ScheduleTask(std::move(task));
	}
	// complete all tasks
	executor.WorkOnTasks();

	// now combine the result schemas
	identifier_map_t<idx_t> union_names_map;
	for (auto &reader : union_readers) {
		auto &col_names = reader->names;
		auto &sql_types = reader->types;
		CombineUnionTypes(col_names, sql_types, union_col_types, union_col_names, union_names_map);
	}
	return union_readers;
}

} // namespace duckdb
