// Backpressure Fabric - independent downstream consumer.
//
// This program only uses the installed public headers and the exported CMake
// target. It drives one real propagation end to end and fails if the observed
// decision does not match the documented invariants.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <memory>
#include <vector>

#include <backpressure/backpressure.hpp>

namespace {

int fail(const char* message) {
  std::printf("consumer failure: %s\n", message);
  std::fflush(stdout);
  return 1;
}

}  // namespace

int main() {
  using namespace backpressure;

  std::printf("Backpressure Fabric %s consumer\n", std::string(kVersionString).c_str());

  std::vector<Resource> resources;
  for (std::uint32_t i = 1; i <= 4; ++i) {
    Resource resource;
    resource.id = ResourceId(i);
    resource.klass = ResourceClass::QueueGroup;
    resource.definition_generation = Generation::initial();
    if (i == 4u) {
      resource.protection = ProtectionClass::Barrier;
    }
    resources.push_back(resource);
  }
  std::vector<DependencyEdge> edges;
  for (std::uint32_t i = 0; i < 3u; ++i) {
    DependencyEdge edge;
    edge.id = EdgeId(i + 1u);
    edge.from = ResourceId(i + 1u);
    edge.to = ResourceId(i + 2u);
    const Result<Attenuation> attenuation = Attenuation::from_percent_milli(60000u);
    if (!attenuation.ok()) {
      return fail("attenuation");
    }
    edge.attenuation = attenuation.value();
    edge.dependency_generation = Generation::initial();
    edges.push_back(edge);
  }

  const Result<Topology> built = Topology::build(resources, edges, TopologyLimits{}, true);
  if (!built.ok()) {
    return fail("topology build");
  }
  const Topology topology = std::move(built).value();

  const Result<PropagationPolicy> policy_result =
      make_conservative_policy(PolicyId(1u), Generation::initial());
  if (!policy_result.ok()) {
    return fail("policy");
  }
  PropagationPolicy policy = policy_result.value();
  policy.max_hops = 4;
  policy.max_records = policy.max_expansions + policy.max_visited;
  policy.max_path_records = policy.max_records;
  policy.lifetime_ticks = 1u << 30;
  if (!policy.validate().ok()) {
    return fail("policy validation");
  }

  PropagationLedger ledger;
  PressureSignal signal;
  signal.id = SignalId(1u);
  signal.source = SourceId(1u);
  signal.origin = ResourceId(1u);
  signal.observation = PressureObservation::observed(
      Magnitude::from_percent_milli(90000u).value(), policy.thresholds);
  signal.origin_generation = Generation::initial();
  signal.topology_digest = topology.digest();
  signal.epoch = Epoch(1);
  signal.publisher = Incarnation::mint(BootId::from_raw(3u, 4u), 1u, 0);
  signal.issued_at = 10;
  signal.authority = AuthorityVector::make_global(Epoch(1), policy.generation, 4u);
  ProvenanceHop hop;
  hop.publisher = signal.publisher;
  hop.epoch = Epoch(1);
  hop.generation = Generation::initial();
  hop.tick = 10;
  signal.provenance.push(hop);
  signal.bind_lineage();

  EngineContext context;
  context.topology = &topology;
  context.policy = &policy;
  context.ledger = &ledger;
  context.live_epoch = Epoch(1);

  PropagationRequest request;
  request.id = PropagationId(1u);
  request.policy_id = policy.id;
  request.policy_generation = policy.generation;
  request.topology_digest = topology.digest();
  request.epoch = Epoch(1);
  request.now = 10;
  request.signals.push_back(signal);

  const PropagationOutcome outcome = PropagationEngine::propagate(context, request);
  if (!outcome.status.ok()) {
    return fail("propagation status");
  }
  if (outcome.nodes.size() != 4u) {
    return fail("expected four settled resources");
  }
  bool saw_barrier = false;
  for (const NodeOutcome& node : outcome.nodes) {
    if (node.applied > signal.observation.magnitude()) {
      return fail("pressure increased through propagation");
    }
    if (node.resource == ResourceId(4u)) {
      saw_barrier = true;
      if (node.forwarded) {
        return fail("a barrier resource forwarded pressure");
      }
    }
  }
  if (!saw_barrier) {
    return fail("barrier resource was not settled");
  }
  std::printf("propagation ok: %zu resources, fingerprint %s\n", outcome.nodes.size(),
              to_hex(outcome.fingerprint).c_str());

  const std::vector<BenchCase> cases = default_benchmark_cases();
  if (cases.empty()) {
    return fail("benchmark cases");
  }

  BenchConfig bench;
  bench.cases.push_back(cases.front());
  bench.cases.front().iterations = 1;
  const Result<std::vector<BenchResult>> measured = run_benchmark(bench);
  if (!measured.ok() || measured.value().empty() || !measured.value().front().synthetic) {
    return fail("benchmark");
  }
  std::printf("synthetic benchmark ok: %llu completed propagations\n",
              static_cast<unsigned long long>(measured.value().front().completed_propagations));
  std::printf("consumer ok\n");
  return 0;
}