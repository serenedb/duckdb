#include "duckdb/logging/log_storage.hpp"
#include "duckdb/logging/log_manager.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {

void Logger::WriteLog(std::string_view log_type, LogLevel log_level, std::string_view message) {
	WriteLogInternal(log_type, log_level, message);
}

Logger &Logger::Get(const DatabaseInstance &db) {
	return db.GetLogManager().GlobalLogger();
}

Logger &Logger::Get(const ThreadContext &thread_context) {
	return *thread_context.logger;
}

Logger &Logger::Get(const ExecutionContext &execution_context) {
	return *execution_context.thread.logger;
}

Logger &Logger::Get(const ClientContext &client_context) {
	return client_context.GetLogger();
}

Logger &Logger::Get(const FileOpener &opener) {
	return opener.GetLogger();
}

Logger &Logger::Get(const shared_ptr<Logger> &logger) {
	return *logger;
}

ThreadSafeLogger::ThreadSafeLogger(shared_ptr<const LogConfig> config_p, LoggingContext &context_p, LogManager &manager)
    : ThreadSafeLogger(std::move(config_p), manager.RegisterLoggingContext(context_p), manager) {
}

ThreadSafeLogger::ThreadSafeLogger(shared_ptr<const LogConfig> config_p, RegisteredLoggingContext context_p,
                                   LogManager &manager)
    : Logger(manager), config(std::move(config_p)), context(context_p) {
	// NopLogger should be used instead
	D_ASSERT(config->enabled);
	log_enabled = true;
}

bool ThreadSafeLogger::ShouldLog(std::string_view log_type, LogLevel log_level) {
	if (config->level > log_level) {
		return false;
	}

	// TODO: we would ideally do prefix matching, not string matching here
	if (config->mode == LogMode::ENABLE_SELECTED) {
		return config->enabled_log_types.find(log_type) != config->enabled_log_types.end();
	}
	if (config->mode == LogMode::DISABLE_SELECTED) {
		return config->disabled_log_types.find(log_type) == config->disabled_log_types.end();
	}
	return true;
}

void ThreadSafeLogger::WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view log_message) {
	manager.WriteLogEntry(Timestamp::GetCurrentTimestamp(), log_type, log_level, log_message, context);
}

void ThreadSafeLogger::Flush() {
	manager.Flush();
	// NOP
}

ThreadLocalLogger::ThreadLocalLogger(LogConfig &config_p, LoggingContext &context_p, LogManager &manager)
    : ThreadLocalLogger(config_p, manager.RegisterLoggingContext(context_p), manager) {
}

ThreadLocalLogger::ThreadLocalLogger(LogConfig &config_p, RegisteredLoggingContext context_p, LogManager &manager)
    : Logger(manager), config(config_p), context(context_p) {
	// NopLogger should be used instead
	D_ASSERT(config_p.enabled);
	log_enabled = true;
}

bool ThreadLocalLogger::ShouldLog(std::string_view log_type, LogLevel log_level) {
	throw NotImplementedException("ThreadLocalLogger::ShouldLog");
}

void ThreadLocalLogger::WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view log_message) {
	throw NotImplementedException("ThreadLocalLogger::WriteLogInternal");
}

void ThreadLocalLogger::Flush() {
	manager.Flush();
}

MutableLogger::MutableLogger(LogConfig &config_p, LoggingContext &context_p, LogManager &manager)
    : MutableLogger(config_p, manager.RegisterLoggingContext(context_p), manager) {
}

MutableLogger::MutableLogger(LogConfig &config_p, RegisteredLoggingContext context_p, LogManager &manager)
    : Logger(manager), config(config_p), context(context_p) {
	log_enabled = config.enabled;
	level = config.level;
	mode = config.mode;
}

void MutableLogger::UpdateConfig(LogConfig &new_config) {
	unique_lock<mutex> lck(lock);
	config = new_config;

	// Update atomics for lock-free access
	log_enabled = config.enabled;
	level = config.level;
	mode = config.mode;
}

void MutableLogger::WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view log_message) {
	manager.WriteLogEntry(Timestamp::GetCurrentTimestamp(), log_type, log_level, log_message, context);
}

bool MutableLogger::ShouldLog(std::string_view log_type, LogLevel log_level) {
	if (!log_enabled) {
		return false;
	}

	// check atomic level to early out if level too low
	if (level > log_level) {
		return false;
	}

	if (mode == LogMode::LEVEL_ONLY) {
		return true;
	}

	// FIXME: ENABLE_SELECTED and DISABLE_SELECTED are expensive and need full global lock
	{
		unique_lock<mutex> lck(lock);
		if (config.mode == LogMode::ENABLE_SELECTED) {
			return config.enabled_log_types.find(log_type) != config.enabled_log_types.end();
		}
		if (config.mode == LogMode::DISABLE_SELECTED) {
			return config.disabled_log_types.find(log_type) == config.disabled_log_types.end();
		}
	}
	throw InternalException("Should be unreachable (MutableLogger::ShouldLog)");
}

void MutableLogger::Flush() {
	manager.Flush();
}

} // namespace duckdb
