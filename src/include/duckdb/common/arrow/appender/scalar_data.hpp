//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/arrow/appender/scalar_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arrow/appender/append_data.hpp"
#include "duckdb/function/table/arrow.hpp"

#include "duckdb/common/bswap.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Scalar Types
//===--------------------------------------------------------------------===//
struct ArrowScalarConverter {
	template <class TGT, class SRC>
	static TGT Operation(SRC input) {
		return input;
	}
};

struct ArrowIntervalConverter {
	template <class TGT, class SRC>
	static TGT Operation(SRC input) {
		ArrowInterval result;
		result.months = input.months;
		result.days = input.days;
		result.nanoseconds = input.micros * Interval::NANOS_PER_MICRO;
		return result;
	}
};

struct ArrowTimeTzConverter {
	template <class TGT, class SRC>
	static TGT Operation(SRC input) {
		return input.time().value;
	}
};

struct ArrowUUIDBlobConverter {
	template <class TGT, class SRC>
	static TGT Operation(hugeint_t input) {
		// Turn into big-end
		auto upper = BSwapIfLE(input.lower);
		// flip Upper MSD
		auto lower =
		    BSwapIfLE(static_cast<int64_t>(static_cast<uint64_t>(input.upper) ^ (static_cast<uint64_t>(1) << 63)));
		return {static_cast<int64_t>(upper), static_cast<uint64_t>(lower)};
	}
};

template <class TGT, class SRC = TGT, class OP = ArrowScalarConverter>
struct ArrowScalarBaseData {
	static void Append(ArrowAppendData &append_data, const Vector &input, idx_t from, idx_t to, idx_t input_size) {
		D_ASSERT(to >= from);
		idx_t size = to - from;
		D_ASSERT(size <= input_size);
		UnifiedVectorFormat format;
		input.ToUnifiedFormat(format);

		// append the validity mask
		append_data.AppendValidity(format, from, to);

		// append the main data
		auto &main_buffer = append_data.GetMainBuffer();
		main_buffer.resize(main_buffer.size() + sizeof(TGT) * size);
		auto data = UnifiedVectorFormat::GetData<SRC>(format);
		auto result_data = main_buffer.GetData<TGT>();

		if (!format.validity.CanHaveNull()) {
			for (idx_t i = from; i < to; i++) {
				auto source_idx = format.sel->get_index(i);
				result_data[append_data.row_count + i - from] = OP::template Operation<TGT, SRC>(data[source_idx]);
			}
		} else {
			for (idx_t i = from; i < to; i++) {
				auto source_idx = format.sel->get_index(i);
				auto result_idx = append_data.row_count + i - from;
				if (!format.validity.RowIsValid(source_idx)) {
					result_data[result_idx] = TGT();
					continue;
				}
				result_data[result_idx] = OP::template Operation<TGT, SRC>(data[source_idx]);
			}
		}
		append_data.row_count += size;
	}
};

template <class TGT, class SRC = TGT, class OP = ArrowScalarConverter>
struct ArrowScalarData : public ArrowScalarBaseData<TGT, SRC, OP> {
	static void Initialize(ArrowAppendData &result, const LogicalType &type, idx_t capacity) {
		result.GetMainBuffer().reserve(capacity * sizeof(TGT));
	}

	static void Finalize(ArrowAppendData &append_data, const LogicalType &type, ArrowArray *result) {
		result->n_buffers = 2;
		result->buffers[1] = append_data.GetMainBuffer().data();
	}
};

} // namespace duckdb
