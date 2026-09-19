#pragma once

// Backpressure Fabric - shared test builders.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"

namespace bpfab_test {

inline backpressure::Resource make_resource(
    std::uint64_t id,
    backpressure::ProtectionClass protection = backpressure::ProtectionClass::None,
    backpressure::ResourceClass klass = backpressure::ResourceClass::QueueGroup,
    bool accepts = true) {
  backpressure::Resource resource;
  resource.id = backpressure::ResourceId(id);
  resource.klass = klass;
  resource.protection = protection;
  resource.definition_generation = backpressure::Generation::initial();
  resource.accepts_propagation = accepts;
  return resource;
}

inline backpressure::DependencyEdge make_edge(
    std::uint64_t id, std::uint64_t from, std::uint64_t to, std::uint32_t attenuation_milli = 800,
    std::uint32_t max_hops = 0, std::uint64_t cooldown = 0,
    backpressure::EdgeFlags flags = backpressure::EdgeFlags::none(),
    std::uint32_t amplification_milli = 100000) {
  backpressure::DependencyEdge edge;
  edge.id = backpressure::EdgeId(id);
  edge.from = backpressure::ResourceId(from);
  edge.to = backpressure::ResourceId(to);
  const auto attenuation = backpressure::Attenuation::from_percent_milli(attenuation_milli);
  edge.attenuation = attenuation.ok() ? attenuation.value() : backpressure::Attenuation::none();
  if (amplification_milli > 100000) {
    const auto gain = backpressure::Gain::from_percent_milli(amplification_milli);
    if (gain.ok()) {
      edge.amplification = gain.value();
    }
  }
  edge.max_hops = max_hops;
  edge.dependency_generation = backpressure::Generation::initial();
  edge.cooldown_ticks = cooldown;
  edge.flags = flags;
  return edge;
}

inline backpressure::TopologyLimits default_limits() {
  backpressure::TopologyLimits limits;
  limits.max_resources = 1u << 14;
  limits.max_edges = 1u << 16;
  limits.max_out_degree = 4096;
  limits.max_in_degree = 4096;
  return limits;
}

inline backpressure::Result<backpressure::Topology> build_topology(
    std::vector<backpressure::Resource> resources,
    std::vector<backpressure::DependencyEdge> edges, bool allow_cycles = true) {
  return backpressure::Topology::build(std::move(resources), std::move(edges), default_limits(),
                                       allow_cycles);
}

inline backpressure::PropagationPolicy test_policy(std::uint64_t id = 1,
                                                   std::uint64_t generation = 1) {
  const auto policy = backpressure::make_conservative_policy(backpressure::PolicyId(id),
                                                             backpressure::Generation::from_raw(generation));
  backpressure::PropagationPolicy out = policy.ok() ? policy.value()
                                                    : backpressure::PropagationPolicy{};
  out.max_hops = 8;
  out.max_fanout = 32;
  out.max_expansions = 2048;
  out.max_visited = 512;
  out.max_records = 2u * out.max_expansions + out.max_visited;
  out.max_path_records = out.max_records;
  out.max_sources = 8;
  out.lifetime_ticks = 1u << 30;
  out.min_propagatable = backpressure::Magnitude::from_raw_q16_saturating(1000u);
  return out;
}

inline backpressure::Incarnation test_incarnation(std::uint64_t seed = 1,
                                                  std::uint32_t pid = 4242) {
  return backpressure::Incarnation::mint(
      backpressure::BootId::from_raw(seed * 0x9E3779B97F4A7C15ull + 1u, seed + 7u), pid, 0);
}

inline backpressure::PressureSignal make_signal(
    std::uint64_t id, std::uint64_t source, std::uint64_t origin,
    const backpressure::Digest128& topology_digest, backpressure::Epoch epoch,
    const backpressure::PropagationPolicy& policy, std::uint32_t magnitude_milli = 60000,
    std::uint64_t issued_at = 100, backpressure::AuthorityLevel level = backpressure::AuthorityLevel::Global,
    std::uint32_t max_hops = 8, const backpressure::Incarnation& incarnation = test_incarnation()) {
  backpressure::PressureSignal signal;
  signal.id = backpressure::SignalId(id);
  signal.source = backpressure::SourceId(source);
  signal.origin = backpressure::ResourceId(origin);
  signal.observation = backpressure::PressureObservation::observed(
      backpressure::Magnitude::from_raw_q16_saturating(
          static_cast<std::uint32_t>((static_cast<std::uint64_t>(magnitude_milli) * 65536u) / 100000u)),
      policy.thresholds);
  signal.origin_generation = backpressure::Generation::initial();
  signal.topology_digest = topology_digest;
  signal.epoch = epoch;
  signal.publisher = incarnation;
  signal.issued_at = issued_at;
  signal.valid_until = 0;
  signal.authority = backpressure::AuthorityVector::make_global(epoch, policy.generation, max_hops);
  signal.authority.level = level;
  if (level == backpressure::AuthorityLevel::Scoped) {
    // The scope is filled in by the caller when needed.
    const backpressure::Status scoped =
        signal.authority.scope.add(backpressure::ResourceId(origin));
    (void)scoped;
  }
  backpressure::ProvenanceHop hop;
  hop.publisher = incarnation;
  hop.epoch = epoch;
  hop.generation = backpressure::Generation::initial();
  hop.tick = issued_at;
  signal.provenance.push(hop);
  signal.bind_lineage();
  return signal;
}

/// Magnitude helper expressed in per mille.
inline backpressure::Magnitude mag(std::uint32_t milli) {
  return backpressure::Magnitude::from_raw_q16_saturating(
      static_cast<std::uint32_t>((static_cast<std::uint64_t>(milli) * 65536u) / 100000u));
}

inline backpressure::PropagationOutcome run_propagation(
    const backpressure::Topology& topology, const backpressure::PropagationPolicy& policy,
    backpressure::PropagationLedger& ledger, backpressure::Epoch epoch, backpressure::Tick now,
    std::vector<backpressure::PressureSignal> signals,
    const backpressure::FenceTable* fences = nullptr, std::uint64_t propagation_id = 1,
    std::uint32_t record_budget = 0,
    std::vector<backpressure::DependencyPin> pins = {},
    bool dry_run = false) {
  backpressure::EngineContext ctx;
  ctx.topology = &topology;
  ctx.policy = &policy;
  ctx.ledger = &ledger;
  ctx.fences = fences;
  ctx.live_epoch = epoch;

  backpressure::PropagationRequest request;
  request.id = backpressure::PropagationId(propagation_id);
  request.policy_id = policy.id;
  request.policy_generation = policy.generation;
  request.topology_digest = topology.digest();
  request.epoch = epoch;
  request.now = now;
  request.signals = std::move(signals);
  request.record_budget_override = record_budget;
  request.pins = std::move(pins);
  request.dry_run = dry_run;
  return backpressure::PropagationEngine::propagate(ctx, request);
}

/// The effective error code of a fabric publish: the Result status when the
/// call itself failed, otherwise the outcome's own status.
inline backpressure::ErrorCode outcome_code(
    const backpressure::Result<backpressure::PropagationOutcome>& result) {
  return result.ok() ? result.value().status.code() : result.status().code();
}

inline const backpressure::NodeOutcome* find_node(
    const backpressure::PropagationOutcome& outcome, std::uint64_t resource) {
  for (const backpressure::NodeOutcome& node : outcome.nodes) {
    if (node.resource == backpressure::ResourceId(resource)) {
      return &node;
    }
  }
  return nullptr;
}

inline const backpressure::HopRecord* find_hop(const backpressure::PropagationExplanation& expl,
                                               std::uint64_t from, std::uint64_t to) {
  for (const backpressure::HopRecord& hop : expl.hops) {
    if (hop.from == backpressure::ResourceId(from) && hop.to == backpressure::ResourceId(to)) {
      return &hop;
    }
  }
  return nullptr;
}

inline bool has_suppression(const backpressure::PropagationExplanation& expl,
                            backpressure::SuppressionReason reason) {
  for (const backpressure::SuppressedEdgeRecord& record : expl.suppressed) {
    if (record.reason == reason) {
      return true;
    }
  }
  return false;
}

/// Build a layered chain 1 -> 2 -> ... -> count with the given attenuation.
inline backpressure::Result<backpressure::Topology> build_chain(std::uint32_t count,
                                                               std::uint32_t attenuation_milli,
                                                               std::uint32_t max_hops = 0) {
  std::vector<backpressure::Resource> resources;
  std::vector<backpressure::DependencyEdge> edges;
  for (std::uint32_t i = 1; i <= count; ++i) {
    resources.push_back(make_resource(i));
  }
  for (std::uint32_t i = 1; i < count; ++i) {
    edges.push_back(make_edge(i, i, i + 1u, attenuation_milli, max_hops));
  }
  return build_topology(std::move(resources), std::move(edges));
}

}  // namespace bpfab_test