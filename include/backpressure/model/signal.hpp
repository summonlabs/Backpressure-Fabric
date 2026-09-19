#pragma once

// Backpressure Fabric - authoritative pressure signals.
//
// A signal is the only thing that may start propagation. It carries the
// observation, the exact generations of the evidence behind it, the authority
// that permits it, a provenance chain and a lineage digest. A signal whose
// lineage has already been applied is idempotent: it is recorded and produces
// no additional effect.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/authority.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/model/observation.hpp"
#include "backpressure/model/policy.hpp"

namespace backpressure {

/// One link in the chain of custody of a signal.
struct ProvenanceHop {
  Incarnation publisher{};
  Epoch epoch{};
  Generation generation{};
  Tick tick = kNoTick;
  PropagationId propagation{};
  Digest128 parent{};
};

/// Bounded provenance chain. Older hops are folded into a running digest rather
/// than dropped, so the whole history is still committed to by chain_digest().
struct Provenance {
  static constexpr std::size_t kMaxChain = 16;

  std::array<ProvenanceHop, kMaxChain> hops{};
  std::uint32_t count = 0;
  Digest128 dropped_prefix{};
  std::uint64_t evictions = 0;

  void push(const ProvenanceHop& hop) noexcept;
  [[nodiscard]] bool empty() const noexcept { return count == 0; }
  [[nodiscard]] const ProvenanceHop& latest() const noexcept;
  [[nodiscard]] Digest128 chain_digest() const noexcept;
};

/// An authoritative pressure observation.
struct PressureSignal {
  SignalId id{};
  SourceId source{};
  ResourceId origin{};
  PressureObservation observation{};
  /// Revision of the evidence behind \c observation.
  Generation origin_generation{};
  /// Topology the source reasoned against.
  Digest128 topology_digest{};
  /// Authority epoch the signal was issued under.
  Epoch epoch{};
  /// Exact process incarnation that published the signal.
  Incarnation publisher{};
  Tick issued_at = kNoTick;
  /// Explicit deadline; kNoTick means "policy lifetime from issued_at".
  Tick valid_until = kNoTick;
  AuthorityVector authority{};
  Provenance provenance{};
  /// Content-addressed lineage. Two signals with the same lineage are the same
  /// evidence and the second one is idempotent.
  Digest128 lineage{};
  /// Lower value wins under AggregationRule::PriorityFirst.
  std::uint32_t source_priority = 0;
  /// Q0.16 weight used by AggregationRule::WeightedMax, in [0, 1].
  std::uint32_t weight_q16 = 65536u;

  /// Full validation against a policy, including generation and epoch binding.
  [[nodiscard]] Status validate(const PropagationPolicy& policy, Tick now) const;

  /// Recompute the lineage digest from the signal's evidentiary content.
  [[nodiscard]] Digest128 compute_lineage() const noexcept;

  /// Set lineage from content. Returns the result for logging/explanation.
  Digest128 bind_lineage() noexcept;

  [[nodiscard]] Tick deadline(const PropagationPolicy& policy) const noexcept;
  [[nodiscard]] bool is_expired(Tick now, const PropagationPolicy& policy) const noexcept;
};

}  // namespace backpressure
