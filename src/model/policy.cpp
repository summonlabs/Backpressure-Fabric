// Backpressure Fabric - policy factories.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/model/policy.hpp"

namespace backpressure {

Severity severity_from_magnitude(Magnitude m, const SeverityThresholds& t) noexcept {
  const std::uint32_t milli = m.as_percent_milli();
  if (milli >= t.exhausted_milli) {
    return Severity::Exhausted;
  }
  if (milli >= t.critical_milli) {
    return Severity::Critical;
  }
  if (milli >= t.high_milli) {
    return Severity::High;
  }
  if (milli >= t.elevated_milli) {
    return Severity::Elevated;
  }
  return Severity::Nominal;
}

Result<PropagationPolicy> make_conservative_policy(PolicyId id, Generation generation) {
  PropagationPolicy p;
  p.id = id;
  p.generation = generation;
  p.max_hops = 4;
  p.max_fanout = 16;
  p.max_expansions = 1024;
  p.max_visited = 256;
  p.max_sources = 4;
  p.max_records = 4096;
  p.max_path_records = 64;
  BPFAB_TRY_DECL(Magnitude, floor, Magnitude::from_percent_milli(1000u));
  p.min_propagatable = floor;
  p.lifetime_ticks = 500;
  p.aggregation = AggregationRule::Max;
  p.aggregation_ceiling = Magnitude::full();
  p.allow_amplification = false;
  p.max_cumulative_gain = Gain::unity();
  p.default_cooldown_ticks = 0;
  p.hysteresis_delta = Magnitude::zero();
  BPFAB_TRY_DECL(Q16, decay_step, Q16::from_percent_milli(75000u));
  p.recovery_decay.per_step = decay_step;
  p.recovery_decay.step_ticks = 1;
  p.recovery_decay.floor = Magnitude::zero();
  p.recovery_decay.max_steps = 32;
  p.thresholds = SeverityThresholds::defaults();
  p.require_known_pressure = true;
  BPFAB_TRY(p.validate());
  return Result<PropagationPolicy>(p);
}

Result<PropagationPolicy> make_amplifying_policy(PolicyId id, Generation generation,
                                                 Gain max_cumulative_gain) {
  PropagationPolicy p;
  p.id = id;
  p.generation = generation;
  p.max_hops = 3;
  p.max_fanout = 8;
  p.max_expansions = 256;
  p.max_visited = 128;
  p.max_sources = 2;
  p.max_records = 2048;
  p.max_path_records = 32;
  BPFAB_TRY_DECL(Magnitude, floor, Magnitude::from_percent_milli(1000u));
  p.min_propagatable = floor;
  p.lifetime_ticks = 250;
  p.aggregation = AggregationRule::Max;
  p.aggregation_ceiling = Magnitude::full();
  p.allow_amplification = true;
  p.max_cumulative_gain = max_cumulative_gain;
  p.default_cooldown_ticks = 0;
  p.hysteresis_delta = Magnitude::zero();
  BPFAB_TRY_DECL(Q16, decay_step, Q16::from_percent_milli(50000u));
  p.recovery_decay.per_step = decay_step;
  p.recovery_decay.step_ticks = 1;
  p.recovery_decay.floor = Magnitude::zero();
  p.recovery_decay.max_steps = 16;
  p.thresholds = SeverityThresholds::defaults();
  p.require_known_pressure = true;
  BPFAB_TRY(p.validate());
  return Result<PropagationPolicy>(p);
}

}  // namespace backpressure
