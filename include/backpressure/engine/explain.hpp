#pragma once

// Backpressure Fabric - explanation records.
//
// Every authoritative decision produces an explanation that names the source
// signal, the propagation path, the attenuation applied at each hop, the
// aggregate result, every suppressed edge with its reason, the loop-prevention
// events and the authority vector that permitted the decision. Explanations are
// bounded: when a budget is exhausted the explanation says so rather than
// silently omitting records.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/fixed.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/authority.hpp"
#include "backpressure/model/fence.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/model/observation.hpp"

namespace backpressure {

enum class HopVerdict : std::uint8_t {
  /// Pressure crossed the edge unchanged in kind.
  Propagated = 0,
  /// Pressure crossed the edge and was explicitly amplified under authority.
  Amplified,
  /// Pressure crossed the edge but the aggregate was clamped by the ceiling.
  Clamped,
  /// The edge was considered and rejected.
  Suppressed,
  /// The traversal stopped at the edge because of a fence.
  Fenced,
  /// The target had already been settled; the edge closes a cycle.
  LoopPrevented,
  /// The edge duplicated an already-applied lineage.
  Idempotent,
  /// The edge is stale with respect to the evidence generation.
  Stale,
  /// Authority does not cover the edge.
  Unauthorized,
  Count,
};

[[nodiscard]] const char* to_string(HopVerdict v) noexcept;

enum class SuppressionReason : std::uint8_t {
  None = 0,
  BelowFloor,
  MaxHops,
  MaxFanout,
  NodeAlreadySettled,
  DuplicateLineage,
  ProtectedBoundary,
  SealedResource,
  ObservedOnlyResource,
  MissingResource,
  StaleDependencyGeneration,
  StaleTopology,
  Fenced,
  CooldownActive,
  HysteresisNotExceeded,
  UnauthorizedScope,
  AmplificationNotAuthorized,
  AmplificationBudgetExhausted,
  LifetimeExpired,
  ExpansionBudgetExhausted,
  VisitedBudgetExhausted,
  RecordBudgetExhausted,
  RecoveryOnlyEdge,
  ZeroAttenuation,
  SourceLimitExceeded,
  DuplicateSource,
  Count,
};

[[nodiscard]] const char* to_string(SuppressionReason r) noexcept;

/// One considered traversal step.
struct HopRecord {
  ResourceId from{};
  ResourceId to{};
  EdgeId edge{};
  std::uint32_t depth = 0;
  Attenuation attenuation{};
  Gain gain = Gain::unity();
  Potential potential_before{};
  Potential potential_after{};
  std::uint32_t cumulative_gain_q16 = 65536u;
  HopVerdict verdict = HopVerdict::Suppressed;
  SuppressionReason reason = SuppressionReason::None;
};

/// A bounded record of edges that were considered and rejected.
///
/// Records are aggregated per (resource, reason): \c count is the exact number
/// of edges refused for that reason and the edge identity is one representative
/// example. This keeps the explanation size independent of fan-out while the
/// counters stay exact.
struct SuppressedEdgeRecord {
  ResourceId from{};
  ResourceId to{};
  EdgeId edge{};
  std::uint32_t depth = 0;
  std::uint32_t count = 1;
  SuppressionReason reason = SuppressionReason::None;
};

/// Number of distinct suppression reasons; bounds the aggregated record count.
inline constexpr std::size_t kSuppressionReasonCount =
    static_cast<std::size_t>(SuppressionReason::Count);

/// One source's contribution to a resource.
struct Contribution {
  SourceId source{};
  PropagationId propagation{};
  SignalId signal{};
  Magnitude magnitude{};
  /// Magnitude after the aggregation weight was applied.
  Magnitude weighted{};
  std::uint32_t depth = 0;
  std::uint32_t source_priority = 0;
};

/// Aggregated authoritative result at one resource.
struct NodeOutcome {
  ResourceId resource{};
  /// Pressure the fabric decided to apply at this resource.
  Magnitude applied{};
  Severity severity = Severity::Unknown;
  /// Strongest single source contribution before aggregation.
  Magnitude strongest_single{};
  bool received = false;
  bool forwarded = false;
  bool recovery = false;
  std::uint32_t best_depth = 0;
  SourceId best_source{};
  PropagationId best_propagation{};
  /// Resource and edge through which the winning potential arrived. Together
  /// with the other outcomes this reconstructs the full propagation path.
  ResourceId parent{};
  EdgeId parent_edge{};
  std::vector<Contribution> contributions{};
  bool contributions_truncated = false;
  std::uint64_t contribution_count = 0;
  SuppressionReason blocked_reason = SuppressionReason::None;
  FenceId governing_fence{};
};

/// Aggregate counters for one engine call. These are counts of completed work,
/// never of submitted work.
struct EngineCounters {
  std::uint64_t signals_received = 0;
  std::uint64_t signals_accepted = 0;
  std::uint64_t signals_rejected = 0;
  std::uint64_t signals_idempotent = 0;
  std::uint64_t propagations = 0;
  std::uint64_t nodes_settled = 0;
  std::uint64_t hops_considered = 0;
  std::uint64_t hops_propagated = 0;
  std::uint64_t hops_suppressed = 0;
  std::uint64_t loop_preventions = 0;
  std::uint64_t stale_rejections = 0;
  std::uint64_t fence_rejections = 0;
  std::uint64_t unauthorized_rejections = 0;
  std::uint64_t amplifications = 0;
  std::uint64_t clamps = 0;
  std::uint64_t budget_exhaustions = 0;
  std::uint64_t record_truncations = 0;
  std::uint64_t lineage_evictions = 0;
  std::uint32_t peak_depth = 0;
};

/// Full explanation of one accepted signal's propagation.
struct PropagationExplanation {
  PropagationId id{};
  SignalId signal{};
  SourceId source{};
  ResourceId origin{};
  Severity source_severity = Severity::Unknown;
  Magnitude source_magnitude{};
  Digest128 lineage{};
  Digest128 topology_digest{};
  Generation topology_generation{};
  Epoch epoch{};
  AuthorityVector authority{};
  Tick issued_at = kNoTick;
  Tick deadline = kNoTick;
  /// Ceiling actually enforced: min(policy ceiling, potential ceiling).
  Magnitude effective_ceiling{};
  /// Cumulative gain granted by policy and authority together.
  std::uint32_t granted_gain_q16 = 65536u;
  bool amplification_granted = false;
  bool amplification_used = false;
  std::uint32_t max_depth_reached = 0;
  std::vector<HopRecord> hops{};
  std::vector<SuppressedEdgeRecord> suppressed{};
  /// True when a record budget stopped the explanation from being complete.
  bool truncated = false;
  EngineCounters counters{};
  Digest128 fingerprint{};

  /// Compute the fingerprint over the complete explanation.
  void finalize() noexcept;
};

}  // namespace backpressure