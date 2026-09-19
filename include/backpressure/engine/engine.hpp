#pragma once

// Backpressure Fabric - propagation engine.
//
// The engine is a pure, deterministic function of immutable inputs (topology,
// policy) and explicitly owned mutable state (the ledger and the fence table).
// It owns no threads, takes no callbacks and holds no internal locks, so it can
// never re-enter itself. Callers that share a ledger across threads serialise
// access to it themselves.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/fixed.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/engine/explain.hpp"
#include "backpressure/model/fence.hpp"
#include "backpressure/model/policy.hpp"
#include "backpressure/model/signal.hpp"
#include "backpressure/model/topology.hpp"

namespace backpressure {

/// Key identifying one directed dependency application.
struct EdgeTargetKey {
  ResourceId target{};
  EdgeId edge{};

  friend bool operator==(const EdgeTargetKey& a, const EdgeTargetKey& b) noexcept {
    return a.target == b.target && a.edge == b.edge;
  }
};

struct EdgeTargetKeyHash {
  [[nodiscard]] std::size_t operator()(const EdgeTargetKey& k) const noexcept {
    const std::size_t h1 = std::hash<ResourceId>{}(k.target);
    const std::size_t h2 = std::hash<EdgeId>{}(k.edge);
    return h1 ^ (h2 + 0x9E3779B9u + (h1 << 6) + (h1 >> 2));
  }
};

/// Key identifying one (source, resource) application.
struct SourceResourceKey {
  SourceId source{};
  ResourceId resource{};

  friend bool operator==(const SourceResourceKey& a, const SourceResourceKey& b) noexcept {
    return a.source == b.source && a.resource == b.resource;
  }
};

struct SourceResourceKeyHash {
  [[nodiscard]] std::size_t operator()(const SourceResourceKey& k) const noexcept {
    const std::size_t h1 = std::hash<SourceId>{}(k.source);
    const std::size_t h2 = std::hash<ResourceId>{}(k.resource);
    return h1 ^ (h2 + 0x9E3779B9u + (h1 << 6) + (h1 >> 2));
  }
};

/// Cross-request authoritative state: lineage idempotency, damping history and
/// the pressure currently applied per (source, resource).
///
/// Every container is bounded. Lineage eviction is FIFO and is counted, because
/// an evicted lineage can no longer be recognised as a replay within the ledger;
/// replay is additionally refused by epoch, lifetime and generation binding, and
/// durable replay protection lives in the store.
class PropagationLedger {
 public:
  static constexpr std::size_t kDefaultLineageCapacity = 1u << 16;
  static constexpr std::size_t kDefaultApplicationCapacity = 1u << 16;
  static constexpr std::size_t kDefaultDampingCapacity = 1u << 16;

  struct ApplicationRecord {
    Magnitude magnitude{};
    Tick tick = kNoTick;
    bool present = false;
  };

  struct DampingRecord {
    Magnitude magnitude{};
    Tick tick = kNoTick;
    bool present = false;
  };

  explicit PropagationLedger(std::size_t lineage_capacity = kDefaultLineageCapacity,
                             std::size_t application_capacity = kDefaultApplicationCapacity,
                             std::size_t damping_capacity = kDefaultDampingCapacity);

  // --- lineage -------------------------------------------------------------
  /// Marks a lineage. Returns true when it was newly marked, false when the
  /// lineage had already been applied (idempotent replay).
  bool mark_lineage(Digest128 lineage);
  [[nodiscard]] bool has_lineage(Digest128 lineage) const;
  [[nodiscard]] std::size_t lineage_count() const noexcept { return lineage_index_.size(); }
  [[nodiscard]] std::uint64_t lineage_evictions() const noexcept { return lineage_evictions_; }

  // --- applied pressure ----------------------------------------------------
  [[nodiscard]] ApplicationRecord application(SourceId source, ResourceId resource) const;
  [[nodiscard]] Status record_application(SourceId source, ResourceId resource, Magnitude m,
                                          Tick tick);
  [[nodiscard]] std::size_t application_count() const noexcept { return applications_.size(); }
  [[nodiscard]] std::uint64_t application_rejections() const noexcept {
    return application_rejections_;
  }

  // --- damping -------------------------------------------------------------
  [[nodiscard]] DampingRecord damping(ResourceId target, EdgeId edge) const;
  [[nodiscard]] Status record_damping(ResourceId target, EdgeId edge, Magnitude m, Tick tick);
  [[nodiscard]] std::size_t damping_count() const noexcept { return damping_.size(); }
  [[nodiscard]] std::uint64_t damping_rejections() const noexcept { return damping_rejections_; }

  /// Visit every recorded (source, resource) application. Order is unspecified;
  /// callers that need determinism sort the collected keys.
  template <class Fn>
  void for_each_application(Fn&& fn) const {
    for (const auto& pair : applications_) {
      fn(pair.first, pair.second);
    }
  }

  void clear();

 private:
  std::size_t lineage_capacity_;
  std::size_t application_capacity_;
  std::size_t damping_capacity_;
  std::deque<Digest128> lineage_order_;
  std::unordered_set<Digest128> lineage_index_;
  std::uint64_t lineage_evictions_ = 0;
  std::unordered_map<SourceResourceKey, ApplicationRecord, SourceResourceKeyHash> applications_;
  std::uint64_t application_rejections_ = 0;
  std::unordered_map<EdgeTargetKey, DampingRecord, EdgeTargetKeyHash> damping_;
  std::uint64_t damping_rejections_ = 0;
};

/// Optional per-edge generation pin. A propagation may pin the dependency
/// generations it reasoned about; an edge whose live generation differs from its
/// pin is refused as stale.
struct DependencyPin {
  EdgeId edge{};
  Generation generation{};
};

struct PropagationRequest {
  PropagationId id{};
  PolicyId policy_id{};
  Generation policy_generation{};
  Digest128 topology_digest{};
  Epoch epoch{};
  Tick now = kNoTick;
  std::vector<PressureSignal> signals{};
  std::vector<DependencyPin> pins{};
  /// Plan only: compute the full decision and explanation but commit nothing to
  /// the ledger.
  bool dry_run = false;
  /// Optional tightening of the policy record budget. When the budget binds, the
  /// traversal stops before applying pressure it could not explain and the
  /// outcome reports complete == false. 0 means "use the policy budget".
  std::uint32_t record_budget_override = 0;
};

struct RecoveryRequest {
  PropagationId id{};
  SourceId source{};
  ResourceId origin{};
  Generation origin_generation{};
  Digest128 topology_digest{};
  Epoch epoch{};
  Tick now = kNoTick;
  /// Residual observed pressure. Zero means the source has fully recovered.
  Magnitude residual{};
  /// Authority under which the recovery is asserted.
  AuthorityVector authority{};
  bool dry_run = false;
};

struct RejectionRecord {
  SignalId signal{};
  SourceId source{};
  Status status{};
};

struct PropagationOutcome {
  Status status{};
  PropagationId id{};
  Epoch epoch{};
  Digest128 topology_digest{};
  Generation topology_generation{};
  Tick now = kNoTick;
  std::vector<PropagationExplanation> explanations{};
  std::vector<RejectionRecord> rejections{};
  std::vector<NodeOutcome> nodes{};
  std::vector<NodeOutcome> recovery_nodes{};
  EngineCounters counters{};
  /// False when any explanation hit a record budget.
  bool complete = true;
  /// Highest hop count that actually carried pressure.
  std::uint32_t peak_depth = 0;
  Digest128 fingerprint{};

  void finalize() noexcept;
};

/// Immutable inputs plus explicitly owned mutable state.
struct EngineContext {
  const Topology* topology = nullptr;
  const PropagationPolicy* policy = nullptr;
  PropagationLedger* ledger = nullptr;
  const FenceTable* fences = nullptr;
  Epoch live_epoch{};

  [[nodiscard]] bool valid() const noexcept {
    return topology != nullptr && policy != nullptr && ledger != nullptr;
  }
};

class PropagationEngine {
 public:
  /// Deterministic bounded propagation for a batch of authoritative signals.
  [[nodiscard]] static PropagationOutcome propagate(const EngineContext& ctx,
                                                    const PropagationRequest& request);

  /// Deterministic recovery decay for one source. Can only lower applied
  /// pressure; it never raises it and never introduces new hops.
  [[nodiscard]] static PropagationOutcome recover(const EngineContext& ctx,
                                                  const RecoveryRequest& request);
};

}  // namespace backpressure
