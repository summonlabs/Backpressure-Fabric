// Backpressure Fabric - synthetic benchmark implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/bench/synthetic.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "backpressure/core/fixed.hpp"
#include "backpressure/core/rng.hpp"
#include "backpressure/engine/engine.hpp"
#include "backpressure/model/policy.hpp"
#include "backpressure/model/topology.hpp"

namespace backpressure {
namespace {

constexpr Generation kGenerated = Generation::initial();

[[nodiscard]] Result<Attenuation> attenuation_for(const SyntheticGraphSpec& spec) {
  return Attenuation::from_percent_milli(spec.attenuation_per_100k);
}

}  // namespace

Result<SyntheticGraph> make_synthetic_graph(const SyntheticGraphSpec& spec) {
  if (spec.nodes == 0 || spec.depth == 0 || spec.fanout == 0 || spec.sources == 0) {
    return fail<SyntheticGraph>(ErrorCode::InvalidArgument, "synthetic graph shape");
  }
  if (spec.sources > spec.nodes) {
    return fail<SyntheticGraph>(ErrorCode::InvalidArgument, "synthetic graph sources");
  }
  BPFAB_TRY_DECL(const Attenuation, attenuation, attenuation_for(spec));

  SyntheticGraph graph;
  graph.depth = spec.depth;
  graph.fanout = spec.fanout;
  graph.sources = spec.sources;

  SplitMix64 rng(derive_seed(spec.seed, "synthetic-graph"));

  auto add_resource = [&graph](std::uint64_t index) {
    Resource resource;
    resource.id = ResourceId(index);
    resource.klass = ResourceClass::QueueGroup;
    resource.protection = ProtectionClass::None;
    resource.definition_generation = kGenerated;
    resource.accepts_propagation = true;
    graph.resources.push_back(resource);
  };

  for (std::uint32_t i = 0; i < spec.sources; ++i) {
    const std::uint64_t id = graph.resources.size() + 1u;
    add_resource(id);
    graph.origins.push_back(ResourceId(id));
  }

  std::vector<std::vector<ResourceId>> levels;
  levels.push_back(graph.origins);

  std::uint64_t next_id = graph.resources.size() + 1u;
  for (std::uint32_t level = 1; level <= spec.depth; ++level) {
    std::vector<ResourceId> current;
    for (const ResourceId parent : levels.back()) {
      for (std::uint32_t child = 0; child < spec.fanout; ++child) {
        if (graph.resources.size() >= spec.nodes) {
          break;
        }
        const std::uint64_t id = next_id++;
        add_resource(id);
        current.push_back(ResourceId(id));
        DependencyEdge edge;
        edge.id = EdgeId(static_cast<std::uint64_t>(graph.edges.size()) + 1u);
        edge.from = parent;
        edge.to = ResourceId(id);
        edge.attenuation = attenuation;
        edge.dependency_generation = kGenerated;
        graph.edges.push_back(edge);
      }
      if (graph.resources.size() >= spec.nodes) {
        break;
      }
    }
    if (current.empty()) {
      break;
    }
    levels.push_back(std::move(current));
    if (graph.resources.size() >= spec.nodes) {
      break;
    }
  }

  // Cycle edges: connect a deep node back to a shallower one.
  std::uint32_t added_cycles = 0;
  std::uint32_t attempts = 0;
  const std::uint32_t max_attempts = spec.cycle_edges * 64u + 64u;
  while (added_cycles < spec.cycle_edges && attempts < max_attempts && levels.size() > 1) {
    ++attempts;
    const std::size_t deep_index = 1u + static_cast<std::size_t>(
                                            rng.next_bounded(static_cast<std::uint64_t>(
                                                levels.size() - 1u)));
    const std::size_t shallow_index = static_cast<std::size_t>(
        rng.next_bounded(static_cast<std::uint64_t>(deep_index)));
    const std::vector<ResourceId>& deep = levels[deep_index];
    const std::vector<ResourceId>& shallow = levels[shallow_index];
    if (deep.empty() || shallow.empty()) {
      continue;
    }
    const ResourceId from = deep[static_cast<std::size_t>(
        rng.next_bounded(static_cast<std::uint64_t>(deep.size())))];
    const ResourceId to = shallow[static_cast<std::size_t>(
        rng.next_bounded(static_cast<std::uint64_t>(shallow.size())))];
    bool duplicate = false;
    for (const DependencyEdge& edge : graph.edges) {
      if (edge.from == from && edge.to == to) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    DependencyEdge edge;
    edge.id = EdgeId(static_cast<std::uint64_t>(graph.edges.size()) + 1u);
    edge.from = from;
    edge.to = to;
    edge.attenuation = attenuation;
    edge.dependency_generation = kGenerated;
    graph.edges.push_back(edge);
    ++added_cycles;
  }
  graph.cycle_edges = added_cycles;
  return Result<SyntheticGraph>(std::move(graph));
}

std::vector<BenchCase> default_benchmark_cases() {
  std::vector<BenchCase> cases;
  {
    BenchCase c;
    c.name = "size-small";
    c.spec.nodes = 128;
    c.spec.depth = 4;
    c.spec.fanout = 3;
    c.iterations = 32;
    cases.push_back(c);
  }
  {
    BenchCase c;
    c.name = "size-large";
    c.spec.nodes = 4096;
    c.spec.depth = 6;
    c.spec.fanout = 4;
    c.iterations = 8;
    cases.push_back(c);
  }
  {
    BenchCase c;
    c.name = "depth-1";
    c.spec.nodes = 2048;
    c.spec.depth = 1;
    c.spec.fanout = 8;
    c.iterations = 16;
    cases.push_back(c);
  }
  {
    BenchCase c;
    c.name = "depth-10";
    c.spec.nodes = 2048;
    c.spec.depth = 10;
    c.spec.fanout = 2;
    c.iterations = 8;
    cases.push_back(c);
  }
  {
    BenchCase c;
    c.name = "fanout-32";
    c.spec.nodes = 2048;
    c.spec.depth = 3;
    c.spec.fanout = 32;
    c.iterations = 8;
    cases.push_back(c);
  }
  {
    BenchCase c;
    c.name = "sources-8";
    c.spec.nodes = 2048;
    c.spec.depth = 5;
    c.spec.fanout = 3;
    c.spec.sources = 8;
    c.iterations = 8;
    cases.push_back(c);
  }
  {
    BenchCase c;
    c.name = "cycles-dense";
    c.spec.nodes = 1024;
    c.spec.depth = 5;
    c.spec.fanout = 3;
    c.spec.cycle_edges = 512;
    c.iterations = 8;
    cases.push_back(c);
  }
  {
    BenchCase c;
    c.name = "attenuation-decay";
    c.spec.nodes = 2048;
    c.spec.depth = 8;
    c.spec.fanout = 3;
    c.spec.attenuation_per_100k = 40000;
    c.iterations = 8;
    cases.push_back(c);
  }
  return cases;
}

Result<std::vector<BenchResult>> run_benchmark(const BenchConfig& config) {
  if (config.cases.empty()) {
    return fail<std::vector<BenchResult>>(ErrorCode::InvalidArgument, "no benchmark cases");
  }
  std::vector<BenchResult> results;
  results.reserve(config.cases.size());

  const PolicyId policy_id(1);
  Result<PropagationPolicy> policy = make_conservative_policy(policy_id, kGenerated);
  if (!policy.ok()) {
    return fail<std::vector<BenchResult>>(policy.status().code(), policy.status().context());
  }
  PropagationPolicy bench_policy = policy.value();
  bench_policy.max_hops = 16;
  bench_policy.max_fanout = 64;
  bench_policy.max_expansions = 1u << 15;
  bench_policy.max_visited = 1u << 14;
  bench_policy.max_records = 2u * bench_policy.max_expansions + bench_policy.max_visited;
  bench_policy.max_path_records = bench_policy.max_records;
  bench_policy.max_sources = 16;
  bench_policy.lifetime_ticks = 1u << 30;
  const Status policy_status = bench_policy.validate();
  if (!policy_status.ok()) {
    return fail<std::vector<BenchResult>>(policy_status.code(), policy_status.context());
  }

  TopologyLimits limits;
  limits.max_resources = 1u << 18;
  limits.max_edges = 1u << 20;
  limits.max_out_degree = 1u << 16;
  limits.max_in_degree = 1u << 16;

  for (const BenchCase& bench_case : config.cases) {
    BenchResult result;
    result.name = bench_case.name;
    Result<SyntheticGraph> graph = make_synthetic_graph(bench_case.spec);
    if (!graph.ok()) {
      result.status = graph.status();
      results.push_back(std::move(result));
      continue;
    }
    SyntheticGraph built = std::move(graph).value();
    result.graph_nodes = built.resources.size();
    result.graph_edges = built.edges.size();
    result.depth = built.depth;
    result.fanout = built.fanout;
    result.sources = built.sources;
    result.cycle_edges = built.cycle_edges;

    if (!config.build_topology) {
      result.status = Status::success();
      result.valid = true;
      results.push_back(std::move(result));
      continue;
    }

    Result<Topology> topology =
        Topology::build(built.resources, built.edges, limits, true);
    if (!topology.ok()) {
      result.status = topology.status();
      results.push_back(std::move(result));
      continue;
    }
    const Topology topo = std::move(topology).value();

    PropagationLedger ledger(1u << 18, 1u << 18, 1u << 18);
    EngineContext ctx;
    ctx.topology = &topo;
    ctx.policy = &bench_policy;
    ctx.ledger = &ledger;
    ctx.fences = nullptr;
    ctx.live_epoch = Epoch(1);

    bool failed = false;
    const std::uint64_t start = steady_nanos();
    for (std::uint32_t iteration = 0; iteration < bench_case.iterations && !failed; ++iteration) {
      std::vector<PressureSignal> signals;
      for (std::size_t s = 0; s < built.origins.size(); ++s) {
        PressureSignal signal;
        signal.id = SignalId(static_cast<std::uint64_t>(iteration) * 1024u + s + 1u);
        signal.source = SourceId(s + 1u);
        signal.origin = built.origins[s];
        signal.observation = PressureObservation::observed(
            Magnitude::from_raw_q16_saturating(52428u), bench_policy.thresholds);
        signal.origin_generation = kGenerated;
        signal.topology_digest = topo.digest();
        signal.epoch = Epoch(1);
        signal.publisher = Incarnation::mint(BootId::from_raw(1u, 1u), 1u,
                                             static_cast<std::uint32_t>(s));
        signal.issued_at = config.start_tick;
        signal.valid_until = config.start_tick + (1u << 24);
        signal.authority = AuthorityVector::make_global(Epoch(1), kGenerated, 16);
        ProvenanceHop hop;
        hop.publisher = signal.publisher;
        hop.epoch = Epoch(1);
        hop.generation = kGenerated;
        hop.tick = config.start_tick;
        signal.provenance.push(hop);
        signal.bind_lineage();
        signals.push_back(signal);
      }

      PropagationRequest request;
      request.id = PropagationId(static_cast<std::uint64_t>(iteration) + 1u);
      request.policy_id = bench_policy.id;
      request.policy_generation = bench_policy.generation;
      request.topology_digest = topo.digest();
      request.epoch = Epoch(1);
      request.now = config.start_tick + iteration;
      request.signals = std::move(signals);

      PropagationOutcome outcome = PropagationEngine::propagate(ctx, request);
      if (!outcome.status.ok()) {
        result.status = outcome.status;
        failed = true;
        break;
      }
      ++result.iterations;
      result.completed_propagations += outcome.counters.propagations;
      result.nodes_settled += outcome.counters.nodes_settled;
      result.hops_propagated += outcome.counters.hops_propagated;
      result.hops_suppressed += outcome.counters.hops_suppressed;
      result.loop_preventions += outcome.counters.loop_preventions;
      result.lineage_evictions = outcome.counters.lineage_evictions;
    }
    const std::uint64_t end = steady_nanos();
    if (!failed) {
      result.status = Status::success();
      result.valid = true;
      result.nanos_total = static_cast<double>(end - start);
      const double seconds = result.nanos_total / 1e9;
      if (seconds > 0.0) {
        result.propagations_per_second =
            static_cast<double>(result.completed_propagations) / seconds;
        result.nodes_per_second = static_cast<double>(result.nodes_settled) / seconds;
      }
    }
    results.push_back(std::move(result));
  }
  return Result<std::vector<BenchResult>>(std::move(results));
}

std::string format_bench_results(const std::vector<BenchResult>& results) {
  std::string out;
  out += "name              nodes  edges  depth fanout src cycles  iters  props     settled    hops     suppr   loops   ms      prop/s\n";
  char line[512];
  for (const BenchResult& r : results) {
    if (!r.valid) {
      std::snprintf(line, sizeof(line), "%-16s  INVALID: %s\n", r.name.c_str(),
                    to_string(r.status.code()));
      out += line;
      continue;
    }
    std::snprintf(line, sizeof(line),
                  "%-16s  %5llu  %5llu  %5u %6u %3u %6u  %5llu  %7llu  %8llu  %7llu  %7llu  %6llu  %8.3f  %10.0f\n",
                  r.name.c_str(), static_cast<unsigned long long>(r.graph_nodes),
                  static_cast<unsigned long long>(r.graph_edges), r.depth, r.fanout, r.sources,
                  r.cycle_edges, static_cast<unsigned long long>(r.iterations),
                  static_cast<unsigned long long>(r.completed_propagations),
                  static_cast<unsigned long long>(r.nodes_settled),
                  static_cast<unsigned long long>(r.hops_propagated),
                  static_cast<unsigned long long>(r.hops_suppressed),
                  static_cast<unsigned long long>(r.loop_preventions),
                  r.nanos_total / 1e6, r.propagations_per_second);
    out += line;
  }
  return out;
}

}  // namespace backpressure