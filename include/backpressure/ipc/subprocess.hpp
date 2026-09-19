#pragma once

// Backpressure Fabric - real OS process management.
//
// Multiprocess validation uses real child processes launched by this runtime,
// over real framed transport. Nothing here simulates a second node.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include "backpressure/core/status.hpp"

namespace backpressure {

struct ProcessHandle {
  void* native = nullptr;
  std::uint32_t pid = 0;

  [[nodiscard]] bool valid() const noexcept { return native != nullptr; }
};

struct SpawnOptions {
  std::string working_directory{};
  /// When true the child inherits this process's stdio. Piped stdio is not
  /// provided: the fabric never depends on capturing child output.
  bool inherit_stdio = true;
};

/// Launch \p argv[0] with the given argument vector. argv[0] is resolved as a
/// path; no shell is involved, so arguments are passed verbatim.
[[nodiscard]] Result<ProcessHandle> spawn_process(const std::vector<std::string>& argv,
                                                  const SpawnOptions& options);

/// Non-blocking liveness check.
[[nodiscard]] Result<bool> process_running(ProcessHandle& handle);

/// Blocking wait for termination. Returns the exit code.
[[nodiscard]] Result<int> wait_process(ProcessHandle& handle);

/// Force termination and wait. Used by tests that prove hard-kill fencing.
[[nodiscard]] Status terminate_process(ProcessHandle& handle);

/// Release the handle without terminating the child.
void close_process(ProcessHandle& handle) noexcept;

/// Absolute path of the running executable image.
[[nodiscard]] Result<std::string> current_executable_path();

/// Absolute path of the running executable's directory.
[[nodiscard]] Result<std::string> current_executable_directory();

/// Absolute path of the current working directory.
[[nodiscard]] Result<std::string> current_working_directory();

}  // namespace backpressure
