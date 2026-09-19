#pragma once

// Backpressure Fabric - pressure observations and UNKNOWN semantics.
//
// UNKNOWN is a first class value, not a low number. An UNKNOWN observation
// carries no magnitude, cannot be compared, and can never authorize
// propagation. A declared severity that overstates the measured magnitude is
// downgraded to the measured one: the fabric never invents pressure.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "backpressure/core/fixed.hpp"
#include "backpressure/core/status.hpp"

namespace backpressure {

/// Ordered pressure severity. Unknown sorts below Nominal and is not a
/// magnitude: it means "no authoritative observation exists".
enum class Severity : std::uint8_t {
  Unknown = 0,
  Nominal = 1,
  Elevated = 2,
  High = 3,
  Critical = 4,
  Exhausted = 5,
};

inline constexpr std::uint8_t kSeverityCount = 6;

[[nodiscard]] constexpr const char* to_string(Severity s) noexcept {
  switch (s) {
    case Severity::Unknown: return "Unknown";
    case Severity::Nominal: return "Nominal";
    case Severity::Elevated: return "Elevated";
    case Severity::High: return "High";
    case Severity::Critical: return "Critical";
    case Severity::Exhausted: return "Exhausted";
  }
  return "Invalid";
}

/// Magnitude bands (per mille of capacity) that map a measurement to a
/// severity. Strictly increasing and bounded by 100000.
struct SeverityThresholds {
  std::uint32_t elevated_milli = 40000u;
  std::uint32_t high_milli = 65000u;
  std::uint32_t critical_milli = 85000u;
  std::uint32_t exhausted_milli = 98000u;

  [[nodiscard]] static SeverityThresholds defaults() noexcept { return SeverityThresholds{}; }

  [[nodiscard]] Status validate() const {
    if (elevated_milli == 0u || elevated_milli >= high_milli || high_milli >= critical_milli ||
        critical_milli >= exhausted_milli || exhausted_milli > 100000u) {
      return Status::error(ErrorCode::InvalidArgument, "severity thresholds",
                           exhausted_milli);
    }
    return Status::success();
  }
};

[[nodiscard]] Severity severity_from_magnitude(Magnitude m,
                                               const SeverityThresholds& t) noexcept;

/// A pressure observation: either UNKNOWN, or a measured magnitude with a
/// severity that never overstates the measurement.
class PressureObservation {
 public:
  constexpr PressureObservation() noexcept = default;

  [[nodiscard]] static constexpr PressureObservation unknown() noexcept {
    return PressureObservation{};
  }

  [[nodiscard]] static PressureObservation observed(Magnitude m,
                                                    const SeverityThresholds& t) noexcept {
    PressureObservation o;
    o.magnitude_ = m;
    o.severity_ = severity_from_magnitude(m, t);
    o.present_ = true;
    return o;
  }

  /// Declared severity is honoured only when it does not exceed the derived
  /// severity. Downgrades are recorded so explanations can show the correction.
  [[nodiscard]] static PressureObservation observed_with_severity(
      Magnitude m, Severity declared, const SeverityThresholds& t) noexcept {
    PressureObservation o;
    o.magnitude_ = m;
    const Severity derived = severity_from_magnitude(m, t);
    if (declared == Severity::Unknown || declared > derived) {
      o.severity_ = derived;
      o.downgraded_ = declared != derived;
    } else {
      o.severity_ = declared;
    }
    o.present_ = true;
    return o;
  }

  [[nodiscard]] constexpr bool is_unknown() const noexcept { return !present_; }
  [[nodiscard]] constexpr bool has_evidence() const noexcept { return present_; }
  [[nodiscard]] constexpr Severity severity() const noexcept {
    return present_ ? severity_ : Severity::Unknown;
  }
  [[nodiscard]] constexpr Magnitude magnitude() const noexcept {
    return present_ ? magnitude_ : Magnitude::zero();
  }
  [[nodiscard]] constexpr bool severity_downgraded() const noexcept { return downgraded_; }

  /// The single gate that decides whether this observation may start anything.
  [[nodiscard]] constexpr bool can_authorize_propagation() const noexcept {
    return present_ && !magnitude_.is_zero() && severity_ != Severity::Unknown;
  }

  friend constexpr bool operator==(const PressureObservation& a,
                                   const PressureObservation& b) noexcept {
    if (a.present_ != b.present_) {
      return false;
    }
    if (!a.present_) {
      return true;
    }
    return a.magnitude_ == b.magnitude_ && a.severity_ == b.severity_;
  }
  friend constexpr bool operator!=(const PressureObservation& a,
                                   const PressureObservation& b) noexcept {
    return !(a == b);
  }

 private:
  Magnitude magnitude_{};
  Severity severity_ = Severity::Unknown;
  bool present_ = false;
  bool downgraded_ = false;
};

}  // namespace backpressure