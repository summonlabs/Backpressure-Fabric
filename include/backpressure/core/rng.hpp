#pragma once

// Backpressure Fabric - deterministic pseudo random generators.
//
// Seeded randomized and adversarial testing needs reproducibility: the same
// seed must produce the same population on every platform and build.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "backpressure/core/digest.hpp"

namespace backpressure {

/// SplitMix64: tiny, fast, fully specified.
class SplitMix64 {
 public:
  explicit constexpr SplitMix64(std::uint64_t seed = 0x9E3779B97F4A7C15ull) noexcept
      : state_(seed) {}

  [[nodiscard]] constexpr std::uint64_t next_u64() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  /// Uniform in [0, bound). Returns 0 when bound == 0.
  [[nodiscard]] constexpr std::uint64_t next_bounded(std::uint64_t bound) noexcept {
    if (bound == 0) {
      return 0;
    }
    const std::uint64_t threshold = (std::numeric_limits<std::uint64_t>::max() % bound) + 1u;
    for (;;) {
      const std::uint64_t r = next_u64();
      if (r >= threshold) {
        return r % bound;
      }
    }
  }

  [[nodiscard]] constexpr bool next_bool(std::uint32_t percent_true = 50) noexcept {
    return next_bounded(100u) < percent_true;
  }

  [[nodiscard]] constexpr std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

/// PCG-XSH-RR 64/32: stable, well distributed, good for structure fuzzing.
class Pcg32 {
 public:
  explicit constexpr Pcg32(std::uint64_t seed = 0x853C49E6748FEA9Bull,
                           std::uint64_t seq = 0xDA3E39CB94B95BDBull) noexcept
      : state_(0u), inc_((seq << 1u) | 1u) {
    (void)next_u32();
    state_ += seed;
    (void)next_u32();
  }

  [[nodiscard]] constexpr std::uint32_t next_u32() noexcept {
    const std::uint64_t old = state_;
    state_ = old * 6364136223846793005ull + inc_;
    const std::uint32_t xorshifted =
        static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
  }

  [[nodiscard]] constexpr std::uint64_t next_u64() noexcept {
    const std::uint64_t hi = static_cast<std::uint64_t>(next_u32()) << 32u;
    return hi | static_cast<std::uint64_t>(next_u32());
  }

  [[nodiscard]] constexpr std::uint32_t next_bounded(std::uint32_t bound) noexcept {
    if (bound == 0u) {
      return 0u;
    }
    const std::uint32_t threshold =
        static_cast<std::uint32_t>((0xFFFFFFFFull % bound) + 1ull);
    for (;;) {
      const std::uint32_t r = next_u32();
      if (r >= threshold) {
        return r % bound;
      }
    }
  }

 private:
  std::uint64_t state_;
  std::uint64_t inc_;
};

/// Derive a stable sub-seed for a labelled stream, so a single test seed fans
/// out into independent but reproducible streams.
[[nodiscard]] inline std::uint64_t derive_seed(std::uint64_t root, std::string_view label) noexcept {
  DigestBuilder b;
  b.domain(0xE1u);
  b.update_u64(root);
  b.update(label);
  return b.finish().fold64();
}

}  // namespace backpressure
