// Backpressure Fabric - propagation engine implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/engine/engine.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>

namespace backpressure {

// ===========================================================================
// PropagationLedger
// ===========================================================================

PropagationLedger::PropagationLedger(std::size_t lineage_capacity,
                                     std::size_t application_capacity,
                                     std::size_t damping_capacity)
    : lineage_capacity_(lineage_capacity == 0 ? 1u : lineage_capacity),
      application_capacity_(application_capacity == 0 ? 1u : application_capacity),
      damping_capacity_(damping_capacity == 0 ? 1u : damping_capacity) {}

bool PropagationLedger::mark_lineage(Digest128 lineage) {
  if (!lineage_index_.insert(lineage).second) {
    return false;
  }
  lineage_order_.push_back(lineage);
  while (lineage_order_.size() > lineage_capacity_) {
    lineage_index_.erase(lineage_order_.front());
    lineage_order_.pop_front();
    ++lineage_evictions_;
  }
  return true;
}

bool PropagationLedger::has_lineage(Digest128 lineage) const {
  return lineage_index_.find(lineage) != lineage_index_.end();
}

PropagationLedger::ApplicationRecord PropagationLedger::application(SourceId source,
                                                                   ResourceId resource) const {
  const auto it = applications_.find(SourceResourceKey{source, resource});
  if (it == applications_.end()) {
    return ApplicationRecord{};
  }
  return it->second;
}

Status PropagationLedger::record_application(SourceId source, ResourceId resource, Magnitude m,
                                             Tick tick) {
  const SourceResourceKey key{source, resource};
  const auto it = applications_.find(key);
  if (it == applications_.end() && applications_.size() >= application_capacity_) {
    ++application_rejections_;
    return Status::error(ErrorCode::LimitExceeded, "application ledger",
                         static_cast<std::uint64_t>(applications_.size()));
  }
  applications_[key] = ApplicationRecord{m, tick, true};
  return Status::success();
}

PropagationLedger::DampingRecord PropagationLedger::damping(ResourceId target,
                                                            EdgeId edge) const {
  const auto it = damping_.find(EdgeTargetKey{target, edge});
  if (it == damping_.end()) {
    return DampingRecord{};
  }
  return it->second;
}

Status PropagationLedger::record_damping(ResourceId target, EdgeId edge, Magnitude m, Tick tick) {
  const EdgeTargetKey key{target, edge};
  const auto it = damping_.find(key);
  if (it == damping_.end() && damping_.size() >= damping_capacity_) {
    ++damping_rejections_;
    return Status::error(ErrorCode::LimitExceeded, "damping ledger",
                         static_cast<std::uint64_t>(damping_.size()));
  }
  damping_[key] = DampingRecord{m, tick, true};
  return Status::success();
}

void PropagationLedger::clear() {
  lineage_order_.clear();
  lineage_index_.clear();
  applications_.clear();
  damping_.clear();
}

// ===========================================================================
// Engine internals
// ===========================================================================

namespace {

constexpr std::uint32_t kNoIndex = Topology::kInvalidIndex;
constexpr std::uint32_t kUnityGainQ16 = 65536u;
constexpr std::size_t kMaxContributionsPerNode = 16;
constexpr std::size_t kMaxDependencyPins = 4096;

struct TraversalState {
  bool settled = false;
  Potential potential{};
  std::uint32_t depth = 0;
  std::uint32_t parent = kNoIndex;
  std::uint32_t parent_edge = kNoIndex;
  std::uint32_t cumulative_gain_q16 = kUnityGainQ16;
  bool amplified = false;
};

struct QueueEntry {
  Potential potential{};
  std::uint32_t node = kNoIndex;
  std::uint32_t depth = 0;
  std::uint32_t cumulative_gain_q16 = kUnityGainQ16;
  /// Predecessor resource and edge, so the winning path can be reconstructed
  /// from the outcome without storing a path per queue entry.
  std::uint32_t parent = kNoIndex;
  std::uint32_t parent_edge = kNoIndex;
  bool amplified = false;
};

/// Highest potential first, then fewest hops, then lowest resource index. The
/// ordering is total, so traversal order never depends on hash iteration order.
struct QueueEntryLess {
  bool operator()(const QueueEntry& a, const QueueEntry& b) const noexcept {
    if (a.potential != b.potential) {
      return a.potential < b.potential;
    }
    if (a.depth != b.depth) {
      return a.depth > b.depth;
    }
    return a.node > b.node;
  }
};

struct NodeAccumulator {
  ResourceId resource{};
  Potential best_potential{};
  Magnitude strongest{};
  bool received = false;
  bool forwarded = false;
  std::uint32_t best_depth = 0;
  SourceId best_source{};
  PropagationId best_propagation{};
  ResourceId parent{};
  EdgeId parent_edge{};
  std::vector<Contribution> contributions{};
  bool contributions_truncated = false;
  std::uint64_t contribution_count = 0;
  SuppressionReason blocked_reason = SuppressionReason::None;
  FenceId governing_fence{};
};

struct Accumulators {
  std::unordered_map<std::uint32_t, NodeAccumulator> by_node{};

  NodeAccumulator& at(std::uint32_t index, ResourceId id) {
    const auto it = by_node.find(index);
    if (it != by_node.end()) {
      return it->second;
    }
    NodeAccumulator acc;
    acc.resource = id;
    return by_node.emplace(index, std::move(acc)).first->second;
  }
};

[[nodiscard]] Magnitude weighted_magnitude(const PressureSignal& signal, Magnitude m) noexcept {
  const std::uint64_t product = static_cast<std::uint64_t>(m.raw()) *
                                static_cast<std::uint64_t>(signal.weight_q16);
  std::uint64_t scaled = product >> 16u;
  if (scaled > static_cast<std::uint64_t>(Magnitude::full().raw())) {
    scaled = Magnitude::full().raw();
  }
  return Magnitude::from_raw_q16_saturating(static_cast<std::uint32_t>(scaled));
}

[[nodiscard]] Magnitude aggregate_contributions(const PropagationPolicy& policy,
                                                const std::vector<Contribution>& cs) noexcept {
  if (cs.empty()) {
    return Magnitude::zero();
  }
  switch (policy.aggregation) {
    case AggregationRule::Max: {
      Magnitude best = cs.front().magnitude;
      for (const Contribution& c : cs) {
        if (c.magnitude > best) {
          best = c.magnitude;
        }
      }
      return best;
    }
    case AggregationRule::WeightedMax: {
      Magnitude best = cs.front().weighted;
      for (const Contribution& c : cs) {
        if (c.weighted > best) {
          best = c.weighted;
        }
      }
      return best;
    }
    case AggregationRule::PriorityFirst: {
      const Contribution* winner = &cs.front();
      for (const Contribution& c : cs) {
        if (c.source_priority < winner->source_priority) {
          winner = &c;
        } else if (c.source_priority == winner->source_priority) {
          if (c.magnitude > winner->magnitude) {
            winner = &c;
          } else if (c.magnitude == winner->magnitude && c.source < winner->source) {
            winner = &c;
          }
        }
      }
      return winner->magnitude;
    }
    case AggregationRule::SaturatingSum: {
      std::uint64_t sum = 0;
      for (const Contribution& c : cs) {
        sum += static_cast<std::uint64_t>(c.magnitude.raw());
        if (sum > static_cast<std::uint64_t>(Magnitude::full().raw())) {
          sum = Magnitude::full().raw();
          break;
        }
      }
      return Magnitude::from_raw_q16_saturating(static_cast<std::uint32_t>(sum));
    }
    case AggregationRule::Count:
      break;
  }
  return Magnitude::zero();
}

[[nodiscard]] bool is_stale_code(ErrorCode code) noexcept { return is_staleness(code); }

}  // namespace

// ===========================================================================
// Traversal
// ===========================================================================

namespace {

class Traversal {
 public:
  Traversal(const EngineContext& ctx, const PropagationPolicy& policy, Tick now,
            const std::vector<std::uint8_t>& pin_present,
            const std::vector<Generation>& pin_generation, std::uint32_t record_budget)
      : ctx_(ctx),
        policy_(policy),
        now_(now),
        pin_present_(pin_present),
        pin_generation_(pin_generation),
        record_budget_(record_budget),
        state_(ctx.topology->resources().size()),
        stamp_(ctx.topology->resources().size(), 0u) {}

  Status run(const PressureSignal& signal, PropagationId pid, Accumulators& acc,
             EngineCounters& counters, PropagationExplanation& expl, bool& truncated) {
    const Topology& topo = *ctx_.topology;
    ++epoch_stamp_;
    if (epoch_stamp_ == 0u) {
      std::fill(stamp_.begin(), stamp_.end(), 0u);
      epoch_stamp_ = 1u;
    }
    while (!queue_.empty()) {
      queue_.pop();
    }

    const std::uint32_t origin_index = topo.index_of(signal.origin);
    const Resource* origin = topo.resource_at(origin_index);
    if (origin == nullptr) {
      return Status::error(ErrorCode::NotFound, "signal origin resource",
                           signal.origin.value());
    }

    const Potential seed = Potential::from_magnitude(signal.observation.magnitude());
    if (seed.is_zero()) {
      return Status::error(ErrorCode::UnknownPressure, "signal magnitude is zero");
    }
    const Potential floor_potential = Potential::from_magnitude(policy_.min_propagatable);

    const Fence* origin_fence =
        ctx_.fences != nullptr ? ctx_.fences->governing(origin->id, now_) : nullptr;

    const std::uint32_t authority_hops =
        signal.authority.max_hops_granted == 0u
            ? policy_.max_hops
            : (signal.authority.max_hops_granted < policy_.max_hops
                   ? signal.authority.max_hops_granted
                   : policy_.max_hops);

    std::uint32_t granted_gain = kUnityGainQ16;
    expl.amplification_granted = false;
    if (policy_.allow_amplification && signal.authority.amplification_authorized) {
      granted_gain = std::min(policy_.max_cumulative_gain.raw(),
                              signal.authority.max_gain.raw());
      expl.amplification_granted = true;
    }
    expl.granted_gain_q16 = granted_gain;
    expl.effective_ceiling = policy_.aggregation_ceiling;

    NodeAccumulator& origin_acc = acc.at(origin_index, origin->id);

    bool origin_receives = origin->can_receive();
    SuppressionReason origin_block = SuppressionReason::None;
    if (origin_fence != nullptr && origin_fence->refuses_ingress()) {
      origin_receives = false;
      origin_block = SuppressionReason::Fenced;
      origin_acc.governing_fence = origin_fence->id;
      ++counters.fence_rejections;
    } else if (!origin_receives) {
      origin_block = origin->protection == ProtectionClass::Sealed
                         ? SuppressionReason::SealedResource
                         : SuppressionReason::ObservedOnlyResource;
    }

    if (!origin_receives) {
      origin_acc.received = false;
      origin_acc.blocked_reason = origin_block;
      suppress_all(origin_index, 0u, origin_block, counters, expl, truncated);
      return Status::success();
    }

    // The origin is settled by the traversal loop itself, exactly once, so it
    // contributes exactly one settlement record like every other resource.
    // Egress rules (protection class, fences) are applied by the generic
    // per-node logic on the first iteration.
    const std::size_t n = state_.size();
    std::size_t visited = 0;
    std::size_t expansions = 0;
    bool budget_stop = false;
    bool record_stop = false;

    push_queue(origin_index, seed, 0u, kUnityGainQ16, false, kNoIndex, kNoIndex);

    while (!queue_.empty()) {
      if (visited >= static_cast<std::size_t>(policy_.max_visited)) {
        ++counters.budget_exhaustions;
        budget_stop = true;
        break;
      }
      const QueueEntry entry = queue_.top();
      queue_.pop();
      if (entry.node >= n) {
        continue;
      }
      TraversalState& st = state_at(entry.node);
      if (st.settled) {
        continue;
      }
      st.settled = true;
      if (!begin_node(entry.node, expl)) {
        truncated = true;
        ++counters.record_truncations;
        record_stop = true;
        break;
      }
      ++visited;
      ++counters.nodes_settled;
      if (entry.depth > counters.peak_depth) {
        counters.peak_depth = entry.depth;
      }
      if (entry.depth > expl.max_depth_reached) {
        expl.max_depth_reached = entry.depth;
      }

      const Resource* res = topo.resource_at(entry.node);
      if (res == nullptr) {
        continue;
      }

      // Every settled resource contributes exactly one settlement record. The
      // queue cannot settle a resource twice, so the contribution count of a
      // resource is exactly the number of sources whose pressure reached it.
      record_settlement(entry.node, res->id, entry.potential, entry.depth, entry.parent,
                        entry.parent_edge, signal, pid, acc);

      const auto outs = topo.out_edge_indices_at(entry.node);

      if (entry.depth >= authority_hops) {
        suppress_all(entry.node, entry.depth, SuppressionReason::MaxHops, counters, expl, truncated);
        continue;
      }

      const Fence* node_fence =
          ctx_.fences != nullptr ? ctx_.fences->governing(res->id, now_) : nullptr;
      if (!res->can_forward()) {
        acc.at(entry.node, res->id).blocked_reason = SuppressionReason::ProtectedBoundary;
        suppress_all(entry.node, entry.depth, SuppressionReason::ProtectedBoundary, counters, expl,
                     truncated);
        continue;
      }
      if (node_fence != nullptr && node_fence->refuses_egress()) {
        NodeAccumulator& na = acc.at(entry.node, res->id);
        na.blocked_reason = SuppressionReason::Fenced;
        na.governing_fence = node_fence->id;
        ++counters.fence_rejections;
        suppress_all(entry.node, entry.depth, SuppressionReason::Fenced, counters, expl, truncated);
        continue;
      }

      std::uint32_t fanout = 0;
      for (const std::uint32_t edge_index : outs) {
        ++counters.hops_considered;
        const DependencyEdge* edge = topo.edge_at(edge_index);
        if (edge == nullptr) {
          continue;
        }

        if (fanout >= policy_.max_fanout) {
          reject(edge, entry.depth, SuppressionReason::MaxFanout, HopVerdict::Suppressed, counters,
                 expl);
          continue;
        }
        if (expansions >= static_cast<std::size_t>(policy_.max_expansions)) {
          reject(edge, entry.depth, SuppressionReason::ExpansionBudgetExhausted,
                 HopVerdict::Suppressed, counters, expl);
          budget_stop = true;
          continue;
        }
        if (edge_index < pin_present_.size() && pin_present_[edge_index] != 0u &&
            pin_generation_[edge_index] != edge->dependency_generation) {
          reject(edge, entry.depth, SuppressionReason::StaleDependencyGeneration,
                 HopVerdict::Stale, counters, expl);
          continue;
        }
        if (edge->attenuation.is_zero()) {
          reject(edge, entry.depth, SuppressionReason::ZeroAttenuation, HopVerdict::Suppressed,
                 counters, expl);
          continue;
        }
        if (edge->flags.has(EdgeFlagKind::RecoveryOnly)) {
          reject(edge, entry.depth, SuppressionReason::RecoveryOnlyEdge, HopVerdict::Suppressed,
                 counters, expl);
          continue;
        }

        const std::uint32_t to_index = topo.index_of(edge->to);
        const Resource* to_res = topo.resource_at(to_index);
        if (to_res == nullptr) {
          reject(edge, entry.depth, SuppressionReason::MissingResource, HopVerdict::Suppressed,
                 counters, expl);
          continue;
        }
        if (!to_res->can_receive()) {
          reject(edge, entry.depth,
                 to_res->protection == ProtectionClass::Sealed
                     ? SuppressionReason::SealedResource
                     : SuppressionReason::ObservedOnlyResource,
                 HopVerdict::Suppressed, counters, expl);
          continue;
        }
        if (state_at(to_index).settled) {
          reject(edge, entry.depth, SuppressionReason::NodeAlreadySettled,
                 HopVerdict::LoopPrevented, counters, expl);
          continue;
        }
        if (!signal.authority.covers(edge->to)) {
          reject(edge, entry.depth, SuppressionReason::UnauthorizedScope,
                 HopVerdict::Unauthorized, counters, expl);
          continue;
        }
        const Fence* target_fence =
            ctx_.fences != nullptr ? ctx_.fences->governing(edge->to, now_) : nullptr;
        if (target_fence != nullptr && target_fence->refuses_ingress()) {
          reject(edge, entry.depth, SuppressionReason::Fenced, HopVerdict::Fenced, counters, expl);
          continue;
        }

        const std::uint32_t edge_hops = policy_.effective_max_hops(edge->max_hops);
        const std::uint32_t bound = edge_hops < authority_hops ? edge_hops : authority_hops;
        if (entry.depth + 1u > bound) {
          reject(edge, entry.depth, SuppressionReason::MaxHops, HopVerdict::Suppressed, counters,
                 expl);
          continue;
        }

        Potential next = entry.potential.attenuated(edge->attenuation);
        std::uint32_t gain = entry.cumulative_gain_q16;
        bool amplified = entry.amplified;
        if (edge->is_amplifying()) {
          if (!policy_.allow_amplification || !signal.authority.amplification_authorized) {
            reject(edge, entry.depth, SuppressionReason::AmplificationNotAuthorized,
                   HopVerdict::Unauthorized, counters, expl);
            continue;
          }
          const std::uint64_t requested =
              (static_cast<std::uint64_t>(gain) *
               static_cast<std::uint64_t>(edge->amplification.raw())) >>
              16u;
          if (requested > static_cast<std::uint64_t>(granted_gain)) {
            reject(edge, entry.depth, SuppressionReason::AmplificationBudgetExhausted,
                   HopVerdict::Unauthorized, counters, expl);
            continue;
          }
          const Result<Potential> amplified_potential = next.amplified(edge->amplification);
          if (!amplified_potential.ok()) {
            reject(edge, entry.depth, SuppressionReason::AmplificationBudgetExhausted,
                   HopVerdict::Unauthorized, counters, expl);
            continue;
          }
          next = amplified_potential.value();
          gain = static_cast<std::uint32_t>(requested);
          amplified = true;
          ++counters.amplifications;
          expl.amplification_used = true;
        }

        if (next < floor_potential) {
          reject(edge, entry.depth, SuppressionReason::BelowFloor, HopVerdict::Suppressed,
                 counters, expl);
          continue;
        }

        const Tick cooldown = policy_.effective_cooldown(edge->cooldown_ticks);
        if (cooldown > 0) {
          const PropagationLedger::DampingRecord dr = ctx_.ledger->damping(edge->to, edge->id);
          if (dr.present) {
            const Tick elapsed = now_ > dr.tick ? now_ - dr.tick : 0;
            if (elapsed < cooldown) {
              const Magnitude candidate = next.to_magnitude();
              const Magnitude previous = dr.magnitude;
              const Magnitude required =
                  Magnitude::from_raw_q16_saturating(static_cast<std::uint32_t>(
                      std::min<std::uint64_t>(
                          static_cast<std::uint64_t>(Magnitude::full().raw()),
                          static_cast<std::uint64_t>(previous.raw()) +
                              static_cast<std::uint64_t>(policy_.hysteresis_delta.raw()))));
              if (candidate <= required) {
                reject(edge, entry.depth, SuppressionReason::HysteresisNotExceeded,
                       HopVerdict::Suppressed, counters, expl);
                continue;
              }
            }
          }
        }

        if (record_budget_ == 0u) {
          truncated = true;
          ++counters.record_truncations;
          record_stop = true;
          break;
        }
        if (!push_hop(res->id, edge->to, edge->id, entry.depth, edge->attenuation,
                      edge->amplification, entry.potential, next, gain,
                      amplified ? HopVerdict::Amplified : HopVerdict::Propagated,
                      SuppressionReason::None, expl)) {
          truncated = true;
          ++counters.record_truncations;
          record_stop = true;
          break;
        }

        NodeAccumulator& from_acc = acc.at(entry.node, res->id);
        from_acc.forwarded = true;
        push_queue(to_index, next, entry.depth + 1u, gain, amplified, entry.node, edge_index);
        ++fanout;
        ++expansions;
        ++counters.hops_propagated;
      }
      if (record_stop) {
        break;
      }
      if (budget_stop) {
        break;
      }
    }

    (void)budget_stop;
    if (!flush_suppressions(expl)) {
      truncated = true;
      ++counters.record_truncations;
    }
    return Status::success();
  }

 private:
  TraversalState& state_at(std::uint32_t index) {
    if (stamp_[index] != epoch_stamp_) {
      stamp_[index] = epoch_stamp_;
      state_[index] = TraversalState{};
    }
    return state_[index];
  }

  void push_queue(std::uint32_t node, Potential potential, std::uint32_t depth,
                  std::uint32_t gain, bool amplified, std::uint32_t parent,
                  std::uint32_t parent_edge) {
    TraversalState& st = state_at(node);
    if (st.settled) {
      return;
    }
    QueueEntry entry;
    entry.potential = potential;
    entry.node = node;
    entry.depth = depth;
    entry.cumulative_gain_q16 = gain;
    entry.parent = parent;
    entry.parent_edge = parent_edge;
    entry.amplified = amplified;
    queue_.push(entry);
    st.potential = potential;
    st.depth = depth;
    st.cumulative_gain_q16 = gain;
    st.parent = parent;
    st.parent_edge = parent_edge;
    st.amplified = amplified;
  }

  void record_settlement(std::uint32_t index, ResourceId id, Potential potential,
                         std::uint32_t depth, std::uint32_t parent, std::uint32_t parent_edge,
                         const PressureSignal& signal, PropagationId pid, Accumulators& acc) {
    NodeAccumulator& a = acc.at(index, id);
    const Topology& topo = *ctx_.topology;
    const Resource* parent_res = parent == kNoIndex ? nullptr : topo.resource_at(parent);
    const DependencyEdge* parent_ed =
        parent_edge == kNoIndex ? nullptr : topo.edge_at(parent_edge);
    if (!a.received || potential > a.best_potential) {
      a.received = true;
      a.best_potential = potential;
      a.strongest = potential.to_magnitude();
      a.best_depth = depth;
      a.best_source = signal.source;
      a.best_propagation = pid;
      a.parent = parent_res != nullptr ? parent_res->id : ResourceId{};
      a.parent_edge = parent_ed != nullptr ? parent_ed->id : EdgeId{};
      a.blocked_reason = SuppressionReason::None;
    }
    Contribution c;
    c.source = signal.source;
    c.propagation = pid;
    c.signal = signal.id;
    c.magnitude = potential.to_magnitude();
    c.weighted = weighted_magnitude(signal, c.magnitude);
    c.depth = depth;
    c.source_priority = signal.source_priority;
    ++a.contribution_count;
    if (a.contributions.size() < kMaxContributionsPerNode) {
      a.contributions.push_back(c);
    } else {
      a.contributions_truncated = true;
    }
  }

  bool push_hop(ResourceId from, ResourceId to, EdgeId edge, std::uint32_t depth,
                Attenuation attenuation, Gain gain, Potential before, Potential after,
                std::uint32_t cumulative_gain, HopVerdict verdict, SuppressionReason reason,
                PropagationExplanation& expl) {
    if (record_budget_ == 0u) {
      return false;
    }
    --record_budget_;
    HopRecord r;
    r.from = from;
    r.to = to;
    r.edge = edge;
    r.depth = depth;
    r.attenuation = attenuation;
    r.gain = gain;
    r.potential_before = before;
    r.potential_after = after;
    r.cumulative_gain_q16 = cumulative_gain;
    r.verdict = verdict;
    r.reason = reason;
    expl.hops.push_back(r);
    return true;
  }

  /// Aggregated refusal for one (resource, reason) pair.
  struct SuppressionAggregate {
    ResourceId from{};
    ResourceId to{};
    EdgeId edge{};
    std::uint32_t depth = 0;
    std::uint32_t count = 0;
  };

  /// Record a refused edge. Records are aggregated per reason so explanation
  /// size never grows with fan-out while the counters stay exact.
  void reject(const DependencyEdge* edge, std::uint32_t depth, SuppressionReason reason,
              HopVerdict verdict, EngineCounters& counters, PropagationExplanation& expl) {
    (void)expl;
    SuppressionAggregate& aggregate = pending_[static_cast<std::size_t>(reason)];
    if (aggregate.count == 0u) {
      aggregate.from = edge->from;
      aggregate.to = edge->to;
      aggregate.edge = edge->id;
      aggregate.depth = depth;
    }
    if (aggregate.count != std::numeric_limits<std::uint32_t>::max()) {
      ++aggregate.count;
    }
    ++counters.hops_suppressed;
    if (verdict == HopVerdict::LoopPrevented) {
      ++counters.loop_preventions;
    }
    if (reason == SuppressionReason::StaleDependencyGeneration) {
      ++counters.stale_rejections;
    }
    if (reason == SuppressionReason::Fenced) {
      ++counters.fence_rejections;
    }
    if (reason == SuppressionReason::UnauthorizedScope ||
        reason == SuppressionReason::AmplificationNotAuthorized ||
        reason == SuppressionReason::AmplificationBudgetExhausted) {
      ++counters.unauthorized_rejections;
    }
    if (reason == SuppressionReason::ExpansionBudgetExhausted ||
        reason == SuppressionReason::VisitedBudgetExhausted) {
      ++counters.budget_exhaustions;
    }
  }

  /// Emit one aggregated record per distinct reason observed at the current
  /// resource, then reset. Returns false when the record budget is exhausted.
  bool flush_suppressions(PropagationExplanation& expl) {
    bool ok = true;
    for (std::size_t slot = 0; slot < pending_.size(); ++slot) {
      SuppressionAggregate& aggregate = pending_[slot];
      if (aggregate.count == 0u) {
        continue;
      }
      if (record_budget_ == 0u) {
        ok = false;
        aggregate.count = 0u;
        continue;
      }
      --record_budget_;
      SuppressedEdgeRecord record;
      record.from = aggregate.from;
      record.to = aggregate.to;
      record.edge = aggregate.edge;
      record.depth = aggregate.depth;
      record.count = aggregate.count;
      record.reason = static_cast<SuppressionReason>(slot);
      expl.suppressed.push_back(record);
      aggregate.count = 0u;
    }
    return ok;
  }

  /// Move the aggregation window to a newly settled resource.
  bool begin_node(std::uint32_t node, PropagationExplanation& expl) {
    if (current_node_ == node) {
      return true;
    }
    const bool ok = flush_suppressions(expl);
    current_node_ = node;
    return ok;
  }

  void suppress_all(std::uint32_t node_index, std::uint32_t depth, SuppressionReason reason,
                    EngineCounters& counters, PropagationExplanation& expl, bool& truncated) {
    const Topology& topo = *ctx_.topology;
    const auto outs = topo.out_edge_indices_at(node_index);
    for (const std::uint32_t edge_index : outs) {
      ++counters.hops_considered;
      const DependencyEdge* edge = topo.edge_at(edge_index);
      if (edge == nullptr) {
        continue;
      }
      reject(edge, depth, reason, HopVerdict::Suppressed, counters, expl);
    }
    if (!flush_suppressions(expl)) {
      truncated = true;
      ++counters.record_truncations;
    }
  }

  const EngineContext& ctx_;
  const PropagationPolicy& policy_;
  Tick now_;
  const std::vector<std::uint8_t>& pin_present_;
  const std::vector<Generation>& pin_generation_;
  std::uint32_t record_budget_ = 0;
  std::uint32_t current_node_ = kNoIndex;
  std::array<SuppressionAggregate, kSuppressionReasonCount> pending_{};
  std::vector<TraversalState> state_;
  std::vector<std::uint32_t> stamp_;
  std::uint32_t epoch_stamp_ = 1u;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueEntryLess> queue_;
};

}  // namespace

// ===========================================================================
// Outcome assembly
// ===========================================================================

void PropagationOutcome::finalize() noexcept {
  DigestBuilder b;
  b.domain(0x81u);
  b.update_u64(id.value());
  digest_epoch(b, epoch);
  b.update_digest(topology_digest);
  digest_generation(b, topology_generation);
  b.update_u64(now);
  b.update_u16(static_cast<std::uint16_t>(status.code()));
  b.update_bool(complete);
  b.update_u32(peak_depth);
  for (const PropagationExplanation& e : explanations) {
    b.update_digest(e.fingerprint);
  }
  for (const RejectionRecord& r : rejections) {
    b.update_u64(r.signal.value());
    b.update_u64(r.source.value());
    b.update_u16(static_cast<std::uint16_t>(r.status.code()));
  }
  for (const NodeOutcome& n : nodes) {
    b.update_u64(n.resource.value());
    b.update_u32(n.applied.raw());
    b.update_bool(n.received);
    b.update_bool(n.forwarded);
    b.update_u32(n.best_depth);
    b.update_u64(n.best_source.value());
    b.update_u8(static_cast<std::uint8_t>(n.severity));
  }
  for (const NodeOutcome& n : recovery_nodes) {
    b.update_u64(n.resource.value());
    b.update_u32(n.applied.raw());
    b.update_bool(n.recovery);
  }
  b.update_u64(counters.signals_accepted);
  b.update_u64(counters.signals_rejected);
  b.update_u64(counters.hops_propagated);
  b.update_u64(counters.hops_suppressed);
  fingerprint = b.finish();
}

// ===========================================================================
// Engine entry points
// ===========================================================================

PropagationOutcome PropagationEngine::propagate(const EngineContext& ctx,
                                                const PropagationRequest& request) {
  PropagationOutcome outcome;
  outcome.id = request.id;
  outcome.epoch = request.epoch;
  outcome.topology_digest = request.topology_digest;
  outcome.now = request.now;

  if (!ctx.valid()) {
    outcome.status = Status::error(ErrorCode::InvalidArgument, "engine context");
    outcome.finalize();
    return outcome;
  }
  const Topology& topo = *ctx.topology;
  const PropagationPolicy& policy = *ctx.policy;
  PropagationLedger& ledger = *ctx.ledger;

  outcome.topology_generation = topo.generation();

  const Status policy_status = policy.validate();
  if (!policy_status.ok()) {
    outcome.status = policy_status;
    outcome.finalize();
    return outcome;
  }
  if (!request.id.valid()) {
    outcome.status = Status::error(ErrorCode::InvalidArgument, "propagation id");
    outcome.finalize();
    return outcome;
  }
  if (request.policy_id != policy.id) {
    outcome.status = Status::error(ErrorCode::NotFound, "policy id", request.policy_id.value());
    outcome.finalize();
    return outcome;
  }
  if (request.policy_generation != policy.generation) {
    outcome.status = Status::error(ErrorCode::StaleGeneration, "policy generation",
                                   request.policy_generation.value());
    outcome.finalize();
    return outcome;
  }
  if (request.topology_digest != topo.digest()) {
    outcome.status = Status::error(ErrorCode::StaleGeneration, "topology digest mismatch");
    outcome.finalize();
    return outcome;
  }
  if (request.epoch != ctx.live_epoch) {
    outcome.status = Status::error(ErrorCode::StaleEpoch, "request epoch", request.epoch.value());
    outcome.finalize();
    return outcome;
  }
  if (request.signals.empty()) {
    outcome.status = Status::error(ErrorCode::InvalidArgument, "no signals");
    outcome.finalize();
    return outcome;
  }
  if (request.signals.size() > static_cast<std::size_t>(policy.max_sources)) {
    outcome.counters.signals_received = request.signals.size();
    outcome.counters.signals_rejected = request.signals.size();
    outcome.status = Status::error(ErrorCode::LimitExceeded, "source limit",
                                   static_cast<std::uint64_t>(request.signals.size()));
    outcome.finalize();
    return outcome;
  }

  outcome.counters.signals_received = request.signals.size();

  if (request.pins.size() > kMaxDependencyPins) {
    outcome.status = Status::error(ErrorCode::LimitExceeded, "dependency pins",
                                   static_cast<std::uint64_t>(request.pins.size()));
    outcome.finalize();
    return outcome;
  }
  std::vector<std::uint8_t> pin_present(topo.edges().size(), 0u);
  std::vector<Generation> pin_generation(topo.edges().size(), Generation::unknown());
  for (const DependencyPin& pin : request.pins) {
    const std::uint32_t idx = [&]() -> std::uint32_t {
      for (std::size_t i = 0; i < topo.edges().size(); ++i) {
        if (topo.edges()[i].id == pin.edge) {
          return static_cast<std::uint32_t>(i);
        }
      }
      return kNoIndex;
    }();
    if (idx == kNoIndex) {
      outcome.status = Status::error(ErrorCode::NotFound, "pinned edge", pin.edge.value());
      outcome.finalize();
      return outcome;
    }
    pin_present[idx] = 1u;
    pin_generation[idx] = pin.generation;
  }

  Status first_rejection = Status::success();
  bool have_rejection = false;

  std::vector<const PressureSignal*> accepted;
  accepted.reserve(request.signals.size());
  for (const PressureSignal& signal : request.signals) {
    Status st = signal.validate(policy, request.now);
    if (st.ok() && signal.topology_digest != request.topology_digest) {
      st = Status::error(ErrorCode::StaleGeneration, "signal topology digest",
                         signal.origin.value());
    }
    if (st.ok() && signal.epoch != request.epoch) {
      st = Status::error(ErrorCode::StaleEpoch, "signal epoch", signal.epoch.value());
    }
    if (st.ok()) {
      for (const PressureSignal* other : accepted) {
        if (other->source == signal.source) {
          st = Status::error(ErrorCode::Conflict, "duplicate source in request",
                             signal.source.value());
          break;
        }
      }
    }
    if (!st.ok()) {
      RejectionRecord rec;
      rec.signal = signal.id;
      rec.source = signal.source;
      rec.status = st;
      outcome.rejections.push_back(rec);
      ++outcome.counters.signals_rejected;
      if (is_stale_code(st.code())) {
        ++outcome.counters.stale_rejections;
      }
      if (st.code() == ErrorCode::Fenced) {
        ++outcome.counters.fence_rejections;
      }
      if (st.code() == ErrorCode::Unauthorized || st.code() == ErrorCode::UnknownPressure) {
        ++outcome.counters.unauthorized_rejections;
      }
      if (!have_rejection) {
        first_rejection = st;
        have_rejection = true;
      }
      continue;
    }
    accepted.push_back(&signal);
  }

  if (accepted.empty()) {
    outcome.status = first_rejection.ok()
                        ? Status::error(ErrorCode::UnknownPressure, "no admissible signals")
                        : first_rejection;
    outcome.finalize();
    return outcome;
  }

  std::vector<const PressureSignal*> live;
  live.reserve(accepted.size());
  for (const PressureSignal* signal : accepted) {
    if (ledger.has_lineage(signal->lineage)) {
      ++outcome.counters.signals_idempotent;
      PropagationExplanation expl;
      expl.id = request.id;
      expl.signal = signal->id;
      expl.source = signal->source;
      expl.origin = signal->origin;
      expl.source_severity = signal->observation.severity();
      expl.source_magnitude = signal->observation.magnitude();
      expl.lineage = signal->lineage;
      expl.topology_digest = request.topology_digest;
      expl.topology_generation = topo.generation();
      expl.epoch = request.epoch;
      expl.authority = signal->authority;
      expl.issued_at = signal->issued_at;
      expl.deadline = signal->deadline(policy);
      expl.truncated = false;
      expl.finalize();
      outcome.explanations.push_back(expl);
      continue;
    }
    live.push_back(signal);
  }

  Accumulators acc;
  bool truncated = false;

  for (const PressureSignal* signal : live) {
    PropagationExplanation expl;
    expl.id = request.id;
    expl.signal = signal->id;
    expl.source = signal->source;
    expl.origin = signal->origin;
    expl.source_severity = signal->observation.severity();
    expl.source_magnitude = signal->observation.magnitude();
    expl.lineage = signal->lineage;
    expl.topology_digest = request.topology_digest;
    expl.topology_generation = topo.generation();
    expl.epoch = request.epoch;
    expl.authority = signal->authority;
    expl.issued_at = signal->issued_at;
    expl.deadline = signal->deadline(policy);

    const EngineCounters before = outcome.counters;
    const std::uint32_t record_budget =
        request.record_budget_override == 0u
            ? policy.max_records
            : (request.record_budget_override < policy.max_records
                   ? request.record_budget_override
                   : policy.max_records);
    Traversal traversal(ctx, policy, request.now, pin_present, pin_generation, record_budget);
    const Status st = traversal.run(*signal, request.id, acc, outcome.counters, expl, truncated);
    if (!st.ok()) {
      RejectionRecord rec;
      rec.signal = signal->id;
      rec.source = signal->source;
      rec.status = st;
      outcome.rejections.push_back(rec);
      ++outcome.counters.signals_rejected;
      continue;
    }
    ++outcome.counters.signals_accepted;
    ++outcome.counters.propagations;

    expl.counters.signals_received = outcome.counters.signals_received;
    expl.counters.nodes_settled = outcome.counters.nodes_settled - before.nodes_settled;
    expl.counters.hops_considered = outcome.counters.hops_considered - before.hops_considered;
    expl.counters.hops_propagated = outcome.counters.hops_propagated - before.hops_propagated;
    expl.counters.hops_suppressed = outcome.counters.hops_suppressed - before.hops_suppressed;
    expl.counters.loop_preventions = outcome.counters.loop_preventions - before.loop_preventions;
    expl.counters.stale_rejections = outcome.counters.stale_rejections - before.stale_rejections;
    expl.counters.fence_rejections = outcome.counters.fence_rejections - before.fence_rejections;
    expl.counters.unauthorized_rejections =
        outcome.counters.unauthorized_rejections - before.unauthorized_rejections;
    expl.counters.amplifications = outcome.counters.amplifications - before.amplifications;
    expl.counters.budget_exhaustions =
        outcome.counters.budget_exhaustions - before.budget_exhaustions;
    expl.counters.record_truncations =
        outcome.counters.record_truncations - before.record_truncations;
    expl.truncated = truncated;
    expl.finalize();
    outcome.explanations.push_back(std::move(expl));
  }

  if (truncated) {
    outcome.complete = false;
  }

  // --- aggregate ------------------------------------------------------------
  for (auto& pair : acc.by_node) {
    NodeAccumulator& a = pair.second;
    NodeOutcome node;
    node.resource = a.resource;
    node.strongest_single = a.strongest;
    node.received = a.received;
    node.forwarded = a.forwarded;
    node.best_depth = a.best_depth;
    node.best_source = a.best_source;
    node.best_propagation = a.best_propagation;
    node.parent = a.parent;
    node.parent_edge = a.parent_edge;
    node.contributions = a.contributions;
    node.contributions_truncated = a.contributions_truncated;
    node.contribution_count = a.contribution_count;
    node.blocked_reason = a.blocked_reason;
    node.governing_fence = a.governing_fence;

    Magnitude aggregated = aggregate_contributions(policy, a.contributions);
    if (aggregated > policy.aggregation_ceiling) {
      aggregated = policy.aggregation_ceiling;
      ++outcome.counters.clamps;
    }
    node.applied = a.received ? aggregated : Magnitude::zero();
    node.severity = a.received ? severity_from_magnitude(node.applied, policy.thresholds)
                               : Severity::Unknown;
    outcome.nodes.push_back(std::move(node));
  }
  std::sort(outcome.nodes.begin(), outcome.nodes.end(),
            [](const NodeOutcome& a, const NodeOutcome& b) { return a.resource < b.resource; });

  for (const NodeOutcome& n : outcome.nodes) {
    if (n.received && n.best_depth > outcome.peak_depth) {
      outcome.peak_depth = n.best_depth;
    }
  }

  // A request in which every signal was refused reports the first refusal as
  // its status; the per-signal records carry the rest.
  if (outcome.counters.signals_accepted == 0 && !outcome.rejections.empty()) {
    outcome.status = outcome.rejections.front().status;
    outcome.finalize();
    return outcome;
  }

  // --- commit ---------------------------------------------------------------
  if (!request.dry_run && !truncated) {
    for (const PressureSignal* signal : live) {
      ledger.mark_lineage(signal->lineage);
    }
    for (const NodeOutcome& n : outcome.nodes) {
      if (!n.received || n.applied.is_zero()) {
        continue;
      }
      const Status st = ledger.record_application(n.best_source, n.resource, n.applied,
                                                  request.now);
      if (!st.ok()) {
        outcome.status = st;
        outcome.complete = false;
        break;
      }
    }
    for (const PropagationExplanation& expl : outcome.explanations) {
      for (const HopRecord& hop : expl.hops) {
        if (hop.verdict != HopVerdict::Propagated && hop.verdict != HopVerdict::Amplified) {
          continue;
        }
        const Status st = ledger.record_damping(hop.to, hop.edge, hop.potential_after.to_magnitude(),
                                                request.now);
        if (!st.ok()) {
          break;
        }
      }
    }
  }

  outcome.counters.lineage_evictions = ledger.lineage_evictions();
  outcome.finalize();
  return outcome;
}

PropagationOutcome PropagationEngine::recover(const EngineContext& ctx,
                                              const RecoveryRequest& request) {
  PropagationOutcome outcome;
  outcome.id = request.id;
  outcome.epoch = request.epoch;
  outcome.topology_digest = request.topology_digest;
  outcome.now = request.now;

  if (!ctx.valid()) {
    outcome.status = Status::error(ErrorCode::InvalidArgument, "engine context");
    outcome.finalize();
    return outcome;
  }
  const Topology& topo = *ctx.topology;
  const PropagationPolicy& policy = *ctx.policy;
  PropagationLedger& ledger = *ctx.ledger;
  outcome.topology_generation = topo.generation();

  const Status policy_status = policy.validate();
  if (!policy_status.ok()) {
    outcome.status = policy_status;
    outcome.finalize();
    return outcome;
  }
  if (!request.id.valid()) {
    outcome.status = Status::error(ErrorCode::InvalidArgument, "recovery id");
    outcome.finalize();
    return outcome;
  }
  if (!request.source.valid()) {
    outcome.status = Status::error(ErrorCode::InvalidArgument, "recovery source");
    outcome.finalize();
    return outcome;
  }
  if (!request.origin_generation.known()) {
    outcome.status = Status::error(ErrorCode::StaleGeneration, "recovery origin generation");
    outcome.finalize();
    return outcome;
  }
  if (request.topology_digest != topo.digest()) {
    outcome.status = Status::error(ErrorCode::StaleGeneration, "recovery topology digest");
    outcome.finalize();
    return outcome;
  }
  if (request.epoch != ctx.live_epoch) {
    outcome.status = Status::error(ErrorCode::StaleEpoch, "recovery epoch", request.epoch.value());
    outcome.finalize();
    return outcome;
  }
  if (request.authority.level != AuthorityLevel::None) {
    const Status authority_status =
        request.authority.check(request.now, ctx.live_epoch, request.origin);
    if (!authority_status.ok()) {
      outcome.status = authority_status;
      outcome.finalize();
      return outcome;
    }
  }
  const std::uint32_t origin_index = topo.index_of(request.origin);
  if (topo.resource_at(origin_index) == nullptr) {
    outcome.status = Status::error(ErrorCode::NotFound, "recovery origin", request.origin.value());
    outcome.finalize();
    return outcome;
  }

  std::vector<SourceResourceKey> targets;
  targets.reserve(ledger.application_count());
  ledger.for_each_application([&](const SourceResourceKey& key, const PropagationLedger::ApplicationRecord&) {
    if (key.source == request.source) {
      targets.push_back(key);
    }
  });
  std::sort(targets.begin(), targets.end(), [](const SourceResourceKey& a,
                                               const SourceResourceKey& b) {
    return a.resource < b.resource;
  });

  for (const SourceResourceKey& key : targets) {
    const PropagationLedger::ApplicationRecord rec = ledger.application(key.source, key.resource);
    if (!rec.present) {
      continue;
    }
    Tick steps = 0;
    if (request.now > rec.tick) {
      steps = (request.now - rec.tick) / policy.recovery_decay.step_ticks;
    }
    const std::uint32_t bounded_steps = static_cast<std::uint32_t>(
        std::min<Tick>(steps, static_cast<Tick>(policy.recovery_decay.max_steps)));
    Magnitude decayed = policy.recovery_decay.decay(rec.magnitude, bounded_steps);
    if (key.resource == request.origin) {
      decayed = request.residual < decayed ? request.residual : decayed;
    }
    if (decayed > rec.magnitude) {
      decayed = rec.magnitude;
    }
    if (decayed > request.residual) {
      decayed = request.residual;
    }

    NodeOutcome node;
    node.resource = key.resource;
    node.applied = decayed;
    node.received = !decayed.is_zero();
    node.recovery = true;
    node.strongest_single = rec.magnitude;
    node.best_source = key.source;
    node.best_propagation = request.id;
    node.severity = node.received ? severity_from_magnitude(decayed, policy.thresholds)
                                  : Severity::Unknown;
    outcome.recovery_nodes.push_back(std::move(node));

    if (!request.dry_run) {
      const Status st = ledger.record_application(key.source, key.resource, decayed, request.now);
      if (!st.ok()) {
        outcome.status = st;
        outcome.complete = false;
        break;
      }
    }
  }

  outcome.counters.signals_received = 1;
  outcome.counters.signals_accepted = 1;
  outcome.counters.propagations = outcome.recovery_nodes.size();
  outcome.counters.nodes_settled = outcome.recovery_nodes.size();
  outcome.finalize();
  return outcome;
}

}  // namespace backpressure