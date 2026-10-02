#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/write_ahead_log.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>
#include <sstream>

namespace duckdb {

constexpr const char *SequenceCatalogEntry::Name;

SequenceSession &SequenceSession::Get(ClientContext &context) {
	return *ClientData::Get(context).sequence_session;
}

SequenceData::SequenceData(CreateSequenceInfo &info)
    : usage_count(info.usage_count), counter(info.start_value), last_value(info.last_value), increment(info.increment),
      start_value(info.start_value), min_value(info.min_value), max_value(info.max_value), cycle(info.cycle),
      cache(info.cache) {
}

SequenceState::SequenceState(const SequenceData &data_p)
    : data(data_p), reserved_usage_count(data.usage_count), reserved_counter(data.counter),
      durable_usage_count(data.usage_count), durable_counter(data.counter) {
}

SequenceCatalogEntry::SequenceCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateSequenceInfo &info)
    : StandardEntry(CatalogType::SEQUENCE_ENTRY, schema, catalog, info.GetSequenceName(), info.oid),
      state(make_shared_ptr<SequenceState>(SequenceData(info))) {
	this->temporary = info.temporary;
	this->comment = info.comment;
	this->tags = info.tags;
	this->permissions = info.permissions;
}

SequenceCatalogEntry::~SequenceCatalogEntry() {
	if (replaced) {
		lock_guard<mutex> seqlock(replaced->lock);
		replaced->generation = replaced_generation;
	}
}

unique_ptr<CatalogEntry> SequenceCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateSequenceInfo>();

	auto result = make_uniq<SequenceCatalogEntry>(catalog, ParentSchema(context), cast_info);
	result->state = state;
	result->generation = generation;
	return std::move(result);
}

static absl::Condition Settled(SequenceState *state) {
	return absl::Condition(
	    +[](SequenceState *settling) { return !settling->logging && settling->appending == 0; }, state);
}

unique_ptr<CatalogEntry> SequenceCatalogEntry::AlterEntry(ClientContext &context, AlterInfo &info) {
	if (!info.GetNewName()) {
		return CatalogEntry::AlterEntry(context, info);
	}
	auto result = unique_ptr_cast<CatalogEntry, SequenceCatalogEntry>(CatalogEntry::AlterEntry(context, info));
	{
		lock_guard<mutex> seqlock(state->lock);
		state->lock.Await(Settled(state.get()));
		ThrowIfSuperseded();
		result->generation = state->generation + 1;
		result->replaced = state;
		result->replaced_generation = state->generation;
		state->generation++;
	}
	return std::move(result);
}

void SequenceCatalogEntry::SetAsRoot(optional_ptr<CatalogTransaction> transaction) {
	replaced.reset();
	lock_guard<mutex> seqlock(state->lock);
	state->generation = generation;
}

void SequenceCatalogEntry::ThrowIfSuperseded() const {
	if (generation != state->generation) {
		throw TransactionException("Transaction conflict: sequence %s was altered by another transaction", name);
	}
}

SequenceData SequenceCatalogEntry::GetData() const {
	lock_guard<mutex> seqlock(state->lock);
	return state->data;
}

SequenceValue SequenceCatalogEntry::GetReservedValue() {
	lock_guard<mutex> seqlock(state->lock);
	return SequenceValue {this, state->reserved_usage_count, state->reserved_counter};
}

bool SequenceCatalogEntry::Committed() const {
	return timestamp < TRANSACTION_ID_START;
}

bool SequenceCatalogEntry::LogsValues() const {
	return !temporary && Committed() && catalog.UsesCatalogLog();
}

static SequenceData LogAhead(SequenceData data, idx_t steps) {
	if (!data.cycle) {
		int64_t span;
		int64_t last;
		int64_t next;
		if (steps > 0 && steps <= idx_t(NumericLimits<int64_t>::Maximum()) && data.counter >= data.min_value &&
		    data.counter <= data.max_value &&
		    TryMultiplyOperator::Operation(data.increment, int64_t(steps - 1), span) &&
		    TryAddOperator::Operation(data.counter, span, last) && last >= data.min_value && last <= data.max_value &&
		    TryAddOperator::Operation(last, data.increment, next)) {
			data.counter = next;
			data.usage_count += steps;
			return data;
		}
		auto increment = hugeint_t(data.increment);
		auto limit =
		    data.increment > 0
		        ? MinValue<hugeint_t>(data.max_value, hugeint_t(NumericLimits<int64_t>::Maximum()) - increment)
		        : MaxValue<hugeint_t>(data.min_value, hugeint_t(NumericLimits<int64_t>::Minimum()) - increment);
		auto counter = hugeint_t(data.counter);
		hugeint_t available = 0;
		if (data.counter >= data.min_value && data.counter <= data.max_value) {
			auto distance = data.increment > 0 ? limit - counter : counter - limit;
			if (distance >= 0) {
				available = distance / (data.increment > 0 ? increment : -increment) + 1;
			}
		}
		auto taken = MinValue<hugeint_t>(available, Hugeint::Convert(steps));
		data.counter = Hugeint::Cast<int64_t>(counter + taken * increment);
		data.usage_count += Hugeint::Cast<uint64_t>(taken);
		return data;
	}
	for (idx_t i = 0; i < steps; i++) {
		int64_t next;
		if (!TryAddOperator::Operation(data.counter, data.increment, next)) {
			next = data.increment < 0 ? data.max_value : data.min_value;
		} else if (next < data.min_value) {
			next = data.max_value;
		} else if (next > data.max_value) {
			next = data.min_value;
		}
		data.counter = next;
		data.usage_count++;
	}
	return data;
}

static absl::Condition NotLogging(bool *logging) {
	return absl::Condition(
	    +[](bool *busy) { return !*busy; }, logging);
}

idx_t SequenceCatalogEntry::Block() const {
	static constexpr idx_t LOG_AHEAD_VALUES = 32;
	static constexpr idx_t MAX_LOG_AHEAD_VALUES = 4096;
	const auto grown = NextPowerOfTwo(state->data.usage_count / 16);
	const auto block = MinValue<idx_t>(MaxValue<idx_t>(LOG_AHEAD_VALUES, grown), MAX_LOG_AHEAD_VALUES);
	return MaxValue<idx_t>(block, state->data.cache);
}

SequenceData SequenceCatalogEntry::Reserved() const {
	auto result = state->data;
	result.usage_count = state->reserved_usage_count;
	result.counter = state->reserved_counter;
	return result;
}

void SequenceCatalogEntry::RaiseReserved(uint64_t usage_count, int64_t counter, shared_ptr<WriteAheadLog> log,
                                         idx_t offset) {
	if (usage_count > state->reserved_usage_count) {
		state->reserved_usage_count = usage_count;
		state->reserved_counter = counter;
		state->reserved_log = std::move(log);
		state->reserved_offset = offset;
	}
}

void SequenceCatalogEntry::RaiseDurable(uint64_t usage_count, int64_t counter) {
	RaiseReserved(usage_count, counter, nullptr, 0);
	if (usage_count > state->durable_usage_count) {
		state->durable_usage_count = usage_count;
		state->durable_counter = counter;
	}
}

void SequenceCatalogEntry::AppendReservation(const SequenceData &target, bool wait) {
	auto log = catalog.CatalogLog();
	if (!log) {
		throw InternalException("Sequence %s advanced without a catalog log", name);
	}
	idx_t offset;
	{
		auto commit_lock = log->GetStorageManager().GetCommitLock();
		log = catalog.CatalogLog();
		if (!log) {
			throw InternalException("Sequence %s advanced without a catalog log", name);
		}
		log->WriteUseCatalog(catalog.GetAttached().oid);
		log->WriteSequenceValue(SequenceValue {this, target.usage_count, target.counter});
		offset = log->FlushMarker();
		lock_guard<mutex> seqlock(state->lock);
		RaiseReserved(target.usage_count, target.counter, log, offset);
	}
	if (wait) {
		log->SyncUpTo(offset);
	} else {
		catalog.RequestCatalogLogSync(std::move(log), offset);
	}
}

void SequenceCatalogEntry::MakeDurable(unique_lock<mutex> &seqlock, const SequenceData &target) {
	state->lock.Await(NotLogging(&state->logging));
	state->logging = true;
	seqlock.unlock();
	try {
		AppendReservation(target, true);
	} catch (...) {
		seqlock.lock();
		state->logging = false;
		throw;
	}
	seqlock.lock();
	RaiseDurable(target.usage_count, target.counter);
	state->logging = false;
}

void SequenceCatalogEntry::Cover(uint64_t usage_count) {
	unique_lock<mutex> seqlock(state->lock);
	while (usage_count > state->durable_usage_count) {
		if (usage_count <= state->reserved_usage_count && state->reserved_log) {
			auto log = state->reserved_log;
			const auto offset = state->reserved_offset;
			const auto target = Reserved();
			seqlock.unlock();
			log->SyncUpTo(offset);
			seqlock.lock();
			RaiseDurable(target.usage_count, target.counter);
			continue;
		}
		if (state->logging) {
			state->lock.Await(NotLogging(&state->logging));
			continue;
		}
		state->logging = true;
		const bool append = usage_count > state->reserved_usage_count;
		const auto target = append ? LogAhead(state->data, Block()) : Reserved();
		seqlock.unlock();
		try {
			if (append) {
				AppendReservation(target, true);
			} else {
				catalog.SyncCatalogLog();
			}
		} catch (...) {
			seqlock.lock();
			state->logging = false;
			throw;
		}
		seqlock.lock();
		RaiseDurable(target.usage_count, target.counter);
		state->logging = false;
	}
}

void SequenceCatalogEntry::ReserveInCommit(WriteAheadLog &catalog_log, uint64_t usage_count,
                                           vector<SequenceValue> &durable_after) {
	unique_lock<mutex> seqlock(state->lock);
	if (usage_count <= state->durable_usage_count) {
		return;
	}
	const bool append = usage_count > state->reserved_usage_count;
	const auto target = append ? LogAhead(state->data, Block()) : Reserved();
	seqlock.unlock();
	if (append) {
		catalog_log.WriteUseCatalog(catalog.GetAttached().oid);
		catalog_log.WriteSequenceValue(SequenceValue {this, target.usage_count, target.counter});
	}
	durable_after.push_back(SequenceValue {this, target.usage_count, target.counter});
}

void SequenceCatalogEntry::MarkReserved(const SequenceValue &value) {
	lock_guard<mutex> seqlock(state->lock);
	RaiseReserved(value.usage_count, value.counter, nullptr, 0);
}

void SequenceCatalogEntry::MarkDurable(const SequenceValue &value) {
	lock_guard<mutex> seqlock(state->lock);
	RaiseDurable(value.usage_count, value.counter);
}

void SequenceCatalogEntry::Fetch(SequenceSessionValue &cached, idx_t needed) {
	optional<SequenceData> reservation;
	{
		lock_guard<mutex> seqlock(state->lock);
		FetchLocked(cached, needed);
		const auto block = Block();
		if (LogsValues() && state->appending == 0 &&
		    state->data.usage_count + block / 2 > state->reserved_usage_count) {
			reservation = LogAhead(state->data, block);
			state->appending++;
		}
	}
	if (reservation) {
		try {
			AppendReservation(*reservation, false);
		} catch (...) {
			FinishAppend();
			throw;
		}
		FinishAppend();
	}
}

void SequenceCatalogEntry::FinishAppend() {
	lock_guard<mutex> seqlock(state->lock);
	state->appending--;
}

void SequenceCatalogEntry::FetchLocked(SequenceSessionValue &cached, idx_t needed) {
	ThrowIfSuperseded();
	auto &data = state->data;
	const auto cache = Committed() ? MaxValue<idx_t>(data.cache, 1) : 1;
	const auto count = (needed + cache - 1) / cache * cache;
	const auto first = data.counter;
	idx_t taken = 0;
	if (!data.cycle) {
		auto reserved = LogAhead(data, count);
		taken = reserved.usage_count - data.usage_count;
		if (taken == 0) {
			if (data.increment < 0) {
				throw SequenceException("nextval: reached minimum value of sequence %s (%lld)", name, data.min_value);
			}
			throw SequenceException("nextval: reached maximum value of sequence %s (%lld)", name, data.max_value);
		}
		data.counter = reserved.counter;
		data.usage_count = reserved.usage_count;
	} else {
		int64_t expected = first;
		while (taken < count && data.counter == expected) {
			int64_t result = data.counter;
			bool overflow = !TryAddOperator::Operation(data.counter, data.increment, data.counter);
			if (overflow) {
				data.counter = data.increment < 0 ? data.max_value : data.min_value;
			} else if (data.counter < data.min_value) {
				data.counter = data.max_value;
			} else if (data.counter > data.max_value) {
				data.counter = data.min_value;
			}
			data.usage_count++;
			taken++;
			if (overflow || !TryAddOperator::Operation(result, data.increment, expected)) {
				break;
			}
		}
	}
	data.last_value = Hugeint::Cast<int64_t>(hugeint_t(first) + hugeint_t(taken - 1) * hugeint_t(data.increment));
	cached.next = first;
	cached.remaining = taken;
	cached.increment = data.increment;
	cached.usage_count = data.usage_count;
	cached.counter = data.counter;
}

int64_t SequenceCatalogEntry::CurrentValue(SequenceSession &session) {
	if (catalog.Compatibility() != SqlCompatibility::POSTGRES) {
		lock_guard<mutex> seqlock(state->lock);
		if (!state->data.last_value) {
			throw SequenceException("currval: sequence is not yet defined in this session");
		}
		return state->data.last_value.value();
	}
	lock_guard<mutex> guard(session.lock);
	auto entry = session.values.find(oid);
	if (entry == session.values.end() || !entry->second.last) {
		throw SequenceException("currval: sequence is not yet defined in this session");
	}
	return entry->second.last.value();
}

int64_t SequenceCatalogEntry::NextValue(DuckTransaction &transaction, SequenceSession &session) {
	lock_guard<mutex> guard(session.lock);
	auto &session_value = session.values[oid];
	SequenceSessionValue uncommitted;
	auto &cached = Committed() ? session_value : uncommitted;
	if (cached.remaining == 0) {
		Fetch(cached, 1);
	}
	const auto result = cached.next;
	if (--cached.remaining) {
		cached.next += cached.increment;
	}
	session_value.last = result;
	if (!temporary) {
		transaction.PushSequenceUsage(*this, cached.usage_count, cached.counter);
	}
	return result;
}

void SequenceCatalogEntry::NextValues(DuckTransaction &transaction, SequenceSession &session, idx_t count,
                                      SequenceRuns &runs) {
	lock_guard<mutex> guard(session.lock);
	auto &session_value = session.values[oid];
	SequenceSessionValue uncommitted;
	auto &cached = Committed() ? session_value : uncommitted;
	runs.size = 0;
	for (idx_t produced = 0; produced < count;) {
		if (cached.remaining == 0) {
			Fetch(cached, count - produced);
		}
		const auto take = MinValue<idx_t>(cached.remaining, count - produced);
		if (runs.size == 2) {
			throw InternalException("Sequence %s handed out a batch in more than two runs", name);
		}
		runs.first[runs.size] = cached.next;
		runs.count[runs.size] = take;
		runs.size++;
		produced += take;
		cached.remaining -= take;
		session_value.last =
		    Hugeint::Cast<int64_t>(hugeint_t(cached.next) + hugeint_t(take - 1) * hugeint_t(cached.increment));
		if (cached.remaining) {
			cached.next =
			    Hugeint::Cast<int64_t>(hugeint_t(cached.next) + hugeint_t(take) * hugeint_t(cached.increment));
		}
	}
	if (!temporary) {
		transaction.PushSequenceUsage(*this, cached.usage_count, cached.counter);
	}
}

int64_t SequenceCatalogEntry::SetValue(DuckTransaction &transaction, SequenceSession &session, int64_t value,
                                       bool is_called) {
	auto result = SetValue(transaction, value, is_called);
	lock_guard<mutex> guard(session.lock);
	auto &cached = session.values[oid];
	cached.remaining = 0;
	if (is_called) {
		cached.last = value;
	}
	return result;
}

int64_t SequenceCatalogEntry::SetValue(DuckTransaction &transaction, int64_t value, bool is_called) {
	unique_lock<mutex> seqlock(state->lock);
	const bool logs_values = LogsValues();
	state->lock.Await(Settled(state.get()));
	ThrowIfSuperseded();
	auto &data = state->data;
	if (value < data.min_value || value > data.max_value) {
		throw SequenceException("setval: value %lld is out of bounds for sequence %s (%lld..%lld)", value, name,
		                        data.min_value, data.max_value);
	}
	state->block_remaining = 0;
	if (is_called) {
		const bool overflow = !TryAddOperator::Operation(value, data.increment, data.counter);
		if (data.cycle) {
			if (overflow) {
				data.counter = data.increment < 0 ? data.max_value : data.min_value;
			} else if (data.counter < data.min_value) {
				data.counter = data.max_value;
			} else if (data.counter > data.max_value) {
				data.counter = data.min_value;
			}
		} else if (overflow) {
			data.counter = data.increment < 0 ? data.min_value : data.max_value;
		}
		data.last_value = value;
	} else {
		data.counter = value;
	}
	data.usage_count = MaxValue(data.usage_count, state->reserved_usage_count) + 1;
	if (logs_values) {
		const auto target = data;
		MakeDurable(seqlock, target);
		return value;
	}
	if (!temporary) {
		transaction.PushSequenceUsage(*this, data.usage_count, data.counter);
	}
	return value;
}

int64_t SequenceCatalogEntry::NextValues(DuckTransaction &transaction, idx_t count) {
	if (count == 0) {
		throw InternalException("SequenceCatalogEntry::NextValues requires a positive count");
	}
	lock_guard<mutex> seqlock(state->lock);
	ThrowIfSuperseded();
	auto &data = state->data;
	if (!data.cycle) {
		if (state->block_remaining < count) {
			auto reserved = LogAhead(data, MaxValue<idx_t>(count, data.cache));
			const auto taken = reserved.usage_count - data.usage_count;
			if (taken >= count) {
				state->block_next = data.counter;
				state->block_remaining = taken;
				data.last_value = Hugeint::Cast<int64_t>(hugeint_t(reserved.counter) - hugeint_t(data.increment));
				data.counter = reserved.counter;
				data.usage_count = reserved.usage_count;
			}
		}
		if (state->block_remaining >= count) {
			const auto base = state->block_next;
			state->block_remaining -= count;
			if (state->block_remaining) {
				state->block_next =
				    Hugeint::Cast<int64_t>(hugeint_t(base) + hugeint_t(count) * hugeint_t(data.increment));
			}
			if (!temporary) {
				transaction.PushSequenceUsage(*this, data.usage_count, data.counter);
			}
			return base;
		}
	}
	auto counter = data.counter;
	int64_t result = counter;
	for (idx_t i = 0; i < count; i++) {
		result = counter;
		bool overflow = !TryAddOperator::Operation(result, data.increment, counter);
		if (data.cycle) {
			if (overflow) {
				counter = data.increment < 0 ? data.max_value : data.min_value;
			} else if (counter < data.min_value) {
				counter = data.max_value;
			} else if (counter > data.max_value) {
				counter = data.min_value;
			}
		} else {
			if (result < data.min_value || (overflow && data.increment < 0)) {
				throw SequenceException("nextval: reached minimum value of sequence %s (%lld)", name, data.min_value);
			}
			if (result > data.max_value || overflow) {
				throw SequenceException("nextval: reached maximum value of sequence %s (%lld)", name, data.max_value);
			}
		}
	}
	auto base = data.counter;
	data.counter = counter;
	data.last_value = result;
	data.usage_count += count;
	if (!temporary) {
		transaction.PushSequenceUsage(*this, data.usage_count, data.counter);
	}
	return base;
}

void SequenceCatalogEntry::ReplayValue(uint64_t v_usage_count, int64_t v_counter, optional<int64_t> last_value) {
	auto &data = state->data;
	if (v_usage_count > data.usage_count) {
		data.usage_count = v_usage_count;
		data.counter = v_counter;
		data.last_value = last_value;
		RaiseDurable(v_usage_count, v_counter);
	}
}

unique_ptr<CreateInfo> SequenceCatalogEntry::GetInfo() const {
	auto seq_data = GetData();

	auto result = make_uniq<CreateSequenceInfo>();
	result->SetQualifiedName(GetQualifiedName(name));
	result->usage_count = seq_data.usage_count;
	result->increment = seq_data.increment;
	result->min_value = seq_data.min_value;
	result->max_value = seq_data.max_value;
	result->start_value = seq_data.counter;
	result->cycle = seq_data.cycle;
	result->cache = seq_data.cache;
	result->last_value = seq_data.last_value;
	result->dependencies = dependencies;
	result->comment = comment;
	result->tags = tags;
	result->permissions = permissions;
	return std::move(result);
}

string SequenceCatalogEntry::ToSQL() const {
	auto seq_data = GetData();

	duckdb::stringstream ss;
	ss << "CREATE SEQUENCE ";
	ss << name.GetIdentifierName();
	ss << " INCREMENT BY " << seq_data.increment;
	ss << " MINVALUE " << seq_data.min_value;
	ss << " MAXVALUE " << seq_data.max_value;
	ss << " START " << seq_data.counter;
	if (seq_data.cache != 1) {
		ss << " CACHE " << seq_data.cache;
	}
	ss << " " << (seq_data.cycle ? "CYCLE" : "NO CYCLE") << ";";
	return ss.str();
}
} // namespace duckdb
