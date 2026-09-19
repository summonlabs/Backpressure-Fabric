#pragma once

// Backpressure Fabric - checked integer arithmetic.
//
// Every size, capacity, rate, counter and time unit that can be influenced from
// outside the runtime is funnelled through these helpers. They never wrap and
// never silently narrow: an unrepresentable result is reported, not truncated.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "backpressure/core/status.hpp"

namespace backpressure::checked {

template <class T>
[[nodiscard]] constexpr T max_of() noexcept {
  return std::numeric_limits<T>::max();
}

// --- unsigned 64-bit primitives -------------------------------------------

[[nodiscard]] constexpr bool add_overflows_u64(std::uint64_t a, std::uint64_t b) noexcept {
  return a > max_of<std::uint64_t>() - b;
}

[[nodiscard]] constexpr bool mul_overflows_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a == 0 || b == 0) {
    return false;
  }
  return a > max_of<std::uint64_t>() / b;
}

[[nodiscard]] inline Result<std::uint64_t> add_u64(std::uint64_t a, std::uint64_t b,
                                                   std::string_view what) {
  if (add_overflows_u64(a, b)) {
    return fail<std::uint64_t>(ErrorCode::Overflow, what, a);
  }
  return Result<std::uint64_t>(a + b);
}

[[nodiscard]] inline Result<std::uint64_t> sub_u64(std::uint64_t a, std::uint64_t b,
                                                   std::string_view what) {
  if (b > a) {
    return fail<std::uint64_t>(ErrorCode::Overflow, what, a);
  }
  return Result<std::uint64_t>(a - b);
}

[[nodiscard]] inline Result<std::uint64_t> mul_u64(std::uint64_t a, std::uint64_t b,
                                                   std::string_view what) {
  if (mul_overflows_u64(a, b)) {
    return fail<std::uint64_t>(ErrorCode::Overflow, what, a);
  }
  return Result<std::uint64_t>(a * b);
}

[[nodiscard]] inline Result<std::uint64_t> div_u64(std::uint64_t a, std::uint64_t b,
                                                   std::string_view what) {
  if (b == 0) {
    return fail<std::uint64_t>(ErrorCode::InvalidArgument, what, a);
  }
  return Result<std::uint64_t>(a / b);
}

/// Saturating addition with a caller supplied ceiling.
[[nodiscard]] constexpr std::uint64_t add_sat_u64(std::uint64_t a, std::uint64_t b,
                                                  std::uint64_t ceiling) noexcept {
  if (add_overflows_u64(a, b)) {
    return ceiling;
  }
  const std::uint64_t sum = a + b;
  return sum > ceiling ? ceiling : sum;
}

[[nodiscard]] constexpr std::uint64_t mul_sat_u64(std::uint64_t a, std::uint64_t b,
                                                  std::uint64_t ceiling) noexcept {
  if (mul_overflows_u64(a, b)) {
    return ceiling;
  }
  const std::uint64_t product = a * b;
  return product > ceiling ? ceiling : product;
}

// --- narrowing -------------------------------------------------------------

[[nodiscard]] inline Result<std::uint8_t> to_u8(std::uint64_t v, std::string_view what) {
  if (v > max_of<std::uint8_t>()) {
    return fail<std::uint8_t>(ErrorCode::OutOfRange, what, v);
  }
  return Result<std::uint8_t>(static_cast<std::uint8_t>(v));
}

[[nodiscard]] inline Result<std::uint16_t> to_u16(std::uint64_t v, std::string_view what) {
  if (v > max_of<std::uint16_t>()) {
    return fail<std::uint16_t>(ErrorCode::OutOfRange, what, v);
  }
  return Result<std::uint16_t>(static_cast<std::uint16_t>(v));
}

[[nodiscard]] inline Result<std::uint32_t> to_u32(std::uint64_t v, std::string_view what) {
  if (v > max_of<std::uint32_t>()) {
    return fail<std::uint32_t>(ErrorCode::OutOfRange, what, v);
  }
  return Result<std::uint32_t>(static_cast<std::uint32_t>(v));
}

/// size_t is 32-bit on some supported targets; narrow only when representable.
[[nodiscard]] inline Result<std::size_t> to_size(std::uint64_t v, std::string_view what) {
  if (v > static_cast<std::uint64_t>(max_of<std::size_t>())) {
    return fail<std::size_t>(ErrorCode::OutOfRange, what, v);
  }
  return Result<std::size_t>(static_cast<std::size_t>(v));
}

/// Convert a signed value that is known to be non-negative.
[[nodiscard]] inline Result<std::uint64_t> from_i64(std::int64_t v, std::string_view what) {
  if (v < 0) {
    return fail<std::uint64_t>(ErrorCode::OutOfRange, what,
                               static_cast<std::uint64_t>(v));
  }
  return Result<std::uint64_t>(static_cast<std::uint64_t>(v));
}

// --- 32-bit guarded ops ----------------------------------------------------

[[nodiscard]] inline Result<std::uint32_t> add_u32(std::uint32_t a, std::uint32_t b,
                                                   std::string_view what) {
  const std::uint64_t sum = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
  return to_u32(sum, what);
}

[[nodiscard]] inline Result<std::uint32_t> mul_u32(std::uint32_t a, std::uint32_t b,
                                                   std::string_view what) {
  const std::uint64_t product =
      static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b);
  return to_u32(product, what);
}

}  // namespace backpressure::checked
