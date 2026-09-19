#pragma once

// Backpressure Fabric - minimal deterministic test harness.
//
// No test in this project uses a timeout. A test that does not terminate is a
// defect to be diagnosed, not something to be masked by a watchdog.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

#include "backpressure/core/status.hpp"

namespace bpfab_test {

struct TestCase {
  std::string suite;
  std::string name;
  void (*fn)();
};

std::vector<TestCase>& registry();
void register_test(const char* suite, const char* name, void (*fn)());

/// Accept either a Status or a Result<T> wherever a status is expected.
///
/// Returns by value on purpose: binding a reference to result.status() of a
/// temporary Result would leave a dangling reference for the rest of the
/// statement.
[[nodiscard]] inline backpressure::Status as_status(const backpressure::Status& status) noexcept {
  return status;
}

template <class T>
[[nodiscard]] inline backpressure::Status as_status(
    const backpressure::Result<T>& result) noexcept {
  return result.status();
}

/// Thrown by BPFAB_REQUIRE to abandon the current test.
struct TestAbort {};

class Context {
 public:
  void fail(const char* file, int line, const std::string& message);
  [[noreturn]] void require_failed(const char* file, int line, const std::string& message);

  int failures = 0;
  int checks = 0;
  std::string current;
  std::string last_failure;
};

Context& context();

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) { register_test(suite, name, fn); }
};

int run_all(int argc, char** argv);

/// Create a unique scratch directory that is removed when the process exits
/// normally. Used by tests that need real files.
std::string make_scratch_directory(const std::string& label);
void cleanup_scratch();

/// Deterministic helpers shared by the suites.
std::string describe(const backpressure::Status& status);
std::string hex_u64(std::uint64_t value);

}  // namespace bpfab_test

#define BPFAB_TEST(suite, name)                                                            static void bpfab_test_##suite##_##name();                                               static const ::bpfab_test::Registrar bpfab_reg_##suite##_##name(                             #suite, #name, &bpfab_test_##suite##_##name);                                        static void bpfab_test_##suite##_##name()

#define BPFAB_CHECK(expr)                                                                  do {                                                                                       ++::bpfab_test::context().checks;                                                        if (!(expr)) {                                                                             ::bpfab_test::context().fail(__FILE__, __LINE__, "CHECK failed: " #expr);              }                                                                                      } while (false)

#define BPFAB_REQUIRE(expr)                                                                do {                                                                                       ++::bpfab_test::context().checks;                                                        if (!(expr)) {                                                                             ::bpfab_test::context().require_failed(__FILE__, __LINE__, "REQUIRE failed: " #expr);     }                                                                                      } while (false)

#define BPFAB_CHECK_MSG(expr, message)                                                     do {                                                                                       ++::bpfab_test::context().checks;                                                        if (!(expr)) {                                                                             std::ostringstream bpfab_oss_;                                                           bpfab_oss_ << "CHECK failed: " #expr " | " << (message);                                 ::bpfab_test::context().fail(__FILE__, __LINE__, bpfab_oss_.str());                    }                                                                                      } while (false)

/// Evaluate a Status-producing expression and require success.
#define BPFAB_REQUIRE_OK(expr)                                                             do {                                                                                       ++::bpfab_test::context().checks;                                                        const ::backpressure::Status bpfab_st_ = ::bpfab_test::as_status(expr);                                         if (!bpfab_st_.ok()) {                                                                     ::bpfab_test::context().require_failed(                                                      __FILE__, __LINE__,                                                                      std::string("expected success: " #expr " -> ") +                                             ::bpfab_test::describe(bpfab_st_));                                            }                                                                                      } while (false)

/// Evaluate a Status-producing expression and require an exact error code.
#define BPFAB_REQUIRE_CODE(expr, expected)                                                 do {                                                                                       ++::bpfab_test::context().checks;                                                        const ::backpressure::Status bpfab_st_ = ::bpfab_test::as_status(expr);                                         if (bpfab_st_.code() != (expected)) {                                                      ::bpfab_test::context().require_failed(                                                      __FILE__, __LINE__,                                                                      std::string("expected ") + ::backpressure::to_string(expected) + " from " #expr               " but got " + ::bpfab_test::describe(bpfab_st_));                              }                                                                                      } while (false)

/// Require a Result<T> to be ok and bind it to a name.
#define BPFAB_REQUIRE_RESULT(name, expr)                                                   auto bpfab_res_##name = (expr);                                                          ++::bpfab_test::context().checks;                                                        if (!bpfab_res_##name.ok()) {                                                              ::bpfab_test::context().require_failed(                                                      __FILE__, __LINE__,                                                                      std::string("expected success: " #expr " -> ") +                                             ::bpfab_test::describe(bpfab_res_##name.status()));                            }                                                                                        auto& name = bpfab_res_##name.value()