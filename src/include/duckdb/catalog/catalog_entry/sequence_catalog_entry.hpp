//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class ClientContext;
class DuckTransaction;
class SequenceCatalogEntry;
class WriteAheadLog;

struct SequenceValue {
	SequenceCatalogEntry *entry;
	uint64_t usage_count;
	int64_t counter;
};

struct SequenceSessionValue {
	int64_t next = 0;
	idx_t remaining = 0;
	int64_t increment = 0;
	uint64_t usage_count = 0;
	int64_t counter = 0;
	optional<int64_t> last;
};

struct SequenceSession {
	static SequenceSession &Get(ClientContext &context);

	mutex lock;
	unordered_map<idx_t, SequenceSessionValue> values;
};

struct SequenceRuns {
	int64_t first[2];
	idx_t count[2];
	idx_t size = 0;
};

struct SequenceData {
	explicit SequenceData(CreateSequenceInfo &info);

	//! The amount of times the sequence has been used
	uint64_t usage_count;
	//! The sequence counter
	int64_t counter;
	//! The most recently returned value
	optional<int64_t> last_value;
	//! The increment value
	int64_t increment;
	//! The start_value of the sequence
	int64_t start_value;
	//! The minimum value of the sequence
	int64_t min_value;
	//! The maximum value of the sequence
	int64_t max_value;
	//! Whether or not the sequence cycles
	bool cycle;
	uint64_t cache;
};

struct SequenceState {
	explicit SequenceState(const SequenceData &data);

	mutable mutex lock;
	SequenceData data;
	uint64_t reserved_usage_count;
	int64_t reserved_counter;
	shared_ptr<WriteAheadLog> reserved_log;
	idx_t reserved_offset = 0;
	uint64_t durable_usage_count;
	int64_t durable_counter;
	int64_t block_next = 0;
	idx_t block_remaining = 0;
	bool logging = false;
	idx_t appending = 0;
	idx_t generation = 0;
};

//! A sequence catalog entry
class SequenceCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::SEQUENCE_ENTRY;
	static constexpr const char *Name = "sequence";

public:
	//! Create a real TableCatalogEntry and initialize storage for it
	SequenceCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateSequenceInfo &info);
	~SequenceCatalogEntry() override;

public:
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
	unique_ptr<CreateInfo> GetInfo() const override;
	unique_ptr<CatalogEntry> AlterEntry(ClientContext &context, AlterInfo &info) override;
	void SetAsRoot(optional_ptr<CatalogTransaction> transaction) override;

	SequenceData GetData() const;
	SequenceValue GetReservedValue();
	int64_t CurrentValue(SequenceSession &session);
	int64_t NextValue(DuckTransaction &transaction, SequenceSession &session);
	void NextValues(DuckTransaction &transaction, SequenceSession &session, idx_t count, SequenceRuns &runs);
	int64_t NextValues(DuckTransaction &transaction, idx_t count);
	int64_t SetValue(DuckTransaction &transaction, SequenceSession &session, int64_t value, bool is_called);
	int64_t SetValue(DuckTransaction &transaction, int64_t value, bool is_called);
	void ReplayValue(uint64_t usage_count, int64_t counter, optional<int64_t> last_value);
	void Cover(uint64_t usage_count);
	void ReserveInCommit(WriteAheadLog &catalog_log, uint64_t usage_count, vector<SequenceValue> &durable_after);
	void MarkReserved(const SequenceValue &value);
	void MarkDurable(const SequenceValue &value);
	bool LogsValues() const;

	string ToSQL() const override;

private:
	idx_t Block() const;
	void ThrowIfSuperseded() const;
	SequenceData Restarted(AlterSequenceInfo &info) const;
	SequenceData Reserved() const;
	void Fetch(SequenceSessionValue &cached, idx_t needed);
	void FetchLocked(SequenceSessionValue &cached, idx_t needed);
	void FinishAppend();
	void RaiseReserved(uint64_t usage_count, int64_t counter, shared_ptr<WriteAheadLog> log, idx_t offset);
	void RaiseDurable(uint64_t usage_count, int64_t counter);
	void AppendReservation(const SequenceData &target, bool wait);
	void MakeDurable(unique_lock<mutex> &seqlock, const SequenceData &target);

private:
	shared_ptr<SequenceState> state;
	idx_t generation = 0;
	shared_ptr<SequenceState> replaced;
	idx_t replaced_generation = 0;
};
} // namespace duckdb
