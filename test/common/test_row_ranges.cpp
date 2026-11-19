#include "catch.hpp"
#include "duckdb/common/fault_points.hpp"
#include "duckdb/formats/row_lexer.hpp"
#include "duckdb/formats/row_ranges.hpp"

#include <chrono>
#include <cstring>
#include <thread>

using namespace duckdb;

namespace {

class RowRangesTestStream : public RowSource {
public:
	explicit RowRangesTestStream(string data_p) : data(std::move(data_p)) {
	}

	idx_t Read(char *out, idx_t count, idx_t offset) const override {
		if (offset >= data.size()) {
			return 0;
		}
		const idx_t size = MinValue<idx_t>(count, data.size() - offset);
		memcpy(out, data.data() + offset, size);
		return size;
	}

	optional_idx End() const override {
		return data.size();
	}

private:
	string data;
};

} // namespace

#ifdef SDB_FAULT_INJECTION
TEST_CASE("A stream range is empty when another reader finds the stream ends before it", "[csv]") {
	string data;
	for (idx_t row = 0; row < 50; row++) {
		data += "a,b\n";
	}
	const auto dialect = RowDialect::PostgresCsv(',', '"', '"');
	RowLexer lexer(dialect, 2, 1 << 20, true);
	RowRangesTestStream source(data);
	RowRanges ranges(lexer, source, 0, NumericLimits<idx_t>::Maximum(), 64, true);
	idx_t range = 0;
	for (idx_t claimed = 0; claimed <= 10; claimed++) {
		REQUIRE(ranges.Claim(range));
	}
	REQUIRE(range == 10);

	RowRangeReader reader(ranges);
	reader.Start(range, true, false, false);
	FaultPoints::Arm("row_range_lex");
	auto result = RowReadResult::ROWS;
	std::thread worker([&]() { result = reader.Advance(STANDARD_VECTOR_SIZE); });
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!FaultPoints::Armed("row_range_lex:parked") && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool parked = FaultPoints::Armed("row_range_lex:parked");
	ranges.FoundEnd(data.size());
	FaultPoints::Arm("row_range_lex:release");
	worker.join();
	FaultPoints::Clear();
	REQUIRE(parked);
	REQUIRE(result == RowReadResult::DONE);
}
#endif
