// Backpressure Fabric - test entry point.
//
// The same binary is re-executed as a child process by the multiprocess suite,
// which is why a role switch is handled before the test registry runs.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <string>

#include "framework.hpp"

/// Defined in test_multiprocess.cpp.
int bpfab_multiprocess_child_main(int argc, char** argv);

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--role=", 7) == 0) {
      return bpfab_multiprocess_child_main(argc, argv);
    }
  }
  return bpfab_test::run_all(argc, argv);
}
