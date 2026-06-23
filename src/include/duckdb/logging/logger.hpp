//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/logger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/logging/logging.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/string_util.hpp"
#include <duckdb/common/types/string_type.hpp>

namespace duckdb {
class TableDescription;
class DatabaseInstance;
class DataChunk;
class LogManager;
class ColumnDataCollection;
class ThreadContext;
class FileOpener;
class LogStorage;
class ExecutionContext;
struct FileHandle;

//! Internal
#define DUCKDB_LOG_INTERNAL(SOURCE, TYPE, LEVEL, ...)                                                                  \
	{                                                                                                                  \
		auto &logger_ref_ = Logger::Get(SOURCE);                                                                       \
		if (logger_ref_.LogEnabled() && logger_ref_.ShouldLog(TYPE, LEVEL)) {                                          \
			logger_ref_.WriteLog(TYPE, LEVEL, __VA_ARGS__);                                                            \
		}                                                                                                              \
	}

//! Default Loggers
#define DUCKDB_LOG_TRACE(SOURCE, ...)                                                                                  \
	DUCKDB_LOG_INTERNAL(SOURCE, DefaultLogType::NAME, LogLevel::LOG_TRACE, __VA_ARGS__)
#define DUCKDB_LOG_DEBUG(SOURCE, ...)                                                                                  \
	DUCKDB_LOG_INTERNAL(SOURCE, DefaultLogType::NAME, LogLevel::LOG_DEBUG, __VA_ARGS__)
#define DUCKDB_LOG_INFO(SOURCE, ...) DUCKDB_LOG_INTERNAL(SOURCE, DefaultLogType::NAME, LogLevel::LOG_INFO, __VA_ARGS__)
#define DUCKDB_LOG_WARNING(SOURCE, ...)                                                                                \
	DUCKDB_LOG_INTERNAL(SOURCE, DefaultLogType::NAME, LogLevel::LOG_WARNING, __VA_ARGS__)
#define DUCKDB_LOG_ERROR(SOURCE, ...)                                                                                  \
	DUCKDB_LOG_INTERNAL(SOURCE, DefaultLogType::NAME, LogLevel::LOG_ERROR, __VA_ARGS__)
#define DUCKDB_LOG_FATAL(SOURCE, ...)                                                                                  \
	DUCKDB_LOG_INTERNAL(SOURCE, DefaultLogType::NAME, LogLevel::LOG_FATAL, __VA_ARGS__)

//! LogType based loggers
#define DUCKDB_LOG(SOURCE, LOG_TYPE_CLASS, ...)                                                                        \
	DUCKDB_LOG_INTERNAL(SOURCE, LOG_TYPE_CLASS::NAME, LOG_TYPE_CLASS::LEVEL,                                           \
	                    LOG_TYPE_CLASS::ConstructLogMessage(__VA_ARGS__))

//! Main logging interface
class Logger {
public:
	DUCKDB_API explicit Logger(LogManager &manager) : manager(manager) {
	}

	DUCKDB_API virtual ~Logger() = default;

	// Main Logging interface. In most cases the macros above should be used instead of calling these directly
	DUCKDB_API virtual bool ShouldLog(std::string_view log_type, LogLevel log_level) = 0;
	DUCKDB_API void WriteLog(std::string_view log_type, LogLevel log_level, std::string_view message);

	// Inline gate so DUCKDB_LOG_INTERNAL skips the virtual ShouldLog dispatch when this logger is
	// disabled (the shared NopLogger used when logging is off, the common case).
	bool LogEnabled() const noexcept {
		return log_enabled.load(std::memory_order_relaxed);
	}

	// const char* disambiguates a no-extra-arg call from the format template below; string_view
	// covers std::string / string_view callers. (string_t callers pass a view explicitly.)
	DUCKDB_API void WriteLog(std::string_view log_type, LogLevel log_level, const char *message) {
		WriteLog(log_type, log_level, std::string_view(message));
	}

	// Syntactic sugar for formatted strings
	template <typename... ARGS>
	void WriteLog(std::string_view log_type, LogLevel log_level, std::string_view format_string, ARGS... params) {
		auto formatted_string = StringUtil::Format(format_string, params...);
		WriteLog(log_type, log_level, std::string_view(formatted_string));
	}

	DUCKDB_API virtual void Flush() = 0;

	// Get the Logger to write log messages to. In decreasing order of preference(!) so the ThreadContext getter is the
	// most preferred way of fetching the logger and the DatabaseInstance getter the least preferred. This has to do
	// both with logging performance and level of detail of logging context that is provided.
	DUCKDB_API static Logger &Get(const ThreadContext &thread_context);
	DUCKDB_API static Logger &Get(const ExecutionContext &execution_context);
	DUCKDB_API static Logger &Get(const ClientContext &client_context);
	DUCKDB_API static Logger &Get(const FileOpener &opener);
	DUCKDB_API static Logger &Get(const DatabaseInstance &db);
	DUCKDB_API static Logger &Get(const shared_ptr<Logger> &logger);

	template <class T>
	static void Flush(T &log_context_source) {
		Get(log_context_source).Flush();
	}

	DUCKDB_API virtual bool IsThreadSafe() = 0;
	DUCKDB_API virtual bool IsMutable() {
		return false;
	};
	DUCKDB_API virtual void UpdateConfig(LogConfig &new_config) {
		throw InternalException("Cannot update the config of this logger!");
	}
	DUCKDB_API virtual const LogConfig &GetConfig() const = 0;

protected:
	virtual void WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view message) = 0;

protected:
	LogManager &manager;
	// Lock-free gate read by LogEnabled(). NopLogger leaves it false; enabled loggers set it true.
	atomic<bool> log_enabled = false;
};

// Thread-safe logger
class ThreadSafeLogger : public Logger {
public:
	explicit ThreadSafeLogger(shared_ptr<const LogConfig> config_p, LoggingContext &context_p, LogManager &manager);
	explicit ThreadSafeLogger(shared_ptr<const LogConfig> config_p, RegisteredLoggingContext context_p,
	                          LogManager &manager);

	// Main Logger API
	bool ShouldLog(std::string_view log_type, LogLevel log_level) override;
	void WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view message) override;

	void Flush() override;
	bool IsThreadSafe() override {
		return true;
	}
	const LogConfig &GetConfig() const override {
		return *config;
	}

protected:
	const shared_ptr<const LogConfig> config;
	mutex lock;
	const RegisteredLoggingContext context;
};

// Non Thread-safe logger
// - will cache log entries locally
class ThreadLocalLogger : public Logger {
public:
	explicit ThreadLocalLogger(LogConfig &config_p, LoggingContext &context_p, LogManager &manager);
	explicit ThreadLocalLogger(LogConfig &config_p, RegisteredLoggingContext context_p, LogManager &manager);

	// Main Logger API
	bool ShouldLog(std::string_view log_type, LogLevel log_level) override;
	void WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view message) override;
	void Flush() override;

	bool IsThreadSafe() override {
		return false;
	}
	const LogConfig &GetConfig() const override {
		return config;
	}

protected:
	const LogConfig config;
	const RegisteredLoggingContext context;
};

// Thread-safe Logger with mutable log settings
class MutableLogger : public Logger {
public:
	explicit MutableLogger(LogConfig &config_p, LoggingContext &context_p, LogManager &manager);
	explicit MutableLogger(LogConfig &config_p, RegisteredLoggingContext context_p, LogManager &manager);

	// Main Logger API
	bool ShouldLog(std::string_view log_type, LogLevel log_level) override;
	void WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view message) override;

	void Flush() override;
	bool IsThreadSafe() override {
		return true;
	}
	bool IsMutable() override {
		return true;
	}
	const LogConfig &GetConfig() const override {
		return config;
	}
	void UpdateConfig(LogConfig &new_config) override;

protected:
	// Atomics for lock-free log setting checks (enabled lives in the base as log_enabled)
	atomic<LogMode> mode;
	atomic<LogLevel> level;

	mutex lock;
	LogConfig config;
	const RegisteredLoggingContext context;
};

// For when logging is disabled: NOPs everything
class NopLogger : public Logger {
public:
	explicit NopLogger(LogManager &manager) : Logger(manager) {
	}
	bool ShouldLog(std::string_view log_type, LogLevel log_level) override {
		return false;
	}
	void WriteLogInternal(std::string_view log_type, LogLevel log_level, std::string_view message) override {};
	void Flush() override {
	}
	bool IsThreadSafe() override {
		return true;
	}
	const LogConfig &GetConfig() const override {
		throw InternalException("Called GetConfig on NopLogger");
	}
};

} // namespace duckdb
