// Backpressure Fabric - measurement clock.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/core/clock.hpp"

#include <chrono>

namespace backpressure {

std::uint64_t steady_nanos() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return nanos < 0 ? 0u : static_cast<std::uint64_t>(nanos);
}

}  // namespace backpressure
