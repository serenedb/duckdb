#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_storage.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/local_file_system.hpp"

namespace duckdb {

shared_ptr<Logger> LogManager::CreateLogger(LoggingContext context, bool thread_safe, bool mutable_settings) {
	auto nop = [&] {
		return shared_ptr<Logger>(shared_ptr<Logger>(), &nop_logger);
	};
	if (!mutable_settings && !any_logging_enabled.load(std::memory_order_relaxed)) {
		return nop();
	}

	if (mutable_settings) {
		return make_shared_ptr<MutableLogger>(config, RegisterLoggingContextInternal(context), *this);
	}
	// Recheck after the lock-free gate: logging may have been disabled in the window.
	if (!any_logging_enabled.load(std::memory_order_relaxed)) {
		return nop();
	}
	if (!thread_safe) {
		// TODO: implement ThreadLocalLogger and return it here
	}
	auto snapshot = config_snapshot.Read([](const shared_ptr<const LogConfig> &cfg) { return cfg; });
	// The any_logging_enabled gate and the snapshot are read without a lock, so logging may have
	// been disabled (and the snapshot updated) in between: don't build a ThreadSafeLogger from a
	// disabled snapshot, fall back to the shared nop logger instead.
	if (!snapshot->enabled) {
		return nop();
	}
	return make_shared_ptr<ThreadSafeLogger>(std::move(snapshot), RegisterLoggingContextInternal(context), *this);
}

RegisteredLoggingContext LogManager::RegisterLoggingContext(LoggingContext &context) {
	return RegisterLoggingContextInternal(context);
}

bool LogManager::RegisterLogStorage(const string &name, shared_ptr<LogStorage> &storage) {
	if (registered_log_storages.find(name) != registered_log_storages.end()) {
		return false;
	}
	registered_log_storages.insert({name, std::move(storage)});
	return true;
}

Logger &LogManager::GlobalLogger() {
	return *global_logger;
}

shared_ptr<Logger> LogManager::GlobalLoggerReference() {
	return global_logger;
}

void LogManager::Flush() {
	// Hot path: ClientContext::Begin/EndQueryInternal flush per query. When nothing has
	// been logged since the last flush, skip the lock + storage flush entirely.
	if (!has_buffered_entries.exchange(false, std::memory_order_relaxed)) {
		return;
	}
	unique_lock<mutex> lck(lock);
	log_storage->FlushAll();
}

shared_ptr<LogStorage> LogManager::GetLogStorage() {
	unique_lock<mutex> lck(lock);
	return log_storage;
}

bool LogManager::CanScan(LoggingTargetTable table) {
	unique_lock<mutex> lck(lock);
	return log_storage->CanScan(table);
}

LogManager::LogManager(DatabaseInstance &db, LogConfig config_p)
    : config(std::move(config_p)), config_snapshot(make_shared_ptr<const LogConfig>(config)), nop_logger(*this),
      db_instance(db) {
	any_logging_enabled.store(config.enabled, std::memory_order_relaxed);
	log_storage = make_uniq<InMemoryLogStorage>(db);
}

LogManager::~LogManager() {
}

void LogManager::Initialize() {
	LoggingContext context(LogContextScope::DATABASE);
	global_logger = CreateLogger(context, true, true);

	RegisterDefaultLogTypes();
}

LogManager &LogManager::Get(ClientContext &context) {
	return context.db->GetLogManager();
}

RegisteredLoggingContext LogManager::RegisterLoggingContextInternal(LoggingContext &context) {
	auto id = next_registered_logging_context_index.fetch_add(1, std::memory_order_relaxed);
	if (id == NumericLimits<idx_t>::Maximum()) {
		throw InternalException("Ran out of available log context ids.");
	}
	return {id, context};
}

void LogManager::WriteLogEntry(timestamp_t timestamp, std::string_view log_type, LogLevel log_level,
                               std::string_view log_message, const RegisteredLoggingContext &context) {
	if (log_level == LogLevel::LOG_WARNING && Settings::Get<WarningsAsErrorsSetting>(db_instance)) {
		throw InvalidInputException(log_message);
	} else {
		unique_lock<mutex> lck(lock);
		log_storage->WriteLogEntry(timestamp, log_level, log_type, log_message, context);
		has_buffered_entries.store(true, std::memory_order_relaxed);
	}
}

void LogManager::FlushCachedLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) {
	throw NotImplementedException("FlushCachedLogEntries");
}

void LogManager::SetConfig(DatabaseInstance &db, const LogConfig &config_p) {
	unique_lock<mutex> lck(lock);

	// We need extra handling for switching storage
	SetLogStorageInternal(db, config_p.storage);

	SetConfigInternal(config_p);
}

void LogManager::SetEnableLogging(bool enable) {
	unique_lock<mutex> lck(lock);
	config.enabled = enable;
	PublishConfigInternal();
	any_logging_enabled.store(enable, std::memory_order_relaxed);
}

void LogManager::SetLogMode(LogMode mode) {
	unique_lock<mutex> lck(lock);
	config.mode = mode;
	PublishConfigInternal();
}

void LogManager::SetLogLevel(LogLevel level) {
	unique_lock<mutex> lck(lock);
	config.level = level;
	PublishConfigInternal();
}

void LogManager::SetEnabledLogTypes(optional_ptr<unordered_set<string>> enabled_log_types) {
	unique_lock<mutex> lck(lock);
	if (enabled_log_types) {
		config.enabled_log_types = *enabled_log_types;
	} else {
		config.enabled_log_types = {};
	}
	PublishConfigInternal();
}

void LogManager::SetDisabledLogTypes(optional_ptr<unordered_set<string>> disabled_log_types) {
	unique_lock<mutex> lck(lock);
	if (disabled_log_types) {
		config.disabled_log_types = *disabled_log_types;
	} else {
		config.disabled_log_types = {};
	}
	PublishConfigInternal();
}

void LogManager::SetLogStorage(DatabaseInstance &db, const string &storage_name) {
	unique_lock<mutex> lck(lock);
	SetLogStorageInternal(db, storage_name);
}

void LogManager::SetLogStorageInternal(DatabaseInstance &db, const string &storage_name) {
	auto storage_name_to_lower = StringUtil::Lower(storage_name);

	if (config.storage == storage_name_to_lower) {
		return;
	}

	if (storage_name_to_lower == LogConfig::FILE_STORAGE_NAME) {
		auto &fs = FileSystem::GetFileSystem(db);
		if (fs.SubSystemIsDisabled(LocalFileSystem().GetName())) {
			throw InvalidConfigurationException("Can not enable file logging with the LocalFileSystem disabled");
		}
	}

	// Flush the old storage, we are going to replace it.
	log_storage->FlushAll();

	if (storage_name_to_lower == LogConfig::IN_MEMORY_STORAGE_NAME) {
		log_storage = make_shared_ptr<InMemoryLogStorage>(db);
	} else if (storage_name_to_lower == LogConfig::STDOUT_STORAGE_NAME) {
		log_storage = make_shared_ptr<StdOutLogStorage>(db);
	} else if (storage_name_to_lower == LogConfig::FILE_STORAGE_NAME) {
		log_storage = make_shared_ptr<FileLogStorage>(db);
	} else if (registered_log_storages.find(storage_name_to_lower) != registered_log_storages.end()) {
		log_storage = registered_log_storages[storage_name_to_lower];
	} else {
		throw InvalidInputException("Log storage '%s' is not yet registered", storage_name);
	}
	config.storage = storage_name_to_lower;
}

void LogManager::UpdateLogStorageConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config_value) {
	unique_lock<mutex> lck(lock);
	log_storage->UpdateConfig(db, config_value);
}

void LogManager::SetEnableStructuredLoggers(vector<string> &enabled_logger_types) {
	unique_lock<mutex> lck(lock);

	LogConfig new_config = config;
	new_config.enabled_log_types.clear();

	LogLevel min_log_level = LogLevel::LOG_FATAL;

	for (const auto &enabled_logger_type : enabled_logger_types) {
		auto lookup = LookupLogTypeInternal(enabled_logger_type);
		if (!lookup) {
			throw InvalidInputException("Unknown log type: '%s'", enabled_logger_type);
		}

		new_config.enabled_log_types.insert(lookup->name);

		min_log_level = MinValue(min_log_level, lookup->level);
	}

	new_config.level = min_log_level;
	new_config.mode = LogMode::ENABLE_SELECTED;
	new_config.enabled = true;

	SetConfigInternal(new_config);
}

void LogManager::TruncateLogStorage() {
	unique_lock<mutex> lck(lock);
	log_storage->Truncate();
}

LogConfig LogManager::GetConfig() {
	unique_lock<mutex> lck(lock);
	return config;
}

optional_ptr<const LogType> LogManager::LookupLogType(const string &type) {
	unique_lock<mutex> lck(lock);
	return LookupLogTypeInternal(type);
}

DUCKDB_API void RegisterDefaultLogTypes() {
}

optional_ptr<const LogType> LogManager::LookupLogTypeInternal(const string &type) {
	auto lookup = registered_log_types.find(type);
	if (lookup != registered_log_types.end()) {
		return *lookup->second;
	}
	return nullptr;
}

void LogManager::SetConfigInternal(LogConfig config_p) {
	// Apply the remainder of the config
	config = std::move(config_p);
	PublishConfigInternal();
	any_logging_enabled.store(config.enabled, std::memory_order_relaxed);
}

void LogManager::PublishConfigInternal() {
	auto snapshot = make_shared_ptr<const LogConfig>(config);
	config_snapshot.Write([&snapshot](shared_ptr<const LogConfig> &cfg) { cfg = snapshot; });
	global_logger->UpdateConfig(config);
}

void LogManager::RegisterLogType(unique_ptr<LogType> type) {
	unique_lock<mutex> lck(lock);

	auto lookup = registered_log_types.find(type->name);
	if (lookup != registered_log_types.end()) {
		throw InvalidInputException("Registered log writer '%s' already exists", type->name);
	}

	registered_log_types[type->name] = std::move(type);
}

void LogManager::RegisterDefaultLogTypes() {
	RegisterLogType(make_uniq<DefaultLogType>());
	RegisterLogType(make_uniq<FileSystemLogType>());
	RegisterLogType(make_uniq<HTTPLogType>());
	RegisterLogType(make_uniq<QueryLogType>());
	RegisterLogType(make_uniq<PhysicalOperatorLogType>());
	RegisterLogType(make_uniq<MetricsLogType>());
	RegisterLogType(make_uniq<AdaptiveFilterLogType>());
	RegisterLogType(make_uniq<ParquetPrefetchLogType>());
}

} // namespace duckdb
