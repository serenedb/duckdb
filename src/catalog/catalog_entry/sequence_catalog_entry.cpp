#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_sequence_info.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/common/operator/add.hpp"
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

SequenceCatalogEntry::SequenceCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateSequenceInfo &info)
    : StandardEntry(CatalogType::SEQUENCE_ENTRY, schema, catalog, info.GetSequenceName(), info.oid), data(info),
      reserved_usage_count(data.usage_count), reserved_counter(data.counter), durable_usage_count(data.usage_count),
      durable_counter(data.counter) {
	this->temporary = info.temporary;
	this->comment = info.comment;
	this->tags = info.tags;
	this->permissions = info.permissions;
}

unique_ptr<CatalogEntry> SequenceCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	auto &cast_info = info_copy->Cast<CreateSequenceInfo>();

	auto result = make_uniq<SequenceCatalogEntry>(catalog, ParentSchema(context), cast_info);
	lock_guard<mutex> seqlock(lock);
	result->data = data;
	result->reserved_usage_count = reserved_usage_count;
	result->reserved_counter = reserved_counter;
	result->durable_usage_count = durable_usage_count;
	result->durable_counter = durable_counter;

	return std::move(result);
}

SequenceData SequenceCatalogEntry::GetData() const {
	lock_guard<mutex> seqlock(lock);
	return data;
}

SequenceValue SequenceCatalogEntry::GetReservedValue() {
	lock_guard<mutex> seqlock(lock);
	return SequenceValue {this, reserved_usage_count, reserved_counter};
}

bool SequenceCatalogEntry::LogsValues() const {
	return !temporary && timestamp < TRANSACTION_ID_START && catalog.UsesCatalogLog();
}

static SequenceData LogAhead(SequenceData data, idx_t steps) {
	if (!data.cycle) {
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
	return LOG_AHEAD_VALUES;
}

SequenceData SequenceCatalogEntry::Reserved() const {
	auto result = data;
	result.usage_count = reserved_usage_count;
	result.counter = reserved_counter;
	return result;
}

void SequenceCatalogEntry::RaiseReserved(uint64_t usage_count, int64_t counter) {
	if (usage_count > reserved_usage_count) {
		reserved_usage_count = usage_count;
		reserved_counter = counter;
	}
}

void SequenceCatalogEntry::RaiseDurable(uint64_t usage_count, int64_t counter) {
	RaiseReserved(usage_count, counter);
	if (usage_count > durable_usage_count) {
		durable_usage_count = usage_count;
		durable_counter = counter;
	}
}

void SequenceCatalogEntry::AppendReservation(const SequenceData &target) {
	auto log = catalog.CatalogLog();
	if (!log) {
		throw InternalException("Sequence \"%s\" advanced without a catalog log", name);
	}
	idx_t offset;
	{
		auto wal_lock = log->GetStorageManager().GetWALLock();
		log = catalog.CatalogLog();
		if (!log) {
			throw InternalException("Sequence \"%s\" advanced without a catalog log", name);
		}
		log->WriteUseCatalog(catalog.GetAttached().oid);
		log->WriteSequenceValue(SequenceValue {this, target.usage_count, target.counter});
		offset = log->FlushAppendNoSync();
		lock_guard<mutex> seqlock(lock);
		RaiseReserved(target.usage_count, target.counter);
	}
	log->GroupSync(offset);
}

void SequenceCatalogEntry::MakeDurable(unique_lock<mutex> &seqlock, const SequenceData &target) {
	lock.Await(NotLogging(&logging));
	logging = true;
	seqlock.unlock();
	try {
		AppendReservation(target);
	} catch (...) {
		seqlock.lock();
		logging = false;
		throw;
	}
	seqlock.lock();
	RaiseDurable(target.usage_count, target.counter);
	logging = false;
}

void SequenceCatalogEntry::Cover(uint64_t usage_count) {
	unique_lock<mutex> seqlock(lock);
	while (usage_count > durable_usage_count) {
		if (logging) {
			lock.Await(NotLogging(&logging));
			continue;
		}
		logging = true;
		const bool append = usage_count > reserved_usage_count;
		const auto target = append ? LogAhead(data, Block()) : Reserved();
		seqlock.unlock();
		try {
			if (append) {
				AppendReservation(target);
			} else {
				catalog.SyncCatalogLog();
			}
		} catch (...) {
			seqlock.lock();
			logging = false;
			throw;
		}
		seqlock.lock();
		RaiseDurable(target.usage_count, target.counter);
		logging = false;
	}
}

void SequenceCatalogEntry::ReserveInCommit(WriteAheadLog &catalog_log, uint64_t usage_count,
                                           vector<SequenceValue> &durable_after) {
	unique_lock<mutex> seqlock(lock);
	if (usage_count <= durable_usage_count) {
		return;
	}
	const bool append = usage_count > reserved_usage_count;
	const auto target = append ? LogAhead(data, Block()) : Reserved();
	seqlock.unlock();
	if (append) {
		catalog_log.WriteUseCatalog(catalog.GetAttached().oid);
		catalog_log.WriteSequenceValue(SequenceValue {this, target.usage_count, target.counter});
	}
	durable_after.push_back(SequenceValue {this, target.usage_count, target.counter});
}

void SequenceCatalogEntry::MarkReserved(const SequenceValue &value) {
	lock_guard<mutex> seqlock(lock);
	RaiseReserved(value.usage_count, value.counter);
}

void SequenceCatalogEntry::MarkDurable(const SequenceValue &value) {
	lock_guard<mutex> seqlock(lock);
	RaiseDurable(value.usage_count, value.counter);
}

void SequenceCatalogEntry::Fetch(SequenceSessionValue &cached, idx_t needed) {
	lock_guard<mutex> seqlock(lock);
	const auto cache = MaxValue<idx_t>(data.cache, 1);
	const auto count = (needed + cache - 1) / cache * cache;
	const auto first = data.counter;
	idx_t taken = 0;
	if (!data.cycle) {
		auto reserved = LogAhead(data, count);
		taken = reserved.usage_count - data.usage_count;
		if (taken == 0) {
			if (data.increment < 0) {
				throw SequenceException("nextval: reached minimum value of sequence \"%s\" (%lld)", name,
				                        data.min_value);
			}
			throw SequenceException("nextval: reached maximum value of sequence \"%s\" (%lld)", name, data.max_value);
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
	lock_guard<mutex> guard(session.lock);
	auto entry = session.values.find(oid);
	if (entry == session.values.end() || !entry->second.last) {
		throw SequenceException("currval: sequence is not yet defined in this session");
	}
	return entry->second.last.value();
}

int64_t SequenceCatalogEntry::NextValue(DuckTransaction &transaction, SequenceSession &session) {
	lock_guard<mutex> guard(session.lock);
	auto &cached = session.values[oid];
	if (cached.remaining == 0) {
		Fetch(cached, 1);
	}
	const auto result = cached.next;
	if (--cached.remaining) {
		cached.next += cached.increment;
	}
	cached.last = result;
	if (!temporary) {
		transaction.PushSequenceUsage(*this, cached.usage_count, cached.counter);
	}
	return result;
}

void SequenceCatalogEntry::NextValues(DuckTransaction &transaction, SequenceSession &session, idx_t count,
                                      SequenceRuns &runs) {
	lock_guard<mutex> guard(session.lock);
	auto &cached = session.values[oid];
	runs.size = 0;
	for (idx_t produced = 0; produced < count;) {
		if (cached.remaining == 0) {
			Fetch(cached, count - produced);
		}
		const auto take = MinValue<idx_t>(cached.remaining, count - produced);
		if (runs.size == 2) {
			throw InternalException("Sequence \"%s\" handed out a batch in more than two runs", name);
		}
		runs.first[runs.size] = cached.next;
		runs.count[runs.size] = take;
		runs.size++;
		produced += take;
		cached.remaining -= take;
		cached.last =
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
	unique_lock<mutex> seqlock(lock);
	const bool logs_values = LogsValues();
	if (logs_values) {
		lock.Await(NotLogging(&logging));
	}
	if (value < data.min_value) {
		throw SequenceException("setval: value %lld is out of bounds for sequence \"%s\" (%lld..%lld)", value, name,
		                        data.min_value, data.max_value);
	}
	if (value > data.max_value) {
		throw SequenceException("setval: value %lld is out of bounds for sequence \"%s\" (%lld..%lld)", value, name,
		                        data.min_value, data.max_value);
	}
	block_remaining = 0;
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
		data.last_value.reset();
	}
	if (logs_values) {
		data.usage_count = MaxValue(data.usage_count, reserved_usage_count) + 1;
		MakeDurable(seqlock, data);
		return value;
	}
	data.usage_count++;
	if (!temporary) {
		transaction.PushSequenceUsage(*this, data.usage_count, data.counter);
	}
	return value;
}

int64_t SequenceCatalogEntry::NextValues(DuckTransaction &transaction, idx_t count) {
	if (count == 0) {
		throw InternalException("SequenceCatalogEntry::NextValues requires a positive count");
	}
	lock_guard<mutex> seqlock(lock);
	if (!data.cycle) {
		if (block_remaining < count) {
			auto reserved = LogAhead(data, MaxValue<idx_t>(count, data.cache));
			const auto taken = reserved.usage_count - data.usage_count;
			if (taken >= count) {
				block_next = data.counter;
				block_remaining = taken;
				data.last_value = Hugeint::Cast<int64_t>(hugeint_t(reserved.counter) - hugeint_t(data.increment));
				data.counter = reserved.counter;
				data.usage_count = reserved.usage_count;
			}
		}
		if (block_remaining >= count) {
			const auto base = block_next;
			block_remaining -= count;
			if (block_remaining) {
				block_next = Hugeint::Cast<int64_t>(hugeint_t(base) + hugeint_t(count) * hugeint_t(data.increment));
			}
			if (!temporary) {
				transaction.PushSequenceUsage(*this, data.usage_count, data.counter);
			}
			return base;
		}
	}
	int64_t base = data.counter;
	for (idx_t i = 0; i < count; i++) {
		int64_t result = data.counter;
		bool overflow = !TryAddOperator::Operation(data.counter, data.increment, data.counter);
		if (data.cycle) {
			if (overflow) {
				data.counter = data.increment < 0 ? data.max_value : data.min_value;
			} else if (data.counter < data.min_value) {
				data.counter = data.max_value;
			} else if (data.counter > data.max_value) {
				data.counter = data.min_value;
			}
		} else {
			if (result < data.min_value || (overflow && data.increment < 0)) {
				throw SequenceException("nextval: reached minimum value of sequence \"%s\" (%lld)", name,
				                        data.min_value);
			}
			if (result > data.max_value || overflow) {
				throw SequenceException("nextval: reached maximum value of sequence \"%s\" (%lld)", name,
				                        data.max_value);
			}
		}
		data.last_value = result;
		data.usage_count++;
	}
	if (!temporary) {
		transaction.PushSequenceUsage(*this, data.usage_count, data.counter);
	}
	return base;
}

void SequenceCatalogEntry::ReplayValue(uint64_t v_usage_count, int64_t v_counter, optional<int64_t> last_value) {
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
	result->SetQualifiedName(QualifiedName(catalog.GetName(), ParentSchemaName(), name));
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
