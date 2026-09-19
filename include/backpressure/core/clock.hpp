#pragma once

// Backpressure Fabric - logical and measurement clocks.
//
// Authoritative decisions never consult the wall clock. They are bound to a
// logical tick supplied by the caller, which is what makes decisions
// deterministic and reproducible. The steady clock exists only to time
// benchmarks.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "backpressure/core/checked.hpp"
#include "backpressure/core/status.hpp"

namespace backpressure {

/// Monotone logical time unit. Its meaning is defined by the embedding system.
using Tick = std::uint64_t;

inline constexpr Tick kNoTick = 0;

/// Monotone logical clock with checked advancement.
class LogicalClock {
 public:
  constexpr LogicalClock() noexcept = default;
  explicit constexpr LogicalClock(Tick start) noexcept : now_(start) {}

  [[nodiscard]] constexpr Tick now() const noexcept { return now_; }

  /// Advance by delta, refusing to wrap.
  [[nodiscard]] Result<Tick> advance(Tick delta) {
    if (checked::add_overflows_u64(now_, delta)) {
      return fail<Tick>(ErrorCode::Overflow, "logical clock", now_);
    }
    now_ += delta;
    return Result<Tick>(now_);
  }

  /// Move forward to an absolute tick; moving backwards is refused so that
  /// authoritative state can never be rewound.
  [[nodiscard]] Status set(Tick value) {
    if (value < now_) {
      return Status::error(ErrorCode::InvalidArgument, "logical clock regression", value);
    }
    now_ = value;
    return Status::success();
  }

 private:
  Tick now_ = kNoTick;
};

/// Wall clock in nanoseconds. Benchmarks only; never used for authority.
[[nodiscard]] std::uint64_t steady_nanos() noexcept;

}  // namespace backpressure
