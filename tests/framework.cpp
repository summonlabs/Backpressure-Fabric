// Backpressure Fabric - test harness implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bpfab_test {
namespace {

std::mutex g_mutex;
std::atomic<std::uint64_t> g_scratch_counter{0};
std::vector<std::string> g_scratch_directories;

std::string describe_code(backpressure::ErrorCode code) {
  return std::string(backpressure::to_string(code));
}

}  // namespace

std::string describe(const backpressure::Status& status) {
  std::string out = describe_code(status.code());
  if (!status.context().empty()) {
    out += " [";
    out += std::string(status.context());
    out += "]";
  }
  if (status.detail() != 0) {
    out += " (";
    out += std::to_string(status.detail());
    out += ")";
  }
  return out;
}

std::string hex_u64(std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void register_test(const char* suite, const char* name, void (*fn)()) {
  TestCase test;
  test.suite = suite;
  test.name = name;
  test.fn = fn;
  registry().push_back(std::move(test));
}

Context& context() {
  static Context ctx;
  return ctx;
}

void Context::fail(const char* file, int line, const std::string& message) {
  ++failures;
  std::ostringstream oss;
  oss << current << " | " << file << ":" << line << " | " << message;
  last_failure = oss.str();
  std::fprintf(stderr, "FAIL %s\n", last_failure.c_str());
  std::fflush(stderr);
}

void Context::require_failed(const char* file, int line, const std::string& message) {
  fail(file, line, message);
  throw TestAbort{};
}

std::string make_scratch_directory(const std::string& label) {
  const std::uint64_t counter = g_scratch_counter.fetch_add(1);
  std::error_code ec;
  const std::filesystem::path base =
      std::filesystem::temp_directory_path(ec) / "bpfab-tests";
  std::filesystem::create_directories(base, ec);
  const std::filesystem::path directory =
      base / (label + "-" + std::to_string(counter));
  std::filesystem::remove_all(directory, ec);
  std::filesystem::create_directories(directory, ec);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_scratch_directories.push_back(directory.string());
  return directory.string();
}

void cleanup_scratch() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::error_code ec;
  for (const std::string& directory : g_scratch_directories) {
    std::filesystem::remove_all(std::filesystem::path(directory), ec);
  }
  g_scratch_directories.clear();
  std::error_code inner;
  const std::filesystem::path base =
      std::filesystem::temp_directory_path(inner) / "bpfab-tests";
  std::filesystem::remove_all(base, inner);
}

int run_all(int argc, char** argv) {
  std::vector<std::string> filters;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--list") {
      list_only = true;
    } else if (arg.rfind("--filter=", 0) == 0) {
      filters.push_back(arg.substr(9));
    }
  }

  std::vector<TestCase> selected;
  for (const TestCase& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (filters.empty()) {
      selected.push_back(test);
      continue;
    }
    for (const std::string& filter : filters) {
      if (full.find(filter) != std::string::npos) {
        selected.push_back(test);
        break;
      }
    }
  }

  if (list_only) {
    for (const TestCase& test : selected) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::printf("running %zu test(s)\n", selected.size());
  std::fflush(stdout);
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : selected) {
    Context& ctx = context();
    ctx.current = test.suite + "." + test.name;
    const int before = ctx.failures;
    try {
      test.fn();
    } catch (const TestAbort&) {
      // Failure already recorded.
    } catch (const std::exception& error) {
      ctx.fail(__FILE__, __LINE__, std::string("unhandled exception: ") + error.what());
    } catch (...) {
      ctx.fail(__FILE__, __LINE__, "unhandled non-standard exception");
    }
    if (ctx.failures == before) {
      ++passed;
      std::printf("PASS %s\n", ctx.current.c_str());
    } else {
      ++failed;
      std::printf("FAIL %s\n", ctx.current.c_str());
    }
    std::fflush(stdout);
  }

  cleanup_scratch();
  std::printf("\n%zu passed, %zu failed, %d checks, %d failures\n", passed, failed,
              context().checks, context().failures);
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

}  // namespace bpfab_test
