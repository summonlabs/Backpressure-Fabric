#pragma once

// Backpressure Fabric - fixed point pressure arithmetic.
//
// Pressure is a fraction of a resource's capacity, represented as unsigned
// Q0.16 in [0, 1]. All conversions out of the representable range are refused
// rather than clamped, except where an explicitly named saturating helper is
// used and saturation is the documented semantic.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string_view>

#include "backpressure/core/checked.hpp"
#include "backpressure/core/status.hpp"

namespace backpressure {

/// Raw unsigned Q0.16 fraction in [0, 1].
class Q16 {
 public:
  static constexpr std::uint32_t kScale = 65536u;
  static constexpr std::uint32_t kOne = kScale;

  constexpr Q16() noexcept = default;

  [[nodiscard]] static constexpr Q16 zero() noexcept { return Q16(0u); }
  [[nodiscard]] static constexpr Q16 one() noexcept { return Q16(kOne); }

  /// Accepts any raw value in [0, kOne].
  [[nodiscard]] static Result<Q16> from_raw(std::uint32_t raw) {
    if (raw > kOne) {
      return fail<Q16>(ErrorCode::OutOfRange, "q16 raw", raw);
    }
    return Result<Q16>(Q16(raw));
  }

  [[nodiscard]] static constexpr Q16 from_raw_saturating(std::uint32_t raw) noexcept {
    return Q16(raw > kOne ? kOne : raw);
  }

  /// num/den with num <= den. Rounds to nearest.
  [[nodiscard]] static Result<Q16> from_ratio(std::uint64_t num, std::uint64_t den) {
    if (den == 0) {
      return fail<Q16>(ErrorCode::InvalidArgument, "q16 ratio denominator");
    }
    if (num > den) {
      return fail<Q16>(ErrorCode::OutOfRange, "q16 ratio numerator", num);
    }
    const std::uint64_t scaled = num * static_cast<std::uint64_t>(kScale);
    const std::uint64_t rounded = (scaled + den / 2u) / den;
    return Result<Q16>(Q16(static_cast<std::uint32_t>(rounded > kOne ? kOne : rounded)));
  }

  /// Parts per 100000 (0..100000) to Q0.16. Rounds to nearest.
  [[nodiscard]] static Result<Q16> from_percent_milli(std::uint32_t milli) {
    if (milli > 100000u) {
      return fail<Q16>(ErrorCode::OutOfRange, "q16 percent-milli", milli);
    }
    const std::uint64_t scaled = static_cast<std::uint64_t>(milli) * kOne;
    const std::uint64_t rounded = (scaled + 50000u) / 100000u;
    return Result<Q16>(Q16(static_cast<std::uint32_t>(rounded > kOne ? kOne : rounded)));
  }

  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0u; }
  [[nodiscard]] constexpr bool is_one() const noexcept { return raw_ == kOne; }

  /// Rounded to parts per 100000.
  [[nodiscard]] constexpr std::uint32_t as_percent_milli() const noexcept {
    const std::uint64_t scaled = static_cast<std::uint64_t>(raw_) * 100000u;
    return static_cast<std::uint32_t>((scaled + kScale / 2u) / kScale);
  }

  [[nodiscard]] constexpr double as_double() const noexcept {
    return static_cast<double>(raw_) / static_cast<double>(kScale);
  }

  friend constexpr bool operator==(Q16 a, Q16 b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr bool operator!=(Q16 a, Q16 b) noexcept { return a.raw_ != b.raw_; }
  friend constexpr bool operator<(Q16 a, Q16 b) noexcept { return a.raw_ < b.raw_; }
  friend constexpr bool operator<=(Q16 a, Q16 b) noexcept { return a.raw_ <= b.raw_; }
  friend constexpr bool operator>(Q16 a, Q16 b) noexcept { return a.raw_ > b.raw_; }
  friend constexpr bool operator>=(Q16 a, Q16 b) noexcept { return a.raw_ >= b.raw_; }

 private:
  explicit constexpr Q16(std::uint32_t raw) noexcept : raw_(raw) {}
  std::uint32_t raw_ = 0u;
};

/// Fraction of capacity currently under pressure at a resource.
class Magnitude {
 public:
  constexpr Magnitude() noexcept = default;

  [[nodiscard]] static constexpr Magnitude zero() noexcept { return Magnitude(Q16::zero()); }
  [[nodiscard]] static constexpr Magnitude full() noexcept { return Magnitude(Q16::one()); }

  [[nodiscard]] static Result<Magnitude> from_q16(Q16 v) { return Result<Magnitude>(Magnitude(v)); }

  [[nodiscard]] static Result<Magnitude> from_percent_milli(std::uint32_t milli) {
    BPFAB_TRY_DECL(Q16, q, Q16::from_percent_milli(milli));
    return Result<Magnitude>(Magnitude(q));
  }

  /// Saturated construction from raw Q0.16; values above 1.0 clamp to 1.0.
  [[nodiscard]] static constexpr Magnitude from_raw_q16_saturating(
      std::uint32_t raw) noexcept {
    return Magnitude(Q16::from_raw_saturating(raw));
  }

  [[nodiscard]] constexpr Q16 q16() const noexcept { return value_; }
  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return value_.raw(); }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_.is_zero(); }
  [[nodiscard]] constexpr std::uint32_t as_percent_milli() const noexcept {
    return value_.as_percent_milli();
  }

  friend constexpr bool operator==(Magnitude a, Magnitude b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(Magnitude a, Magnitude b) noexcept {
    return !(a == b);
  }
  friend constexpr bool operator<(Magnitude a, Magnitude b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(Magnitude a, Magnitude b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>(Magnitude a, Magnitude b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator>=(Magnitude a, Magnitude b) noexcept {
    return a.value_ >= b.value_;
  }

 private:
  explicit constexpr Magnitude(Q16 v) noexcept : value_(v) {}
  Q16 value_{};
};

/// Per-edge multiplier, required to be <= 1.
class Attenuation {
 public:
  constexpr Attenuation() noexcept = default;

  [[nodiscard]] static constexpr Attenuation none() noexcept {  // multiplies by zero
    return Attenuation(Q16::zero());
  }
  [[nodiscard]] static constexpr Attenuation unity() noexcept { return Attenuation(Q16::one()); }

  [[nodiscard]] static Result<Attenuation> from_q16(Q16 v) {
    return Result<Attenuation>(Attenuation(v));
  }

  [[nodiscard]] static Result<Attenuation> from_percent_milli(std::uint32_t milli) {
    BPFAB_TRY_DECL(Q16, q, Q16::from_percent_milli(milli));
    return Result<Attenuation>(Attenuation(q));
  }

  [[nodiscard]] constexpr Q16 q16() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_.is_zero(); }
  [[nodiscard]] constexpr std::uint32_t as_percent_milli() const noexcept {
    return value_.as_percent_milli();
  }

  friend constexpr bool operator==(Attenuation a, Attenuation b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(Attenuation a, Attenuation b) noexcept {
    return !(a == b);
  }

 private:
  explicit constexpr Attenuation(Q16 v) noexcept : value_(v) {}
  Q16 value_{};
};

/// Authorized amplification multiplier, required to be >= 1.
///
/// A gain cannot be represented as Q0.16 in [0, 1] because it is by definition
/// at least 1 and may exceed it, so the gain carries its own bounded raw Q0.16
/// scale in [1.0, kMaxRaw]. Anything above kMaxRaw is refused rather than
/// clamped, so a policy can never request an unbounded amplification.
class Gain {
 public:
  static constexpr std::uint32_t kUnityRaw = 65536u;
  /// Hard ceiling: eight times the incoming pressure.
  static constexpr std::uint32_t kMaxRaw = 8u * kUnityRaw;

  constexpr Gain() noexcept = default;

  [[nodiscard]] static constexpr Gain unity() noexcept { return Gain{}; }

  [[nodiscard]] static Result<Gain> from_raw(std::uint32_t raw) {
    if (raw < kUnityRaw) {
      return fail<Gain>(ErrorCode::OutOfRange, "gain below unity", raw);
    }
    if (raw > kMaxRaw) {
      return fail<Gain>(ErrorCode::OutOfRange, "gain above ceiling", raw);
    }
    return Result<Gain>(Gain(raw));
  }

  [[nodiscard]] static Result<Gain> from_percent_milli(std::uint32_t milli) {
    if (milli < 100000u) {
      return fail<Gain>(ErrorCode::OutOfRange, "gain below unity", milli);
    }
    const std::uint64_t scaled = static_cast<std::uint64_t>(milli) * kUnityRaw;
    const std::uint64_t rounded = (scaled + 50000u) / 100000u;
    if (rounded > kMaxRaw) {
      return fail<Gain>(ErrorCode::OutOfRange, "gain above ceiling", milli);
    }
    return Result<Gain>(Gain(static_cast<std::uint32_t>(rounded)));
  }

  /// Rounded back to parts per 100000.
  [[nodiscard]] constexpr std::uint32_t as_percent_milli() const noexcept {
    const std::uint64_t scaled = static_cast<std::uint64_t>(raw_) * 100000u;
    return static_cast<std::uint32_t>((scaled + kUnityRaw / 2u) / kUnityRaw);
  }

  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool is_unity() const noexcept { return raw_ == kUnityRaw; }

  friend constexpr bool operator==(Gain a, Gain b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr bool operator!=(Gain a, Gain b) noexcept { return !(a == b); }

 private:
  explicit constexpr Gain(std::uint32_t raw) noexcept : raw_(raw) {}
  /// Safe default: no amplification.
  std::uint32_t raw_ = kUnityRaw;
};

/// Traversal potential, unsigned Q0.32 in [0, 1]. The potential is the quantity
/// that attenuation acts upon and that bounds every propagation: without
/// authorized amplification it is non-increasing, and it is the reason
/// propagation terminates even on cyclic topologies.
class Potential {
 public:
  static constexpr std::uint64_t kOne = 1ull << 32;

  constexpr Potential() noexcept = default;

  [[nodiscard]] static constexpr Potential zero() noexcept { return Potential(0u); }
  [[nodiscard]] static constexpr Potential one() noexcept { return Potential(kOne); }

  [[nodiscard]] static Potential from_magnitude(Magnitude m) noexcept {
    return Potential(static_cast<std::uint64_t>(m.raw()) << 16u);
  }

  [[nodiscard]] static constexpr Potential from_raw(std::uint64_t raw) noexcept {
    return Potential(raw > kOne ? kOne : raw);
  }

  /// Exact scaling: (raw * att.raw) >> 16. Cannot overflow: raw < 2^32 and
  /// att.raw <= 2^16, so the product is below 2^48. Always <= the input.
  [[nodiscard]] constexpr Potential attenuated(Attenuation att) const noexcept {
    const std::uint64_t product = raw_ * static_cast<std::uint64_t>(att.q16().raw());
    return Potential(product >> 16u);
  }

  /// Authorized amplification. Refuses results above the absolute potential
  /// ceiling; the caller additionally bounds cumulative gain.
  [[nodiscard]] Result<Potential> amplified(Gain gain) const {
    if (gain.is_unity() || raw_ == 0u) {
      return Result<Potential>(*this);
    }
    const std::uint64_t factor = static_cast<std::uint64_t>(gain.raw());
    const std::uint64_t cap = kOne;
    if (raw_ > (cap << 16u) / factor) {
      return fail<Potential>(ErrorCode::Overflow, "potential amplification", raw_);
    }
    const std::uint64_t product = raw_ * factor;
    const std::uint64_t result = product >> 16u;
    return Result<Potential>(Potential(result > kOne ? kOne : result));
  }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0u; }

  /// Round to nearest magnitude; saturates at full.
  [[nodiscard]] constexpr Magnitude to_magnitude() const noexcept {
    const std::uint64_t rounded = (raw_ + 32768u) >> 16u;
    return Magnitude::from_raw_q16_saturating(
        static_cast<std::uint32_t>(rounded > 65536u ? 65536u : rounded));
  }

  friend constexpr bool operator==(Potential a, Potential b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr bool operator!=(Potential a, Potential b) noexcept { return a.raw_ != b.raw_; }
  friend constexpr bool operator<(Potential a, Potential b) noexcept { return a.raw_ < b.raw_; }
  friend constexpr bool operator<=(Potential a, Potential b) noexcept { return a.raw_ <= b.raw_; }
  friend constexpr bool operator>(Potential a, Potential b) noexcept { return a.raw_ > b.raw_; }
  friend constexpr bool operator>=(Potential a, Potential b) noexcept { return a.raw_ >= b.raw_; }

 private:
  explicit constexpr Potential(std::uint64_t raw) noexcept : raw_(raw) {}
  std::uint64_t raw_ = 0u;
};

}  // namespace backpressure