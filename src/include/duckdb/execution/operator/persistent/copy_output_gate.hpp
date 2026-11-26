//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/copy_output_gate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parallel/interrupt.hpp"

namespace duckdb {

class CopyOutputGate {
public:
	virtual ~CopyOutputGate() = default;

	virtual bool BlockSink(const InterruptState &interrupt_state) = 0;
};

} // namespace duckdb
