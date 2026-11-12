#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"

using namespace duckdb;

namespace {

struct HeldState {
	int64_t *value;
};

atomic<int64_t> held_values {0};
atomic<idx_t> updated_rows {0};
atomic<idx_t> interrupt_after_rows {0};
atomic<bool> interrupt_on_combine {false};
atomic<bool> interrupt_on_finalize {false};
Connection *interrupted_connection = nullptr;
atomic<idx_t> block_allocations {0};
atomic<idx_t> failing_block_allocation {0};
atomic<bool> counting_block_allocations {false};
atomic<bool> count_next_finalize {false};

data_ptr_t FailingAllocate(PrivateAllocatorData *, idx_t size) {
	if (size >= DEFAULT_BLOCK_ALLOC_SIZE && counting_block_allocations &&
	    ++block_allocations == failing_block_allocation) {
		return nullptr;
	}
	return data_ptr_cast(malloc(size));
}

void FailingFree(PrivateAllocatorData *, data_ptr_t pointer, idx_t) {
	free(pointer);
}

data_ptr_t FailingReallocate(PrivateAllocatorData *, data_ptr_t pointer, idx_t, idx_t size) {
	return data_ptr_cast(realloc(pointer, size));
}

idx_t HeldSize(AggregateStateInput &) {
	return sizeof(HeldState);
}

void HeldInit(AggregateStateInput &, data_ptr_t *states, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		reinterpret_cast<HeldState *>(states[i])->value = nullptr;
	}
}

void HeldUpdate(Vector inputs[], AggregateInputData &, idx_t, Vector &state_vector, idx_t count) {
	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(sdata);
	auto states = UnifiedVectorFormat::GetData<HeldState *>(sdata);
	auto values = inputs[0].Values<int64_t>();
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[sdata.sel->get_index(i)];
		if (!state.value) {
			state.value = new int64_t(0);
			held_values++;
		}
		*state.value += values[i].GetValue();
	}
	const auto after = interrupt_after_rows.load();
	if (after != 0 && updated_rows.fetch_add(count) + count >= after) {
		interrupted_connection->Interrupt();
	}
}

void HeldCombine(Vector &source, Vector &target, AggregateInputData &, idx_t count) {
	UnifiedVectorFormat sdata;
	source.ToUnifiedFormat(sdata);
	auto sources = UnifiedVectorFormat::GetData<HeldState *>(sdata);
	auto targets = FlatVector::GetDataMutable<HeldState *>(target);
	for (idx_t i = 0; i < count; i++) {
		auto &from = *sources[sdata.sel->get_index(i)];
		auto &to = *targets[i];
		if (!from.value) {
			continue;
		}
		if (!to.value) {
			to.value = from.value;
			from.value = nullptr;
			continue;
		}
		*to.value += *from.value;
	}
	if (interrupt_on_combine) {
		interrupted_connection->Interrupt();
	}
}

void HeldFinalize(Vector &state_vector, AggregateFinalizeInputData &, Vector &result, idx_t count, idx_t offset) {
	UnifiedVectorFormat sdata;
	state_vector.ToUnifiedFormat(sdata);
	auto states = UnifiedVectorFormat::GetData<HeldState *>(sdata);
	auto results = FlatVector::GetDataMutable<int64_t>(result);
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[sdata.sel->get_index(i)];
		results[offset + i] = state.value ? *state.value : 0;
	}
	if (interrupt_on_finalize) {
		interrupted_connection->Interrupt();
	}
	if (count_next_finalize) {
		if (block_allocations != 0) {
			count_next_finalize = false;
			counting_block_allocations = false;
		} else {
			counting_block_allocations = true;
		}
	}
}

void HeldDestroy(Vector &state_vector, AggregateInputData &, idx_t count) {
	auto states = FlatVector::GetData<HeldState *>(state_vector);
	for (idx_t i = 0; i < count; i++) {
		if (states[i]->value) {
			delete states[i]->value;
			states[i]->value = nullptr;
			held_values--;
		}
	}
}

void RegisterHeldSum(Connection &con) {
	AggregateFunction fn("held_sum", {LogicalType::BIGINT}, LogicalType::BIGINT, HeldSize, HeldInit, HeldUpdate,
	                     HeldCombine, HeldFinalize, FunctionNullHandling::DEFAULT_NULL_HANDLING);
	fn.SetStateDestructorCallback(HeldDestroy);
	CreateAggregateFunctionInfo info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });
}

unique_ptr<QueryResult> RunHeldSum(Connection &con, const string &group, idx_t after_rows, bool on_combine,
                                   bool on_finalize) {
	updated_rows = 0;
	interrupt_after_rows = after_rows;
	interrupt_on_combine = on_combine;
	interrupt_on_finalize = on_finalize;
	auto result = con.Query("SELECT sum(s) FROM (SELECT " + group +
	                        " AS g, held_sum(v) AS s FROM range(2000000) t(v) GROUP BY g)");
	interrupt_after_rows = 0;
	interrupt_on_combine = false;
	interrupt_on_finalize = false;
	return result;
}

unique_ptr<QueryResult> RunFailingAllocation(Connection &con, const string &query, idx_t allocation,
                                             bool next_finalize) {
	block_allocations = 0;
	failing_block_allocation = allocation;
	count_next_finalize = next_finalize;
	counting_block_allocations = !next_finalize;
	auto result = con.Query(query);
	counting_block_allocations = false;
	count_next_finalize = false;
	failing_block_allocation = 0;
	return result;
}

void FailEachBlockAllocation(Connection &con, const string &query, int64_t sum, bool next_finalize) {
	REQUIRE_NO_FAIL(*RunFailingAllocation(con, query, 0, next_finalize));
	const auto allocation_count = block_allocations.load();
	for (idx_t allocation = 1; allocation <= allocation_count; allocation++) {
		auto result = RunFailingAllocation(con, query, allocation, next_finalize);
		if (result->HasError()) {
			REQUIRE(result->GetErrorType() == ExceptionType::OUT_OF_MEMORY);
		} else {
			REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == sum);
		}
		REQUIRE(held_values == 0);
	}
}

} // namespace

TEST_CASE("An interrupted hash aggregate destroys every aggregate state it holds", "[api][aggregate_function]") {
	DuckDB db(nullptr);
	Connection con(db);
	interrupted_connection = &con;
	RegisterHeldSum(con);
	REQUIRE_NO_FAIL(con.Query("SET threads = 4"));

	for (const string group : {"v % 500000", "[v % 500000]::BIGINT[1]"}) {
		auto result = RunHeldSum(con, group, 0, false, false);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Collection().GetValue(0, 0).GetValue<int64_t>() == 1999999000000);
		REQUIRE(held_values == 0);

		REQUIRE_FAIL(RunHeldSum(con, group, 1500000, false, false));
		REQUIRE(held_values == 0);

		REQUIRE_FAIL(RunHeldSum(con, group, 0, true, false));
		REQUIRE(held_values == 0);

		REQUIRE_FAIL(RunHeldSum(con, group, 0, false, true));
		REQUIRE(held_values == 0);
	}

	REQUIRE_NO_FAIL(con.Query("SET memory_limit = '200MB'"));
	REQUIRE_FAIL(RunHeldSum(con, "v % 500000", 1500000, false, false));
	REQUIRE(held_values == 0);
}

TEST_CASE("A hash aggregate that runs out of memory destroys every aggregate state it holds",
          "[api][aggregate_function]") {
	DBConfig config;
	config.allocator =
	    make_uniq<Allocator>(FailingAllocate, FailingFree, FailingReallocate, make_uniq<PrivateAllocatorData>());
	DuckDB db(nullptr, &config);
	Connection con(db);
	RegisterHeldSum(con);
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("SET debug_force_external = true"));

	FailEachBlockAllocation(con,
	                        "SELECT sum(s) FROM (SELECT v AS g, held_sum(v) AS s FROM range(400000) t(v) GROUP BY g)",
	                        79999800000, false);
	FailEachBlockAllocation(con,
	                        "SELECT sum(s) FROM (SELECT v AS g, held_sum(v) AS s FROM range(1100000) t(v) GROUP BY g)",
	                        604999450000, true);
}
