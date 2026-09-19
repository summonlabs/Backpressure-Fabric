// Backpressure Fabric - explanation rendering and fingerprints.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/engine/explain.hpp"

namespace backpressure {

const char* to_string(HopVerdict v) noexcept {
  switch (v) {
    case HopVerdict::Propagated: return "Propagated";
    case HopVerdict::Amplified: return "Amplified";
    case HopVerdict::Clamped: return "Clamped";
    case HopVerdict::Suppressed: return "Suppressed";
    case HopVerdict::Fenced: return "Fenced";
    case HopVerdict::LoopPrevented: return "LoopPrevented";
    case HopVerdict::Idempotent: return "Idempotent";
    case HopVerdict::Stale: return "Stale";
    case HopVerdict::Unauthorized: return "Unauthorized";
    case HopVerdict::Count: break;
  }
  return "Invalid";
}

const char* to_string(SuppressionReason r) noexcept {
  switch (r) {
    case SuppressionReason::None: return "None";
    case SuppressionReason::BelowFloor: return "BelowFloor";
    case SuppressionReason::MaxHops: return "MaxHops";
    case SuppressionReason::MaxFanout: return "MaxFanout";
    case SuppressionReason::NodeAlreadySettled: return "NodeAlreadySettled";
    case SuppressionReason::DuplicateLineage: return "DuplicateLineage";
    case SuppressionReason::ProtectedBoundary: return "ProtectedBoundary";
    case SuppressionReason::SealedResource: return "SealedResource";
    case SuppressionReason::ObservedOnlyResource: return "ObservedOnlyResource";
    case SuppressionReason::MissingResource: return "MissingResource";
    case SuppressionReason::StaleDependencyGeneration: return "StaleDependencyGeneration";
    case SuppressionReason::StaleTopology: return "StaleTopology";
    case SuppressionReason::Fenced: return "Fenced";
    case SuppressionReason::CooldownActive: return "CooldownActive";
    case SuppressionReason::HysteresisNotExceeded: return "HysteresisNotExceeded";
    case SuppressionReason::UnauthorizedScope: return "UnauthorizedScope";
    case SuppressionReason::AmplificationNotAuthorized: return "AmplificationNotAuthorized";
    case SuppressionReason::AmplificationBudgetExhausted: return "AmplificationBudgetExhausted";
    case SuppressionReason::LifetimeExpired: return "LifetimeExpired";
    case SuppressionReason::ExpansionBudgetExhausted: return "ExpansionBudgetExhausted";
    case SuppressionReason::VisitedBudgetExhausted: return "VisitedBudgetExhausted";
    case SuppressionReason::RecordBudgetExhausted: return "RecordBudgetExhausted";
    case SuppressionReason::RecoveryOnlyEdge: return "RecoveryOnlyEdge";
    case SuppressionReason::ZeroAttenuation: return "ZeroAttenuation";
    case SuppressionReason::SourceLimitExceeded: return "SourceLimitExceeded";
    case SuppressionReason::DuplicateSource: return "DuplicateSource";
    case SuppressionReason::Count: break;
  }
  return "Invalid";
}

void PropagationExplanation::finalize() noexcept {
  DigestBuilder b;
  b.domain(0x80u);
  b.update_u64(id.value());
  b.update_u64(signal.value());
  b.update_u64(source.value());
  b.update_u64(origin.value());
  b.update_u8(static_cast<std::uint8_t>(source_severity));
  b.update_u32(source_magnitude.raw());
  b.update_digest(lineage);
  b.update_digest(topology_digest);
  digest_generation(b, topology_generation);
  digest_epoch(b, epoch);
  b.update_digest(authority.digest());
  b.update_u64(issued_at);
  b.update_u64(deadline);
  b.update_u32(effective_ceiling.raw());
  b.update_u32(granted_gain_q16);
  b.update_bool(amplification_granted);
  b.update_bool(amplification_used);
  b.update_u32(max_depth_reached);
  b.update_bool(truncated);
  for (const HopRecord& h : hops) {
    b.update_u64(h.from.value());
    b.update_u64(h.to.value());
    b.update_u64(h.edge.value());
    b.update_u32(h.depth);
    b.update_u32(h.attenuation.q16().raw());
    b.update_u32(h.gain.raw());
    b.update_u64(h.potential_before.raw());
    b.update_u64(h.potential_after.raw());
    b.update_u32(h.cumulative_gain_q16);
    b.update_u8(static_cast<std::uint8_t>(h.verdict));
    b.update_u8(static_cast<std::uint8_t>(h.reason));
  }
  for (const SuppressedEdgeRecord& s : suppressed) {
    b.update_u64(s.from.value());
    b.update_u64(s.to.value());
    b.update_u64(s.edge.value());
    b.update_u32(s.depth);
    b.update_u8(static_cast<std::uint8_t>(s.reason));
  }
  fingerprint = b.finish();
}

}  // namespace backpressure