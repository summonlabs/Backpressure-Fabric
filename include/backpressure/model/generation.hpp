#pragma once

// Backpressure Fabric - generations, epochs and incarnations.
//
// Every authoritative statement in the fabric is bound to the exact generation
// of the thing it describes, the epoch under which the authority was granted,
// and the incarnation of the process that asserted it. Evidence that cannot be
// bound is not evidence.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <limits>
#include <string_view>

#include "backpressure/core/checked.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"

namespace backpressure {

/// Monotone revision of a resource definition, a dependency relationship or a
/// policy. Zero means UNKNOWN: an unbound value that can never be matched.
class Generation {
 public:
  using rep_type = std::uint64_t;
  static constexpr rep_type kUnknown = 0;

  constexpr Generation() noexcept = default;

  [[nodiscard]] static constexpr Generation unknown() noexcept { return Generation{}; }
  [[nodiscard]] static constexpr Generation initial() noexcept { return Generation(1); }

  [[nodiscard]] static Result<Generation> from_u64(std::uint64_t raw) {
    return Result<Generation>(Generation(raw));
  }

  /// Unchecked construction for values derived from an in-process digest.
  [[nodiscard]] static constexpr Generation from_raw(std::uint64_t raw) noexcept {
    return Generation(raw == kUnknown ? 1u : raw);
  }

  [[nodiscard]] constexpr rep_type value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool known() const noexcept { return value_ != kUnknown; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return known(); }

  /// Next generation. Refuses to wrap at the representable maximum.
  [[nodiscard]] Result<Generation> next() const {
    if (value_ == std::numeric_limits<rep_type>::max()) {
      return fail<Generation>(ErrorCode::Overflow, "generation exhausted", value_);
    }
    return Result<Generation>(Generation(value_ + 1u));
  }

  friend constexpr bool operator==(Generation a, Generation b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(Generation a, Generation b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Generation a, Generation b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(Generation a, Generation b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>(Generation a, Generation b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator>=(Generation a, Generation b) noexcept {
    return a.value_ >= b.value_;
  }

 private:
  explicit constexpr Generation(rep_type value) noexcept : value_(value) {}
  rep_type value_ = kUnknown;
};

/// Fabric-wide authority epoch. Advancing the epoch fences every statement made
/// under a previous epoch. Zero means "no epoch": never valid authority.
class Epoch {
 public:
  using rep_type = std::uint64_t;
  static constexpr rep_type kNone = 0;

  constexpr Epoch() noexcept = default;
  explicit constexpr Epoch(rep_type value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Epoch none() noexcept { return Epoch{}; }

  [[nodiscard]] static Result<Epoch> from_u64(std::uint64_t raw) {
    return Result<Epoch>(Epoch(raw));
  }

  /// Unchecked construction for values read back from an integrity-checked
  /// journal record.
  [[nodiscard]] static constexpr Epoch from_raw(std::uint64_t raw) noexcept {
    return Epoch(raw);
  }

  [[nodiscard]] constexpr rep_type value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool known() const noexcept { return value_ != kNone; }

  [[nodiscard]] Result<Epoch> next() const {
    if (value_ == std::numeric_limits<rep_type>::max()) {
      return fail<Epoch>(ErrorCode::Overflow, "epoch exhausted", value_);
    }
    return Result<Epoch>(Epoch(value_ + 1u));
  }

  friend constexpr bool operator==(Epoch a, Epoch b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Epoch a, Epoch b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Epoch a, Epoch b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator<=(Epoch a, Epoch b) noexcept { return a.value_ <= b.value_; }
  friend constexpr bool operator>(Epoch a, Epoch b) noexcept { return a.value_ > b.value_; }
  friend constexpr bool operator>=(Epoch a, Epoch b) noexcept { return a.value_ >= b.value_; }

 private:
  rep_type value_ = kNone;
};

/// Freshly minted per process boot. Two processes never share a boot id, which
/// is what makes cross-restart evidence distinguishable from live evidence.
struct BootId {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  [[nodiscard]] static BootId from_seed(std::uint64_t seed, std::uint32_t pid,
                                        std::uint64_t nonce) noexcept {
    DigestBuilder b;
    b.domain(0xB0u);
    b.update_u64(seed);
    b.update_u32(pid);
    b.update_u64(nonce);
    const Digest128 d = b.finish();
    return BootId{d.hi | 1u, d.lo};
  }
  [[nodiscard]] static BootId from_raw(std::uint64_t hi, std::uint64_t lo) noexcept {
    return BootId{hi, lo};
  }
  [[nodiscard]] constexpr bool valid() const noexcept { return hi != 0 || lo != 0; }

  friend constexpr bool operator==(const BootId& a, const BootId& b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
  }
  friend constexpr bool operator!=(const BootId& a, const BootId& b) noexcept {
    return !(a == b);
  }
};

/// Identity of one running participant: which boot, which OS process, which
/// logical participant index within that process, and a monotone sequence
/// number that advances on every incarnation change.
struct Incarnation {
  BootId boot{};
  std::uint32_t pid = 0;
  std::uint32_t index = 0;
  std::uint64_t seq = 0;

  [[nodiscard]] static Incarnation mint(BootId boot, std::uint32_t pid,
                                        std::uint32_t index) noexcept {
    return Incarnation{boot, pid, index, 1};
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    return boot.valid() && seq != 0;
  }

  /// Advance to the next incarnation of the same participant (new worker,
  /// reconnected publisher). Never resets the boot id.
  [[nodiscard]] Result<Incarnation> reincarnate() const {
    if (seq == std::numeric_limits<std::uint64_t>::max()) {
      return fail<Incarnation>(ErrorCode::Overflow, "incarnation exhausted", seq);
    }
    return Result<Incarnation>(Incarnation{boot, pid, index, seq + 1u});
  }

  friend constexpr bool operator==(const Incarnation& a, const Incarnation& b) noexcept {
    return a.boot == b.boot && a.pid == b.pid && a.index == b.index && a.seq == b.seq;
  }
  friend constexpr bool operator!=(const Incarnation& a, const Incarnation& b) noexcept {
    return !(a == b);
  }
};

/// Two incarnations belong to the same process boot.
[[nodiscard]] constexpr bool same_boot(const Incarnation& a, const Incarnation& b) noexcept {
  return a.boot == b.boot;
}

inline void digest_incarnation(DigestBuilder& b, const Incarnation& inc) noexcept {
  b.domain(0x11u);
  b.update_u64(inc.boot.hi);
  b.update_u64(inc.boot.lo);
  b.update_u32(inc.pid);
  b.update_u32(inc.index);
  b.update_u64(inc.seq);
}

inline void digest_generation(DigestBuilder& b, Generation g) noexcept {
  b.domain(0x12u);
  b.update_u64(g.value());
}

inline void digest_epoch(DigestBuilder& b, Epoch e) noexcept {
  b.domain(0x13u);
  b.update_u64(e.value());
}

}  // namespace backpressure

namespace std {
template <>
struct hash<backpressure::Generation> {
  [[nodiscard]] std::size_t operator()(backpressure::Generation g) const noexcept {
    return std::hash<std::uint64_t>{}(g.value());
  }
};
template <>
struct hash<backpressure::Epoch> {
  [[nodiscard]] std::size_t operator()(backpressure::Epoch e) const noexcept {
    return std::hash<std::uint64_t>{}(e.value());
  }
};
}  // namespace std
