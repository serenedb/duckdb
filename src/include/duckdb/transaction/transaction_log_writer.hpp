#pragma once

namespace duckdb {

class WriteAheadLog;

class TransactionLogWriter {
public:
	virtual ~TransactionLogWriter() = default;

	virtual void WriteToWAL(WriteAheadLog &wal) = 0;
	virtual void OnDurable() noexcept = 0;
};

} // namespace duckdb
