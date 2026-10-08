#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#define CATCH_CONFIG_RUNNER
#include "catch.hpp"
#include <stdlib.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>
#include <unordered_set>

#if defined(__linux__)
#include <features.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef DUCKDB_WINDOWS
#include <chrono>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "debug_fs_extension.hpp"
#include "sqlite/catch_test_reporter.hpp"
#include "sqlite/sqllogic_test_logger.hpp"
#include "sqlite/sqllogic_test_runner.hpp"
#include "test_helpers.hpp"
#include "test_config.hpp"

using namespace duckdb;

namespace {

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 18)
#define DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
#endif
#endif

static bool TryParseThreadStackSize(const string &value, size_t &result) {
	if (value.empty()) {
		return false;
	}
	size_t parsed_value = 0;
	for (auto character : value) {
		if (character < '0' || character > '9') {
			return false;
		}
		auto digit = static_cast<size_t>(character - '0');
		if (parsed_value > (std::numeric_limits<size_t>::max() - digit) / 10) {
			return false;
		}
		parsed_value = parsed_value * 10 + digit;
	}
	if (parsed_value == 0) {
		return false;
	}
	result = parsed_value;
	return true;
}

#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
static string PthreadError(const char *function, int error) {
	return string(function) + " failed: " + std::strerror(error);
}

enum class ThreadStackProbeResult : uint8_t { SUCCESS, TOO_SMALL, ERROR };

// glibc's fixed Linux minimum excludes the binary-specific static TLS overhead.
static constexpr size_t GLIBC_PTHREAD_STACK_MIN = 16384;

static void *MinimumThreadStackProbe(void *) {
	return nullptr;
}

// Probes run on a stack this function maps itself to not pollute the glibc stack cache with stacks created during these
// probes.
static ThreadStackProbeResult TryThreadStackSize(size_t stack_size, string &error) {
	pthread_attr_t attributes;
	auto result = pthread_attr_init(&attributes);
	if (result != 0) {
		error = PthreadError("pthread_attr_init", result);
		return ThreadStackProbeResult::ERROR;
	}

	auto stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED) {
		pthread_attr_destroy(&attributes);
		error = "Failed to map a " + to_string(stack_size) + " byte probe stack";
		return ThreadStackProbeResult::ERROR;
	}

	result = pthread_attr_setstack(&attributes, stack, stack_size);
	if (result != 0) {
		pthread_attr_destroy(&attributes);
		munmap(stack, stack_size);
		if (result == EINVAL) {
			return ThreadStackProbeResult::TOO_SMALL;
		}
		error = PthreadError("pthread_attr_setstack", result);
		return ThreadStackProbeResult::ERROR;
	}

	pthread_t probe;
	result = pthread_create(&probe, &attributes, MinimumThreadStackProbe, nullptr);
	auto destroy_result = pthread_attr_destroy(&attributes);
	if (result != 0) {
		munmap(stack, stack_size);
		if (result == EINVAL) {
			return ThreadStackProbeResult::TOO_SMALL;
		}
		error = PthreadError("pthread_create", result);
		return ThreadStackProbeResult::ERROR;
	}
	auto join_result = pthread_join(probe, nullptr);
	// only once the thread is gone is the stack free to unmap
	munmap(stack, stack_size);
	if (join_result != 0) {
		error = PthreadError("pthread_join", join_result);
		return ThreadStackProbeResult::ERROR;
	}
	if (destroy_result != 0) {
		error = PthreadError("pthread_attr_destroy", destroy_result);
		return ThreadStackProbeResult::ERROR;
	}
	return ThreadStackProbeResult::SUCCESS;
}

static bool GetMinimumThreadStackSize(size_t default_stack_size, size_t page_size, size_t &minimum_stack_size,
                                      string &error) {
	auto lower_page = (GLIBC_PTHREAD_STACK_MIN + page_size - 1) / page_size;
	auto upper_page = default_stack_size / page_size;
	if (upper_page < lower_page) {
		error = "Default pthread stack size is below the glibc minimum";
		return false;
	}
	auto default_probe_result = TryThreadStackSize(upper_page * page_size, error);
	if (default_probe_result != ThreadStackProbeResult::SUCCESS) {
		if (default_probe_result == ThreadStackProbeResult::TOO_SMALL) {
			error = "Unable to create a thread with the default pthread stack size";
		} else {
			error = "Unable to create a thread with the default pthread stack size: " + error;
		}
		return false;
	}

	while (lower_page < upper_page) {
		auto middle_page = lower_page + (upper_page - lower_page) / 2;
		auto probe_result = TryThreadStackSize(middle_page * page_size, error);
		if (probe_result == ThreadStackProbeResult::SUCCESS) {
			upper_page = middle_page;
		} else if (probe_result == ThreadStackProbeResult::TOO_SMALL) {
			lower_page = middle_page + 1;
		} else {
			return false;
		}
	}
	minimum_stack_size = lower_page * page_size;
	return true;
}

static bool SetThreadStackSize(size_t requested_stack_size, string &error) {
	pthread_attr_t default_attributes;
	auto result = pthread_getattr_default_np(&default_attributes);
	if (result != 0) {
		error = PthreadError("pthread_getattr_default_np", result);
		return false;
	}

	size_t default_stack_size;
	result = pthread_attr_getstacksize(&default_attributes, &default_stack_size);
	if (result != 0) {
		pthread_attr_destroy(&default_attributes);
		error = PthreadError("pthread_attr_getstacksize", result);
		return false;
	}
	auto page_size_result = sysconf(_SC_PAGESIZE);
	if (page_size_result <= 0) {
		pthread_attr_destroy(&default_attributes);
		error = "Failed to determine the system page size";
		return false;
	}
	auto page_size = static_cast<size_t>(page_size_result);
	size_t minimum_stack_size;
	if (!GetMinimumThreadStackSize(default_stack_size, page_size, minimum_stack_size, error)) {
		pthread_attr_destroy(&default_attributes);
		return false;
	}
	auto stack_overhead = minimum_stack_size - GLIBC_PTHREAD_STACK_MIN;
	if (requested_stack_size > std::numeric_limits<size_t>::max() - stack_overhead) {
		pthread_attr_destroy(&default_attributes);
		error = "--thread-stack-size is too large";
		return false;
	}
	auto configured_stack_size = requested_stack_size + stack_overhead;

	result = pthread_attr_setstacksize(&default_attributes, configured_stack_size);
	if (result == 0) {
		result = pthread_setattr_default_np(&default_attributes);
	}
	auto destroy_result = pthread_attr_destroy(&default_attributes);
	if (result != 0) {
		error = PthreadError("setting the default pthread stack size", result);
		return false;
	}
	if (destroy_result != 0) {
		error = PthreadError("pthread_attr_destroy", destroy_result);
		return false;
	}

	size_t observed_stack_size = 0;
	int probe_result = 0;
	try {
		std::thread probe([&]() {
			pthread_attr_t probe_attributes;
			probe_result = pthread_getattr_np(pthread_self(), &probe_attributes);
			if (probe_result != 0) {
				return;
			}
			probe_result = pthread_attr_getstacksize(&probe_attributes, &observed_stack_size);
			auto probe_destroy_result = pthread_attr_destroy(&probe_attributes);
			if (probe_result == 0) {
				probe_result = probe_destroy_result;
			}
		});
		probe.join();
	} catch (std::exception &ex) {
		error = string("Failed to create thread stack size probe: ") + ex.what();
		return false;
	}
	if (probe_result != 0) {
		error = PthreadError("reading the probe thread stack size", probe_result);
		return false;
	}

	auto effective_stack_size = observed_stack_size > stack_overhead ? observed_stack_size - stack_overhead : 0;
	auto difference = effective_stack_size > requested_stack_size ? effective_stack_size - requested_stack_size
	                                                              : requested_stack_size - effective_stack_size;
	if (difference > page_size) {
		error = "Thread stack size probe reported " + to_string(observed_stack_size) + " total bytes (" +
		        to_string(effective_stack_size) + " after glibc overhead) after requesting " +
		        to_string(requested_stack_size) + " bytes";
		return false;
	}
	return true;
}
#endif

static bool ConfigureThreadStackSize(int argc, char *argv[], string &error) {
	bool stack_size_specified = false;
#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
	size_t requested_stack_size = 0;
#endif
	for (int i = 1; i < argc; i++) {
		if (string(argv[i]) != "--thread-stack-size") {
			continue;
		}
		if (stack_size_specified) {
			error = "--thread-stack-size may only be specified once";
			return false;
		}
		if (++i >= argc) {
			error = "--thread-stack-size expected a size in bytes";
			return false;
		}
		size_t parsed_stack_size;
		if (!TryParseThreadStackSize(argv[i], parsed_stack_size)) {
			error = "--thread-stack-size expected a positive integer size in bytes";
			return false;
		}
#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
		requested_stack_size = parsed_stack_size;
#endif
		stack_size_specified = true;
	}
	if (!stack_size_specified) {
		return true;
	}

#ifdef DUCKDB_UNITTEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
	return SetThreadStackSize(requested_stack_size, error);
#else
	error = "--thread-stack-size is only supported on Linux with glibc 2.18 or newer";
	return false;
#endif
}

struct TempDirReclaimer {
	~TempDirReclaimer() {
		if (active) {
			DestroyTempDir(success);
		}
	}

	bool success = false;
	bool active = true;
};

#ifndef DUCKDB_WINDOWS
Catch::Totals worker_totals;

struct WorkerTotalsListener : public Catch::TestEventListenerBase {
	using TestEventListenerBase::TestEventListenerBase;

	void testRunEnded(Catch::TestRunStats const &stats) override {
		worker_totals = stats.totals;
		TestEventListenerBase::testRunEnded(stats);
	}
};

void AppendNumber(string &out, int64_t value) {
	out.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void AppendString(string &out, const string &value) {
	AppendNumber(out, static_cast<int64_t>(value.size()));
	out += value;
}

void AppendCounts(string &out, const Catch::Counts &counts) {
	AppendNumber(out, static_cast<int64_t>(counts.passed));
	AppendNumber(out, static_cast<int64_t>(counts.failed));
	AppendNumber(out, static_cast<int64_t>(counts.failedButOk));
}

bool ReadNumber(const string &in, idx_t &offset, int64_t &value) {
	if (offset + sizeof(value) > in.size()) {
		return false;
	}
	memcpy(&value, in.data() + offset, sizeof(value));
	offset += sizeof(value);
	return true;
}

bool ReadString(const string &in, idx_t &offset, string &value) {
	int64_t size;
	if (!ReadNumber(in, offset, size) || size < 0 || offset + static_cast<idx_t>(size) > in.size()) {
		return false;
	}
	value = in.substr(offset, static_cast<idx_t>(size));
	offset += static_cast<idx_t>(size);
	return true;
}

bool ReadCounts(const string &in, idx_t &offset, Catch::Counts &counts) {
	int64_t passed;
	int64_t failed;
	int64_t failed_but_ok;
	if (!ReadNumber(in, offset, passed) || !ReadNumber(in, offset, failed) || !ReadNumber(in, offset, failed_but_ok)) {
		return false;
	}
	counts.passed = static_cast<std::size_t>(passed);
	counts.failed = static_cast<std::size_t>(failed);
	counts.failedButOk = static_cast<std::size_t>(failed_but_ok);
	return true;
}

void WriteWorkerResult(int fd) {
	string out;
	AppendNumber(out, static_cast<int64_t>(worker_totals.skippedTests));
	AppendCounts(out, worker_totals.assertions);
	AppendCounts(out, worker_totals.testCases);
	AppendNumber(out, static_cast<int64_t>(worker_totals.skippedTestReasons.size()));
	for (auto &entry : worker_totals.skippedTestReasons) {
		AppendString(out, entry.first);
		AppendNumber(out, static_cast<int64_t>(entry.second));
	}
	AppendString(out, FailureSummary::GetFailureSummary());
	AppendString(out, SQLLogicTestRunner::GetSkipReasonSummary());
	idx_t written = 0;
	while (written < out.size()) {
		auto count = write(fd, out.data() + written, out.size() - written);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			return;
		}
		written += static_cast<idx_t>(count);
	}
}

struct ParallelTest {
	string name;
	pid_t pid = -1;
	int output_fd = -1;
	int error_fd = -1;
	int result_fd = -1;
	string output;
	string error;
	string result;
	bool launched = false;
	bool exited = false;
	int status = 0;
	std::chrono::steady_clock::time_point start;
	std::chrono::steady_clock::time_point end;
	bool body_started = false;
	bool body_ended = false;
	bool newline_owed = false;
	double took = -1;
	idx_t failure_headers = 0;
};

class ParallelRunner {
public:
	ParallelRunner(const std::vector<Catch::TestCase> &test_cases, idx_t jobs, std::shared_ptr<Catch::Config> config,
	               vector<string> unmatched)
	    : jobs(jobs), config(std::move(config)), unmatched(std::move(unmatched)) {
		for (auto &test_case : test_cases) {
			tests.emplace_back();
			tests.back().name = test_case.name;
		}
	}

	bool Run(int &exit_code, string &worker_spec, int &worker_result_fd) {
		Catch::ConsoleReporter reporter {Catch::ReporterConfig(config)};
		Catch::TestRunInfo run_info(config->name());
		reporter.testRunStarting(run_info);
		if (!tests.empty()) {
			Catch::renderTestProgress(0, static_cast<int>(tests.size()), tests[0].name);
		}
		vector<idx_t> launch_order;
		for (idx_t i = 0; i < tests.size(); i++) {
			launch_order.push_back(i);
		}
		std::stable_partition(launch_order.begin(), launch_order.end(),
		                      [&](idx_t index) { return StringUtil::EndsWith(tests[index].name, ".test_slow"); });
		idx_t next = 0;
		idx_t cursor = 0;
		while (cursor < tests.size()) {
			while (next < tests.size() && running.size() < jobs) {
				if (Launch(launch_order[next], worker_spec, worker_result_fd)) {
					return true;
				}
				next++;
			}
			Poll();
			Reap();
			while (cursor < tests.size() && tests[cursor].launched) {
				auto &test = tests[cursor];
				bool complete = test.exited && test.output_fd < 0 && test.error_fd < 0 && test.result_fd < 0;
				Display(test, complete);
				if (!complete) {
					break;
				}
				Finish(cursor);
				cursor++;
			}
			std::cout.flush();
			std::cerr.flush();
		}
		for (auto &name : unmatched) {
			reporter.noMatchingTestCases(name);
			totals.error = -1;
		}
		reporter.testRunEnded(Catch::TestRunStats(run_info, totals, false));
		if (config->warnAboutNoTests() && totals.error == -1) {
			exit_code = 2;
		} else {
			exit_code = std::min(255, std::max(totals.error, static_cast<int>(totals.assertions.failed)));
		}
		return false;
	}

	string SkipReasonSummary() const {
		std::ostringstream oss;
		for (auto &entry : skip_reasons) {
			oss << entry.first << ": " << entry.second << "\n";
		}
		return oss.str();
	}

private:
	bool Launch(idx_t index, string &worker_spec, int &worker_result_fd) {
		auto &test = tests[index];
		int output_pipe[2];
		int error_pipe[2];
		int result_pipe[2];
		if (pipe(output_pipe) != 0 || pipe(error_pipe) != 0 || pipe(result_pipe) != 0) {
			perror("pipe");
			exit(1);
		}
		std::cout.flush();
		std::cerr.flush();
		fflush(nullptr);
		auto pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(1);
		}
		if (pid == 0) {
			dup2(output_pipe[1], STDOUT_FILENO);
			dup2(error_pipe[1], STDERR_FILENO);
			close(output_pipe[0]);
			close(output_pipe[1]);
			close(error_pipe[0]);
			close(error_pipe[1]);
			close(result_pipe[0]);
			for (auto &other : tests) {
				for (auto fd : {other.output_fd, other.error_fd, other.result_fd}) {
					if (fd >= 0) {
						close(fd);
					}
				}
			}
			worker_result_fd = result_pipe[1];
			worker_spec = "\"";
			for (auto c : test.name) {
				if (c == ',' || c == '\\' || c == '"') {
					worker_spec += '\\';
				}
				worker_spec += c;
			}
			worker_spec += "\"";
			return true;
		}
		close(output_pipe[1]);
		close(error_pipe[1]);
		close(result_pipe[1]);
		fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
		fcntl(error_pipe[0], F_SETFL, O_NONBLOCK);
		fcntl(result_pipe[0], F_SETFL, O_NONBLOCK);
		test.pid = pid;
		test.output_fd = output_pipe[0];
		test.error_fd = error_pipe[0];
		test.result_fd = result_pipe[0];
		test.launched = true;
		test.start = std::chrono::steady_clock::now();
		running[pid] = index;
		return false;
	}

	static void Drain(int &fd, string &target) {
		char buffer[65536];
		while (fd >= 0) {
			auto count = read(fd, buffer, sizeof(buffer));
			if (count > 0) {
				target.append(buffer, static_cast<idx_t>(count));
			} else if (count == 0) {
				close(fd);
				fd = -1;
			} else if (errno != EINTR) {
				return;
			}
		}
	}

	void Poll() {
		vector<pollfd> fds;
		for (auto &test : tests) {
			for (auto fd : {test.output_fd, test.error_fd, test.result_fd}) {
				if (fd >= 0) {
					fds.push_back({fd, POLLIN, 0});
				}
			}
		}
		if (!fds.empty()) {
			poll(fds.data(), fds.size(), 100);
		}
		auto now = std::chrono::steady_clock::now();
		for (auto &test : tests) {
			Drain(test.output_fd, test.output);
			Drain(test.error_fd, test.error);
			Drain(test.result_fd, test.result);
			if (test.exited && now - test.end > std::chrono::seconds(1)) {
				for (auto fd : {&test.output_fd, &test.error_fd, &test.result_fd}) {
					if (*fd >= 0) {
						close(*fd);
						*fd = -1;
					}
				}
			}
		}
	}

	void Reap() {
		int status;
		pid_t pid;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
			auto entry = running.find(pid);
			if (entry == running.end()) {
				continue;
			}
			auto &test = tests[entry->second];
			test.exited = true;
			test.status = status;
			test.end = std::chrono::steady_clock::now();
			running.erase(entry);
		}
	}

	static bool IsRunInfo(const string &text, idx_t offset, idx_t &end) {
		const string rule(CATCH_CONFIG_CONSOLE_WIDTH - 1, '~');
		const string run_with = "Run with -? for options\n\n";
		if (text.compare(offset, rule.size() + 1, rule + "\n") != 0) {
			return false;
		}
		auto host_end = text.find('\n', offset + rule.size() + 1);
		if (host_end == string::npos || !StringUtil::EndsWith(text.substr(0, host_end), " host application.") ||
		    text.compare(host_end + 1, run_with.size(), run_with) != 0) {
			return false;
		}
		end = host_end + 1 + run_with.size() - 1;
		return true;
	}

	string Renumber(ParallelTest &test, const string &line) {
		idx_t digits = 0;
		while (digits < line.size() && StringUtil::CharacterIsDigit(line[digits])) {
			digits++;
		}
		auto suffix = ". " + test.name + ":";
		if (digits == 0 || line.compare(digits, suffix.size(), suffix) != 0) {
			return line;
		}
		auto number = std::stoull(line.substr(0, digits));
		test.failure_headers = std::max<idx_t>(test.failure_headers, number);
		return std::to_string(failures_before + number) + line.substr(digits);
	}

	string RenumberText(ParallelTest &test, const string &text) {
		string result;
		idx_t offset = 0;
		while (offset <= text.size()) {
			auto newline = text.find('\n', offset);
			if (newline == string::npos) {
				result += Renumber(test, text.substr(offset));
				break;
			}
			result += Renumber(test, text.substr(offset, newline - offset));
			result += '\n';
			offset = newline + 1;
		}
		return result;
	}

	void Emit(ParallelTest &test, const string &segment) {
		if (test.newline_owed) {
			std::cout << '\n';
		}
		std::cout << segment;
		test.newline_owed = true;
	}

	void Display(ParallelTest &test, bool complete) {
		idx_t error_offset = 0;
		while (error_offset < test.error.size()) {
			auto newline = test.error.find('\n', error_offset);
			if (newline == string::npos) {
				if (complete) {
					std::cerr << Renumber(test, test.error.substr(error_offset));
					error_offset = test.error.size();
				}
				break;
			}
			std::cerr << Renumber(test, test.error.substr(error_offset, newline - error_offset)) << '\n';
			error_offset = newline + 1;
		}
		test.error.erase(0, error_offset);
		if (!test.body_started) {
			auto marker = "[0/1] (0%): " + test.name;
			auto position = test.output.find(marker);
			if (position == string::npos) {
				if (!complete) {
					return;
				}
			} else {
				test.output.erase(0, position + marker.size());
			}
			test.body_started = true;
		}
		if (test.body_ended) {
			test.output.clear();
			return;
		}
		auto end_marker = "[1/1] (100%): " + test.name + " took ";
		idx_t offset = 0;
		while (offset < test.output.size()) {
			auto newline = test.output.find('\n', offset);
			if (newline == string::npos) {
				if (complete) {
					Emit(test, Renumber(test, test.output.substr(offset)));
					offset = test.output.size();
				}
				break;
			}
			auto line = test.output.substr(offset, newline - offset);
			if (StringUtil::StartsWith(line, end_marker)) {
				test.took = std::strtod(line.c_str() + end_marker.size(), nullptr);
				test.body_ended = true;
				offset = test.output.size();
				break;
			}
			if (line == string(CATCH_CONFIG_CONSOLE_WIDTH - 1, '~')) {
				idx_t run_info_end;
				if (IsRunInfo(test.output, offset, run_info_end)) {
					if (run_info_shown) {
						offset = run_info_end + 1;
						test.newline_owed = false;
						continue;
					}
					run_info_shown = true;
				} else if (!complete && test.output.find("\n\n", newline) == string::npos) {
					break;
				}
			}
			Emit(test, Renumber(test, line));
			offset = newline + 1;
		}
		test.output.erase(0, offset);
	}

	void Finish(idx_t index) {
		auto &test = tests[index];
		Catch::Totals test_totals;
		string failures;
		string skip_summary;
		if (ParseResult(test.result, test_totals, failures, skip_summary)) {
			totals += test_totals;
			if (!failures.empty()) {
				FailureSummary::Log(RenumberText(test, failures));
			}
			idx_t offset = 0;
			while (offset < skip_summary.size()) {
				auto newline = skip_summary.find('\n', offset);
				if (newline == string::npos) {
					newline = skip_summary.size();
				}
				auto line = skip_summary.substr(offset, newline - offset);
				auto separator = line.rfind(": ");
				if (separator != string::npos) {
					skip_reasons[line.substr(0, separator)] += std::stoull(line.substr(separator + 2));
				}
				offset = newline + 1;
			}
		} else {
			totals.testCases.failed++;
			totals.assertions.failed++;
			if (WIFSIGNALED(test.status)) {
				Emit(test, test.name + " was terminated by signal " + std::to_string(WTERMSIG(test.status)));
			} else {
				Emit(test, test.name + " exited with status " + std::to_string(WEXITSTATUS(test.status)) +
				               " before reporting its result");
			}
		}
		failures_before += test.failure_headers;
		auto elapsed = test.took >= 0 ? test.took : std::chrono::duration<double>(test.end - test.start).count();
		Catch::renderTestProgress(static_cast<int>(index + 1), static_cast<int>(tests.size()), test.name, elapsed);
		if (index + 1 == tests.size()) {
			std::cout << std::endl;
		}
		test.output.clear();
		test.result.clear();
	}

	static bool ParseResult(const string &result, Catch::Totals &test_totals, string &failures, string &skip_summary) {
		idx_t offset = 0;
		int64_t skipped_tests;
		int64_t reason_count;
		if (!ReadNumber(result, offset, skipped_tests) || !ReadCounts(result, offset, test_totals.assertions) ||
		    !ReadCounts(result, offset, test_totals.testCases) || !ReadNumber(result, offset, reason_count)) {
			return false;
		}
		test_totals.skippedTests = static_cast<std::size_t>(skipped_tests);
		for (int64_t i = 0; i < reason_count; i++) {
			string reason;
			int64_t count;
			if (!ReadString(result, offset, reason) || !ReadNumber(result, offset, count)) {
				return false;
			}
			test_totals.skippedTestReasons[reason] = static_cast<std::size_t>(count);
		}
		return ReadString(result, offset, failures) && ReadString(result, offset, skip_summary);
	}

	idx_t jobs;
	std::shared_ptr<Catch::Config> config;
	vector<string> unmatched;
	vector<ParallelTest> tests;
	unordered_map<pid_t, idx_t> running;
	Catch::Totals totals;
	std::map<string, idx_t> skip_reasons;
	idx_t failures_before = 0;
	bool run_info_shown = false;
};
#endif

} // namespace

#ifndef DUCKDB_WINDOWS
CATCH_REGISTER_LISTENER(WorkerTotalsListener)
#endif

static bool IsSQLLogicTestFile(const string &path) {
	return StringUtil::EndsWith(path, ".test") || StringUtil::EndsWith(path, ".test_slow") ||
	       StringUtil::EndsWith(path, ".test_coverage");
}

static bool TryReadExactSQLLogicTestFilter(const vector<string> &input_files, vector<string> &test_paths,
                                           string &error) {
	if (input_files.empty()) {
		return false;
	}
	auto fs = FileSystem::CreateLocal();
	unordered_set<string> seen_paths;
	for (auto &input_file : input_files) {
		std::ifstream file(input_file.c_str());
		if (!file.is_open()) {
			return false;
		}
		string line;
		while (std::getline(file, line)) {
			StringUtil::Trim(line);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (!IsSQLLogicTestFile(line)) {
				return false;
			}
			if (!fs->FileExists(line)) {
				error = "Unable to find sqllogictest file from -f/--input-file: " + line;
				return true;
			}
			if (seen_paths.insert(line).second) {
				test_paths.push_back(line);
			}
		}
	}
	return !test_paths.empty();
}

int main(int argc_in, char *argv[]) {
	string thread_stack_error;
	if (!ConfigureThreadStackSize(argc_in, argv, thread_stack_error)) {
		fprintf(stderr, "%s\n", thread_stack_error.c_str());
		return 1;
	}

	string test_directory = DUCKDB_ROOT_DIRECTORY;

	// route the sqllogictest runner's verdicts into the Catch session
	static CatchTestReporter catch_reporter;
	TestReporter::Set(catch_reporter);
	// debug_fs is linked into this binary; hand it to every database the runner creates
	SetStaticExtensionLoader([](DuckDB &db) { db.LoadStaticExtension<DebugFsExtension>(); });

	auto &test_config = TestConfiguration::Get();
	try {
		// Applies every DUCKDB_TEST_<NAME> fallback, so a bad env value surfaces here.
		test_config.Initialize();
	} catch (std::exception &ex) {
		fprintf(stderr, "%s\n", ex.what());
		return 1;
	}
	bool keep_home = false;
	bool use_stdin = false;
	idx_t jobs = 1;
	vector<string> input_files;
	unordered_set<idx_t> input_file_arg_indices;

	// The --temp-dir-* family, --run-id and --env-passthrough live in TestConfiguration's option table,
	// so ParseArgument handles them below and Initialize() already applied their env fallbacks. What
	// remains here is what that table cannot express.
	idx_t argc = NumericCast<idx_t>(argc_in);
	int new_argc = 0;
	auto new_argv = duckdb::unique_ptr<char *[]>(new char *[argc]);
	for (idx_t i = 0; i < argc; i++) {
		string argument(argv[i]);
		if (argument == "--test-dir") {
			test_directory = string(argv[++i]);
		} else if (argument == "--require") {
			AddRequire(string(argv[++i]));
		} else if (argument == "--emit-on-skip") {
			SetEmitOnSkip(true);
		} else if (argument == "--keep-home") {
			keep_home = true;
		} else if (argument == "--stdin") {
			use_stdin = true;
		} else if (argument == "--jobs") {
			jobs = std::stoull(argv[++i]);
		} else if (argument == "--emit-test-events") {
			SetEmitTestEvents(true);
		} else if (argument == "--thread-stack-size") {
			i++;
		} else {
			try {
				if (!test_config.ParseArgument(argument, argc, argv, i)) {
					if ((argument == "-f" || argument == "--input-file") && i + 1 < argc) {
						input_files.push_back(TestMakeAbsolute(argv[i + 1], TestGetCurrentDirectory()));
						input_file_arg_indices.insert(new_argc);
						input_file_arg_indices.insert(new_argc + 1);
					}
					new_argv[new_argc] = argv[i];
					new_argc++;
				}
			} catch (std::exception &ex) {
				fprintf(stderr, "%s\n", ex.what());
				return 1;
			}
		}
	}

	// Before any temp-dir prep or test runs: a named-but-absent var kills the whole invocation, unlike
	// require-env's per-test skip.
	string env_error;
	if (!ValidateEnvPassthrough(env_error) || !test_config.ValidateTestEnv(env_error)) {
		fprintf(stderr, "%s\n", env_error.c_str());
		return 1;
	}

	// Keep input filenames anchored to the invocation directory, including Catch's filter fallback.
	idx_t input_file_index = 0;
	for (int i = 0; i < new_argc; i++) {
		if (input_file_arg_indices.find(i) != input_file_arg_indices.end()) {
			new_argv[++i] = &input_files[input_file_index++][0];
		}
	}
	test_config.ChangeWorkingDirectory(test_directory);

	vector<string> exact_sqllogic_tests;
	string exact_sqllogic_error;
	bool exact_sqllogic_filter =
	    TryReadExactSQLLogicTestFilter(input_files, exact_sqllogic_tests, exact_sqllogic_error);
	if (!exact_sqllogic_error.empty()) {
		fprintf(stderr, "%s\n", exact_sqllogic_error.c_str());
		return 1;
	}
	string exact_sqllogic_test_filter = "*";
	if (exact_sqllogic_filter) {
		int filtered_argc = 0;
		for (int i = 0; i < new_argc; i++) {
			if (input_file_arg_indices.find(i) != input_file_arg_indices.end()) {
				continue;
			}
			new_argv[filtered_argc++] = new_argv[i];
		}
		new_argv[filtered_argc++] = const_cast<char *>(exact_sqllogic_test_filter.c_str());
		new_argc = filtered_argc;
	}

	// Resolve + provision $BASE/[RUN_ID] (the TEST_ID level is materialized later, on the
	// per-test path, once a test name is known).
	string prep_error;
	if (!PrepareTempDir(prep_error)) {
		fprintf(stderr, "Failed to prepare temp directory: %s\n", prep_error.c_str());
		return 1;
	}
	TempDirReclaimer temp_dir_reclaimer;

	// Capture env now that all --temp-dir-* context (base/run-id/create) is final; must run
	// after PrepareTempDir so TEMP_DIR reflects the materialized run root.
	test_config.UpdateEnvironment();

	string data_dir_error;
	if (!test_config.ValidateDataDirs(data_dir_error)) {
		fprintf(stderr, "%s\n", data_dir_error.c_str());
		return 1;
	}

	// Set ONCE per invocation, never per-test, so ~/.duckdb (extensions, secrets) is isolated without
	// landing inside a {TEST_DIR} a test whitelists. Absolute because a relative home would shift under
	// any test chdir.
	string home_dir = TestMakeAbsolute(GetTempDirHome(), TestGetCurrentDirectory());

	if (!keep_home) {
#ifdef DUCKDB_WINDOWS
		if (_putenv_s("USERPROFILE", home_dir.c_str()) != 0) {
			fprintf(stderr, "Failed to set USERPROFILE environment variable\n");
			return 1;
		}
#else
		if (setenv("HOME", home_dir.c_str(), 1) != 0) {
			fprintf(stderr, "Failed to set HOME environment variable\n");
			return 1;
		}
#endif
	}

	if (use_stdin || exact_sqllogic_filter || test_config.GetSkipCompiledTests()) {
		Catch::getMutableRegistryHub().clearTests();
	}
	if (use_stdin) {
		RegisterSqllogictestStdin();
	} else if (exact_sqllogic_filter) {
		RegisterSqllogictests(exact_sqllogic_tests);
	} else {
		RegisterSqllogictests();
	}

	string worker_spec;
	int worker_result_fd = -1;
	bool ran_in_parallel = false;
	string parallel_skip_reason_summary;
	int result = 0;
#ifndef DUCKDB_WINDOWS
	if (jobs > 1 && !use_stdin) {
		Catch::ConfigData data;
		auto parsed = Catch::makeCommandLineParser(data).parse(Catch::clara::Args(new_argc, new_argv.get()));
		if (parsed && !data.showHelp && !data.libIdentify && !data.listTests && !data.listTestNamesOnly &&
		    !data.listTags && !data.listReporters && !data.printFailingTests) {
			auto config = std::make_shared<Catch::Config>(data);
			auto &all_tests = Catch::getAllTestCasesSorted(*config);
			vector<string> unmatched;
			for (auto &match : config->testSpec().matchesByFilter(all_tests, *config)) {
				if (match.tests.empty()) {
					unmatched.push_back(match.name);
				}
			}
			auto tests = Catch::filterTests(all_tests, config->testSpec(), *config);
			ParallelRunner runner(tests, jobs, config, std::move(unmatched));
			if (runner.Run(result, worker_spec, worker_result_fd)) {
				temp_dir_reclaimer.active = false;
				new_argv[1] = &worker_spec[0];
				new_argc = 2;
			} else {
				ran_in_parallel = true;
				parallel_skip_reason_summary = runner.SkipReasonSummary();
			}
		}
	}
#endif

	if (!ran_in_parallel) {
		result = Catch::Session().run(new_argc, new_argv.get());
	}
#ifndef DUCKDB_WINDOWS
	if (worker_result_fd >= 0) {
		WriteWorkerResult(worker_result_fd);
		close(worker_result_fd);
		return result;
	}
#endif

	std::string failures_summary = FailureSummary::GetFailureSummary();
	if (!failures_summary.empty()) {
		auto description = test_config.GetDescription();
		if (!description.empty()) {
			std::cerr << "\n====================================================" << std::endl;
			std::cerr << "====================  TEST INFO  ===================" << std::endl;
			std::cerr << "====================================================\n" << std::endl;
			std::cerr << description << std::endl;
		}
		std::cerr << "\n====================================================" << std::endl;
		std::cerr << "================  FAILURES SUMMARY  ================" << std::endl;
		std::cerr << "====================================================\n" << std::endl;
		std::cerr << failures_summary;
	}
	std::string skip_reason_summary =
	    ran_in_parallel ? parallel_skip_reason_summary : SQLLogicTestRunner::GetSkipReasonSummary();
	if (!skip_reason_summary.empty()) {
		std::cerr << "\n"
		          << "Skipped tests for the following reasons:" << std::endl;
		std::cerr << skip_reason_summary;
	}

	// Execute the run-id-level destroy disposition ($BASE/[RUN_ID]); pass/fail-aware, recursive.
	temp_dir_reclaimer.success = result == 0;

	return result;
}
