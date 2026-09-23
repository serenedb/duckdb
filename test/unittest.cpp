#define CATCH_CONFIG_RUNNER
#include "catch.hpp"
#include <stdlib.h>

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "sqlite/sqllogic_test_logger.hpp"
#include "sqlite/sqllogic_test_runner.hpp"
#include "test_helpers.hpp"
#include "test_config.hpp"

#ifndef DUCKDB_WINDOWS
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace duckdb;

int main(int argc_in, char *argv[]) {
	duckdb::unique_ptr<FileSystem> fs = FileSystem::CreateLocal();
	string test_directory = DUCKDB_ROOT_DIRECTORY;

	auto &test_config = TestConfiguration::Get();
	test_config.Initialize();
	bool keep_home = false;
	bool use_stdin = false;
	idx_t jobs = 1;

	idx_t argc = NumericCast<idx_t>(argc_in);
	int new_argc = 0;
	auto new_argv = duckdb::unique_ptr<char *[]>(new char *[argc]);
	for (idx_t i = 0; i < argc; i++) {
		string argument(argv[i]);
		if (argument == "--test-dir") {
			test_directory = string(argv[++i]);
		} else if (argument == "--test-temp-dir") {
			SetDeleteTestPath(false);
			auto test_dir = string(argv[++i]);
			if (fs->DirectoryExists(test_dir)) {
				fprintf(stderr, "--test-temp-dir cannot point to a directory that already exists (%s)\n",
				        test_dir.c_str());
				return 1;
			}
			SetTestDirectory(test_dir);
		} else if (argument == "--require") {
			AddRequire(string(argv[++i]));
		} else if (argument == "--keep-home") {
			keep_home = true;
		} else if (argument == "--stdin") {
			use_stdin = true;
		} else if (argument == "--jobs") {
			jobs = std::stoull(argv[++i]);
		} else {
			try {
				if (!test_config.ParseArgument(argument, argc, argv, i)) {
					new_argv[new_argc] = argv[i];
					new_argc++;
				}
			} catch (std::exception &ex) {
				fprintf(stderr, "%s\n", ex.what());
				return 1;
			}
		}
	}
	test_config.ChangeWorkingDirectory(test_directory);

	if (use_stdin || test_config.GetSkipCompiledTests()) {
		Catch::getMutableRegistryHub().clearTests();
	}
	if (use_stdin) {
		RegisterSqllogictestStdin();
	} else {
		RegisterSqllogictests();
	}

	string worker_spec;
#ifndef DUCKDB_WINDOWS
	if (jobs > 1) {
		Catch::ConfigData data;
		data.testsOrTags.assign(new_argv.get() + 1, new_argv.get() + new_argc);
		Catch::Config config(data);
		auto &all_tests = Catch::getAllTestCasesSorted(config);
		for (auto &match : config.testSpec().matchesByFilter(all_tests, config)) {
			if (match.tests.empty()) {
				std::cout << "No test cases matched '" << match.name << "'" << std::endl;
			}
		}
		auto tests = Catch::filterTests(all_tests, config.testSpec(), config);
		std::stable_partition(tests.begin(), tests.end(),
		                      [](const Catch::TestCase &test) { return StringUtil::EndsWith(test.name, ".test_slow"); });
		jobs = MinValue<idx_t>(jobs, tests.size());
		idx_t worker = 0;
		for (; worker < jobs; worker++) {
			auto pid = fork();
			if (pid < 0) {
				perror("fork");
				return 1;
			}
			if (pid == 0) {
				break;
			}
		}
		if (worker == jobs) {
			idx_t failed_workers = 0;
			int status;
			while (wait(&status) > 0) {
				if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
					failed_workers++;
				}
			}
			std::cout << "\n===============================================================================" << std::endl;
			if (failed_workers == 0) {
				std::cout << "All tests passed (" << tests.size() << " test cases)" << std::endl;
				return 0;
			}
			std::cout << "test cases: " << tests.size() << " | " << failed_workers << " of " << jobs
			          << " workers failed" << std::endl;
			return 1;
		}
		for (idx_t i = worker; i < tests.size(); i += jobs) {
			worker_spec += (worker_spec.empty() ? "\"" : ",\"") + tests[i].name + "\"";
		}
		new_argv[1] = &worker_spec[0];
		new_argc = 2;
	}
#endif

	// delete the testing directory if it exists
	auto dir = TestCreatePath("");
	try {
		TestDeleteDirectory(dir);
		// create the empty testing directory
		TestCreateDirectory(dir);
	} catch (std::exception &ex) {
		fprintf(stderr, "Failed to create testing directory \"%s\": %s\n", dir.c_str(), ex.what());
		return 1;
	}

	// Override the home dir so the .duckdb dir is isolated per test process.
	if (!keep_home) {
#ifdef DUCKDB_WINDOWS
		if (_putenv_s("USERPROFILE", dir.c_str()) != 0) {
			fprintf(stderr, "Failed to set USERPROFILE environment variable\n");
			return 1;
		}
#else
		if (setenv("HOME", dir.c_str(), 1) != 0) {
			fprintf(stderr, "Failed to set HOME environment variable\n");
			return 1;
		}
#endif
	}

	int result = Catch::Session().run(new_argc, new_argv.get());

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
	std::string skip_reason_summary = SQLLogicTestRunner::GetSkipReasonSummary();
	if (!skip_reason_summary.empty()) {
		std::cerr << "\n"
		          << "Skipped tests for the following reasons:" << std::endl;
		std::cerr << skip_reason_summary;
	}

	if (DeleteTestPath()) {
		TestDeleteDirectory(dir);
	}

	return result;
}
