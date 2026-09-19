#pragma once

// Backpressure Fabric - propagation policy.
//
// A policy is the complete, bounded description of how far pressure may travel,
// how much it may be damped, how simultaneous sources are combined, and when it
// must decay. Policies are versioned by identity and generation; changing a
// policy produces a new generation and invalidates propagations bound to the
// old one.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/fixed.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/model/observation.hpp"

namespace backpressure {

enum class AggregationRule : std::uint8_t {
  /// Take the strongest single contribution. Never exceeds any input.
  Max = 0,
  /// Saturating sum, clamped by the policy ceiling and by 1.0.
  SaturatingSum,
  /// Sum of (contribution x weight) with a per-source weight, clamped.
  WeightedMax,
  /// The contribution from the highest priority source wins outright.
  PriorityFirst,
  Count,
};

[[nodiscard]] constexpr const char* to_string(AggregationRule r) noexcept {
  switch (r) {
    case AggregationRule::Max: return "Max";
    case AggregationRule::SaturatingSum: return "SaturatingSum";
    case AggregationRule::WeightedMax: return "WeightedMax";
    case AggregationRule::PriorityFirst: return "PriorityFirst";
    case AggregationRule::Count: break;
  }
  return "Invalid";
}

/// Recovery decay applied when the source pressure falls or disappears.
struct DecaySchedule {
  /// Multiplier applied once per step. 1.0 disables decay.
  Q16 per_step = Q16::one();
  /// Logical ticks between steps. Must be >= 1.
  Tick step_ticks = 1;
  /// Decay never takes pressure below this floor.
  Magnitude floor = Magnitude::zero();
  /// Hard cap on the number of steps a single recovery may be scheduled for.
  std::uint32_t max_steps = 64;

  [[nodiscard]] Status validate() const {
    if (step_ticks == 0) {
      return Status::error(ErrorCode::InvalidArgument, "decay step ticks");
    }
    if (max_steps == 0u || max_steps > 4096u) {
      return Status::error(ErrorCode::OutOfRange, "decay max steps", max_steps);
    }
    return Status::success();
  }

  /// Magnitude after p steps decay steps, never below the floor and never
  /// above the starting magnitude.
  [[nodiscard]] Magnitude decay(Magnitude start, std::uint32_t steps) const noexcept {
    if (steps > max_steps) {
      steps = max_steps;
    }
    std::uint64_t raw = start.raw();
    for (std::uint32_t i = 0; i < steps && raw > 0u; ++i) {
      raw = (raw * static_cast<std::uint64_t>(per_step.raw())) >> 16u;
    }
    if (raw > start.raw()) {
      raw = start.raw();
    }
    const std::uint32_t floor_raw = floor.raw();
    if (raw < floor_raw) {
      raw = floor_raw;
    }
    if (raw > start.raw()) {
      raw = start.raw();
    }
    return Magnitude::from_raw_q16_saturating(static_cast<std::uint32_t>(raw));
  }
};

struct PropagationPolicy {
  PolicyId id{};
  Generation generation{};

  /// Hard bound on hops from the origin resource.
  std::uint32_t max_hops = 8;
  /// Hard bound on edges expanded from a single resource within one propagation.
  std::uint32_t max_fanout = 32;
  /// Hard bound on total edge expansions within one propagation.
  std::uint32_t max_expansions = 4096;
  /// Hard bound on total node settlements within one propagation.
  std::uint32_t max_visited = 1024;
  /// Hard bound on simultaneous sources one request may carry.
  std::uint32_t max_sources = 8;
  /// Hard bound on explanation records produced by one propagation.
  std::uint32_t max_records = 8192;
  /// Hard bound on one explanation path.
  std::uint32_t max_path_records = 256;

  /// Pressure below this magnitude never propagates and damps out.
  Magnitude min_propagatable{};
  /// Logical lifetime of a propagation instance.
  Tick lifetime_ticks = 1000;

  AggregationRule aggregation = AggregationRule::Max;
  /// Absolute ceiling for the aggregated pressure at any resource. No
  /// combination of authorizations may exceed the policy ceiling.
  Magnitude aggregation_ceiling = Magnitude::full();

  // --- amplification -------------------------------------------------------
  /// Master switch. When false, no propagation may ever increase pressure.
  bool allow_amplification = false;
  /// Bound on cumulative gain along one propagation path. Must be unity when
  /// amplification is disabled.
  Gain max_cumulative_gain = Gain::unity();

  // --- damping -------------------------------------------------------------
  /// Default cooldown applied to edges that do not override it.
  Tick default_cooldown_ticks = 0;
  /// A repeated observation must exceed the previous accepted magnitude by at
  /// least this much to be accepted again inside the cooldown window.
  Magnitude hysteresis_delta{};

  /// Recovery decay schedule.
  DecaySchedule recovery_decay{};

  /// Severity bands used to derive severity from a magnitude.
  SeverityThresholds thresholds{};

  /// UNKNOWN observations never authorize propagation. Kept explicit so a
  /// policy cannot be edited into believing otherwise.
  bool require_known_pressure = true;

  [[nodiscard]] static PropagationPolicy defaults() noexcept { return PropagationPolicy{}; }

  [[nodiscard]] Status validate() const {
    if (!id.valid()) {
      return Status::error(ErrorCode::InvalidArgument, "policy id");
    }
    if (!generation.known()) {
      return Status::error(ErrorCode::StaleGeneration, "policy generation unbound");
    }
    if (max_hops == 0u || max_hops > 64u) {
      return Status::error(ErrorCode::OutOfRange, "policy max hops", max_hops);
    }
    if (max_fanout == 0u || max_fanout > (1u << 16)) {
      return Status::error(ErrorCode::OutOfRange, "policy max fanout", max_fanout);
    }
    if (max_expansions == 0u || max_expansions > (1u << 24)) {
      return Status::error(ErrorCode::OutOfRange, "policy max expansions", max_expansions);
    }
    if (max_visited == 0u || max_visited > (1u << 22)) {
      return Status::error(ErrorCode::OutOfRange, "policy max visited", max_visited);
    }
    if (max_sources == 0u || max_sources > (1u << 12)) {
      return Status::error(ErrorCode::OutOfRange, "policy max sources", max_sources);
    }
    if (max_records == 0u || max_records > (1u << 20)) {
      return Status::error(ErrorCode::OutOfRange, "policy max records", max_records);
    }
    if (max_path_records == 0u || max_path_records > max_records) {
      return Status::error(ErrorCode::OutOfRange, "policy max path records", max_path_records);
    }
    // The record budget has to cover one hop record per expansion plus one
    // aggregated suppression record per settled resource. A policy that cannot
    // afford its own explanation is rejected rather than silently truncated.
    {
      const std::uint64_t worst_case = static_cast<std::uint64_t>(max_expansions) +
                                       static_cast<std::uint64_t>(max_visited);
      if (static_cast<std::uint64_t>(max_records) < worst_case) {
        return Status::error(ErrorCode::InvalidArgument, "policy record budget", worst_case);
      }
    }
    if (lifetime_ticks == 0) {
      return Status::error(ErrorCode::OutOfRange, "policy lifetime");
    }
    if (aggregation >= AggregationRule::Count) {
      return Status::error(ErrorCode::InvalidArgument, "policy aggregation rule",
                           static_cast<std::uint64_t>(aggregation));
    }
    if (aggregation_ceiling.is_zero()) {
      return Status::error(ErrorCode::InvalidArgument, "policy aggregation ceiling");
    }
    if (!allow_amplification && !max_cumulative_gain.is_unity()) {
      return Status::error(ErrorCode::Conflict, "gain bound without amplification");
    }
    if (!require_known_pressure) {
      return Status::error(ErrorCode::InvalidArgument,
                           "unknown pressure can never be authorized");
    }
    BPFAB_TRY(thresholds.validate());
    BPFAB_TRY(recovery_decay.validate());
    if (default_cooldown_ticks > (1ull << 40)) {
      return Status::error(ErrorCode::OutOfRange, "policy cooldown", default_cooldown_ticks);
    }
    return Status::success();
  }

  /// Effective hop bound for an edge: the tighter of edge override and policy.
  [[nodiscard]] constexpr std::uint32_t effective_max_hops(std::uint32_t edge_override) const noexcept {
    const std::uint32_t base = edge_override == 0u ? max_hops : edge_override;
    return base < max_hops ? base : max_hops;
  }

  [[nodiscard]] constexpr Tick effective_cooldown(Tick edge_override) const noexcept {
    return edge_override == 0 ? default_cooldown_ticks : edge_override;
  }

  [[nodiscard]] Digest128 digest() const noexcept {
    DigestBuilder b;
    b.domain(0x50u);
    b.update_u64(id.value());
    digest_generation(b, generation);
    b.update_u32(max_hops);
    b.update_u32(max_fanout);
    b.update_u32(max_expansions);
    b.update_u32(max_visited);
    b.update_u32(max_sources);
    b.update_u32(max_records);
    b.update_u32(max_path_records);
    b.update_u32(min_propagatable.raw());
    b.update_u64(lifetime_ticks);
    b.update_u8(static_cast<std::uint8_t>(aggregation));
    b.update_u32(aggregation_ceiling.raw());
    b.update_bool(allow_amplification);
    b.update_u32(max_cumulative_gain.raw());
    b.update_u64(default_cooldown_ticks);
    b.update_u32(hysteresis_delta.raw());
    b.update_u32(recovery_decay.per_step.raw());
    b.update_u64(recovery_decay.step_ticks);
    b.update_u32(recovery_decay.floor.raw());
    b.update_u32(recovery_decay.max_steps);
    b.update_u32(thresholds.elevated_milli);
    b.update_u32(thresholds.high_milli);
    b.update_u32(thresholds.critical_milli);
    b.update_u32(thresholds.exhausted_milli);
    b.update_bool(require_known_pressure);
    return b.finish();
  }
};

/// Fully-formed conservative policy: no amplification, modest bounds, damping on.
[[nodiscard]] Result<PropagationPolicy> make_conservative_policy(PolicyId id,
                                                                Generation generation);

/// Policy that explicitly authorizes bounded amplification.
[[nodiscard]] Result<PropagationPolicy> make_amplifying_policy(PolicyId id, Generation generation,
                                                               Gain max_cumulative_gain);

}  // namespace backpressure