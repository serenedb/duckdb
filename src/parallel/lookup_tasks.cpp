#include "duckdb/parallel/lookup_tasks.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/parallel/task_executor.hpp"

namespace duckdb {

void LookupTaskOutput::ResetFound(ClientContext &context, const vector<LogicalType> &types, idx_t count) {
	if (found.ColumnCount() == 0 || found.GetCapacity() < count) {
		found.Destroy();
		found.Initialize(context, types, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
		found_rows.resize(found.GetCapacity());
	}
	found.Reset();
}

idx_t LookupTaskOutput::AppendFound(DataChunk &output, const idx_t *column_map, idx_t *survivors, idx_t w) {
	const auto count = found.size();
	for (idx_t c = 0; c < output.ColumnCount(); c++) {
		if (column_map[c] != DConstants::INVALID_INDEX) {
			VectorOperations::Copy(found.data[c], output.data[c], count, 0, w);
		}
	}
	std::copy_n(found_rows.begin(), count, survivors + w);
	return w + count;
}

namespace {

class LookupTask : public BaseExecutorTask {
public:
	LookupTask(TaskExecutor &executor, idx_t task, idx_t count, atomic<idx_t> &next,
	           const std::function<void(idx_t, idx_t)> &step)
	    : BaseExecutorTask(executor), task(task), count(count), next(next), step(step) {
	}

	TaskExecutionResult ExecuteTaskStep() override {
		const auto index = next++;
		if (index >= count) {
			return TaskExecutionResult::TASK_FINISHED;
		}
		step(task, index);
		return next < count ? TaskExecutionResult::TASK_NOT_FINISHED : TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "LookupTask";
	}

private:
	idx_t task;
	idx_t count;
	atomic<idx_t> &next;
	const std::function<void(idx_t, idx_t)> &step;
};

} // namespace

void RunLookupTasks(ClientContext &context, idx_t tasks, idx_t count, const std::function<void(idx_t, idx_t)> &step) {
	TaskExecutor executor(context);
	atomic<idx_t> next {0};
	for (idx_t t = 0; t < tasks; t++) {
		executor.ScheduleTask(make_uniq<LookupTask>(executor, t, count, next, step));
	}
	executor.WorkOnTasks();
}

} // namespace duckdb
