// Backpressure Fabric - seeded randomized property tests.
//
// Every population here is generated from a seed and is fully reproducible.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;
using bpfab_test::find_node;
using bpfab_test::make_edge;
using bpfab_test::make_resource;
using bpfab_test::run_propagation;

namespace {

struct RandomGraph {
  std::vector<Resource> resources;
  std::vector<DependencyEdge> edges;
  std::vector<ResourceId> origins;
};

RandomGraph generate_graph(std::uint64_t seed, std::uint32_t nodes, std::uint32_t edges,
                           std::uint32_t origins) {
  SplitMix64 rng(seed);
  RandomGraph graph;
  for (std::uint32_t i = 1; i <= nodes; ++i) {
    const std::uint32_t roll = static_cast<std::uint32_t>(rng.next_bounded(100u));
    ProtectionClass protection = ProtectionClass::None;
    if (roll < 3u) {
      protection = ProtectionClass::Barrier;
    } else if (roll < 5u) {
      protection = ProtectionClass::Sealed;
    } else if (roll < 7u) {
      protection = ProtectionClass::ObservedOnly;
    }
    graph.resources.push_back(make_resource(i, protection));
  }
  std::uint64_t edge_id = 1;
  for (std::uint32_t i = 0; i < edges; ++i) {
    const std::uint64_t from = rng.next_bounded(nodes) + 1u;
    const std::uint64_t to = rng.next_bounded(nodes) + 1u;
    bool duplicate = false;
    for (const DependencyEdge& existing : graph.edges) {
      if (existing.from == ResourceId(from) && existing.to == ResourceId(to)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    const std::uint32_t attenuation = static_cast<std::uint32_t>(rng.next_bounded(100001u));
    const std::uint32_t hops = static_cast<std::uint32_t>(rng.next_bounded(5u));
    const std::uint64_t cooldown = rng.next_bounded(3u);
    graph.edges.push_back(make_edge(edge_id++, from, to, attenuation, hops, cooldown));
  }
  for (std::uint32_t i = 0; i < origins; ++i) {
    graph.origins.push_back(ResourceId(rng.next_bounded(nodes) + 1u));
  }
  return graph;
}

}  // namespace

BPFAB_TEST(property, bounded_traversal_invariants_hold_over_random_graphs) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    const RandomGraph graph = generate_graph(seed, 40u, 90u, 3u);
    BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(graph.resources, graph.edges, true));

    PropagationPolicy policy = bpfab_test::test_policy();
    policy.max_hops = 4;
    policy.max_fanout = 6;
    policy.max_expansions = 128;
    policy.max_visited = 64;
    policy.max_sources = 3;
    policy.max_records = 2u * policy.max_expansions + policy.max_visited;
    policy.max_path_records = policy.max_records;
    BPFAB_REQUIRE_OK(policy.validate());

    PropagationLedger ledger(1024, 1024, 1024);
    std::vector<PressureSignal> signals;
    std::uint64_t source_id = 0;
    for (const ResourceId origin : graph.origins) {
      ++source_id;
      signals.push_back(bpfab_test::make_signal(
          source_id, source_id, origin.value(), topo.digest(), Epoch(1), policy,
          static_cast<std::uint32_t>(50000u + source_id * 5000u), 100u, AuthorityLevel::Global,
          4u));
    }

    const PropagationOutcome outcome =
        run_propagation(topo, policy, ledger, Epoch(1), 100, signals, nullptr, seed);
    BPFAB_REQUIRE_OK(outcome.status);
    BPFAB_CHECK(outcome.counters.nodes_settled <=
                static_cast<std::uint64_t>(policy.max_visited) * signals.size());
    BPFAB_CHECK(outcome.counters.hops_propagated <=
                static_cast<std::uint64_t>(policy.max_expansions) * signals.size());
    BPFAB_CHECK(outcome.peak_depth <= policy.max_hops);
    BPFAB_CHECK(outcome.complete);

    std::vector<ResourceId> seen;
    for (const NodeOutcome& node : outcome.nodes) {
      BPFAB_CHECK(node.applied <= Magnitude::full());
      BPFAB_CHECK(node.best_depth <= policy.max_hops);
      BPFAB_CHECK(node.contribution_count <= signals.size());
      BPFAB_CHECK(node.contributions.size() <= 16u);
      if (node.received) {
        for (const Contribution& contribution : node.contributions) {
          BPFAB_CHECK(contribution.magnitude <= Magnitude::full());
        }
        BPFAB_CHECK(node.applied <= policy.aggregation_ceiling);
      }
      seen.push_back(node.resource);
    }
    std::sort(seen.begin(), seen.end());
    BPFAB_CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());

    // Re-running the same request with a fresh ledger must reproduce the
    // decision exactly.
    PropagationLedger second_ledger(1024, 1024, 1024);
    const PropagationOutcome repeated =
        run_propagation(topo, policy, second_ledger, Epoch(1), 100, signals, nullptr, seed);
    BPFAB_REQUIRE_OK(repeated.status);
    BPFAB_CHECK(repeated.fingerprint == outcome.fingerprint);
    BPFAB_CHECK(repeated.nodes.size() == outcome.nodes.size());
  }
}

BPFAB_TEST(property, pressure_never_increases_through_propagation) {
  for (std::uint64_t seed = 100; seed <= 130; ++seed) {
    const RandomGraph graph = generate_graph(seed, 24u, 60u, 1u);
    BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(graph.resources, graph.edges, true));
    PropagationPolicy policy = bpfab_test::test_policy();
    policy.max_hops = 6;
    BPFAB_REQUIRE_OK(policy.validate());

    PropagationLedger ledger(512, 512, 512);
    SplitMix64 rng(seed);
    const std::uint32_t magnitude = static_cast<std::uint32_t>(rng.next_bounded(100001u));
    PressureSignal signal = bpfab_test::make_signal(1u, 1u, graph.origins[0].value(), topo.digest(),
                                                    Epoch(1), policy, magnitude, 100u,
                                                    AuthorityLevel::Global, 6u);
    const PropagationOutcome outcome =
        run_propagation(topo, policy, ledger, Epoch(1), 100, {signal}, nullptr, seed);
    BPFAB_REQUIRE_OK(outcome.status);
    for (const NodeOutcome& node : outcome.nodes) {
      BPFAB_CHECK(node.applied <= signal.observation.magnitude());
      BPFAB_CHECK(node.strongest_single <= signal.observation.magnitude());
    }
    for (const PropagationExplanation& explanation : outcome.explanations) {
      for (const HopRecord& hop : explanation.hops) {
        if (hop.verdict == HopVerdict::Propagated) {
          BPFAB_CHECK(hop.potential_after <= hop.potential_before);
        }
        BPFAB_CHECK(hop.cumulative_gain_q16 <= 65536u);
      }
    }
  }
}

BPFAB_TEST(property, malformed_requests_are_always_refused_without_mutation) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(4u, 70000u));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(256, 256, 256);
  EngineContext ctx;
  ctx.topology = &topo;
  ctx.policy = &policy;
  ctx.ledger = &ledger;
  ctx.live_epoch = Epoch(1);

  SplitMix64 rng(4242u);
  for (std::uint32_t iteration = 0; iteration < 200u; ++iteration) {
    PressureSignal signal = bpfab_test::make_signal(
        iteration + 1u, 1u, 1u, topo.digest(), Epoch(1), policy,
        static_cast<std::uint32_t>(rng.next_bounded(100001u)), 100u);
    switch (rng.next_bounded(6u)) {
      case 0:
        signal.epoch = Epoch::none();
        break;
      case 1:
        signal.topology_digest = Digest128{};
        break;
      case 2:
        signal.observation = PressureObservation::unknown();
        break;
      case 3:
        signal.authority.level = AuthorityLevel::None;
        break;
      case 4:
        signal.provenance = Provenance{};
        break;
      default:
        signal.valid_until = 1u;
        break;
    }
    signal.bind_lineage();

    PropagationRequest request;
    request.id = PropagationId(iteration + 1u);
    request.policy_id = policy.id;
    request.policy_generation = policy.generation;
    request.topology_digest = topo.digest();
    request.epoch = Epoch(1);
    request.now = 100;
    request.signals = {signal};

    const PropagationOutcome outcome = PropagationEngine::propagate(ctx, request);
    BPFAB_CHECK(!outcome.status.ok());
    BPFAB_CHECK(outcome.nodes.empty());
    BPFAB_CHECK(ledger.lineage_count() == 0u);
    BPFAB_CHECK(ledger.application_count() == 0u);
  }
}

BPFAB_TEST(property, zero_magnitude_sources_produce_no_effect) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(3u, 90000u));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 0u,
                                                  100u);
  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_CODE(outcome.status, ErrorCode::UnknownPressure);
  BPFAB_CHECK(outcome.nodes.empty());
  BPFAB_CHECK(ledger.application_count() == 0u);
}
