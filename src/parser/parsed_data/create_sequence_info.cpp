#include "duckdb/parser/parsed_data/create_sequence_info.hpp"

namespace duckdb {

CreateSequenceInfo::CreateSequenceInfo()
    : CreateInfo(CatalogType::SEQUENCE_ENTRY, Identifier::InvalidSchema()), usage_count(0), increment(1), min_value(1),
      max_value(NumericLimits<int64_t>::Maximum()), start_value(1), cycle(false), cache(1) {
}

unique_ptr<CreateInfo> CreateSequenceInfo::Copy() const {
	auto result = make_uniq<CreateSequenceInfo>();
	CopyProperties(*result);
	result->usage_count = usage_count;
	result->increment = increment;
	result->min_value = min_value;
	result->max_value = max_value;
	result->start_value = start_value;
	result->cycle = cycle;
	result->cache = cache;
	result->last_value = last_value;
	return std::move(result);
}

string CreateSequenceInfo::ToString() const {
	duckdb::stringstream ss;
	ss << "CREATE";
	if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		ss << " OR REPLACE";
	}
	if (temporary) {
		ss << " TEMPORARY";
	}
	ss << " SEQUENCE ";
	if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		ss << " IF NOT EXISTS ";
	}
	ss << QualifiedNameToString();
	ss << " INCREMENT BY " << increment;
	ss << " MINVALUE " << min_value;
	ss << " MAXVALUE " << max_value;
	ss << " START " << start_value;
	if (cache != 1) {
		ss << " CACHE " << cache;
	}
	ss << " " << (cycle ? "CYCLE" : "NO CYCLE") << ";";
	return ss.str();
}

} // namespace duckdb
