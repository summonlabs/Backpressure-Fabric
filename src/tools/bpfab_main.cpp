// Backpressure Fabric - command line front end.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "backpressure/backpressure.hpp"

namespace {

using namespace backpressure;

void print_usage() {
  std::printf(
      "bpfab %s - Backpressure Fabric command line front end\n"
      "\n"
      "usage: bpfab <command> [options]\n"
      "\n"
      "commands:\n"
      "  version                     print build and format version\n"
      "  selftest                    run a bounded end-to-end propagation scenario\n"
      "  explain                     print a propagation explanation for a demo graph\n"
      "  bench [--cases=N]           run the SYNTHETIC propagation benchmark\n"
      "  store-inspect <directory>   replay and describe a durable state directory\n"
      "  help                        print this message\n",
      std::string(kVersionString).c_str());
}

int command_version() {
  std::printf("Backpressure Fabric %s (format version %u)\n",
              std::string(kVersionString).c_str(),
              static_cast<unsigned>(kStateFormatVersion));
  std::printf("propagation state format %u\n", static_cast<unsigned>(kStateFormatVersion));
  return 0;
}

struct Demo {
  Topology topology;
  PropagationPolicy policy;
  static Result<Demo> make() {
    std::vector<Resource> resources;
    std::vector<DependencyEdge> edges;
    for (std::uint32_t i = 1; i <= 5; ++i) {
      Resource resource;
      resource.id = ResourceId(i);
      resource.klass = ResourceClass::QueueGroup;
      resource.definition_generation = Generation::initial();
      if (i == 4u) {
        resource.protection = ProtectionClass::Barrier;
      }
      resources.push_back(resource);
    }
    const std::uint32_t attenuation[4] = {70000u, 60000u, 50000u, 90000u};
    for (std::uint32_t i = 0; i < 4u; ++i) {
      DependencyEdge edge;
      edge.id = EdgeId(i + 1u);
      edge.from = ResourceId(i + 1u);
      edge.to = ResourceId(i + 2u);
      edge.attenuation = Attenuation::from_percent_milli(attenuation[i]).value();
      edge.dependency_generation = Generation::initial();
      edges.push_back(edge);
    }
    DependencyEdge cycle;
    cycle.id = EdgeId(5u);
    cycle.from = ResourceId(5u);
    cycle.to = ResourceId(2u);
    cycle.attenuation = Attenuation::from_percent_milli(20000u).value();
    cycle.dependency_generation = Generation::initial();
    edges.push_back(cycle);

    Result<Topology> built = Topology::build(resources, edges, TopologyLimits{}, true);
    if (!built.ok()) {
      return fail<Demo>(built.status().code(), built.status().context());
    }
    Demo demo{std::move(built).value(), make_conservative_policy(PolicyId(1u), Generation::initial()).value()};
    demo.policy.max_hops = 8;
    demo.policy.max_records = 2u * demo.policy.max_expansions + demo.policy.max_visited;
    demo.policy.max_path_records = demo.policy.max_records;
    demo.policy.lifetime_ticks = 1u << 30;
    const Status valid = demo.policy.validate();
    if (!valid.ok()) {
      return fail<Demo>(valid.code(), valid.context());
    }
    return Result<Demo>(std::move(demo));
  }
};

std::string render_outcome(const PropagationOutcome& outcome);

int command_explain() {
  Result<Demo> demo = Demo::make();
  if (!demo.ok()) {
    std::fprintf(stderr, "demo setup failed: %s\n", to_string(demo.status().code()));
    return 1;
  }
  Demo built = std::move(demo).value();

  PropagationLedger ledger;
  PressureSignal signal;
  signal.id = SignalId(1u);
  signal.source = SourceId(1u);
  signal.origin = ResourceId(1u);
  signal.observation = PressureObservation::observed(
      Magnitude::from_percent_milli(92000u).value(), built.policy.thresholds);
  signal.origin_generation = Generation::initial();
  signal.topology_digest = built.topology.digest();
  signal.epoch = Epoch(1);
  signal.publisher = Incarnation::mint(BootId::from_raw(0x11u, 0x22u), 1u, 0);
  signal.issued_at = 10;
  signal.authority = AuthorityVector::make_global(Epoch(1), built.policy.generation, 8u);
  ProvenanceHop hop;
  hop.publisher = signal.publisher;
  hop.epoch = Epoch(1);
  hop.generation = Generation::initial();
  hop.tick = 10;
  signal.provenance.push(hop);
  signal.bind_lineage();

  EngineContext ctx;
  ctx.topology = &built.topology;
  ctx.policy = &built.policy;
  ctx.ledger = &ledger;
  ctx.live_epoch = Epoch(1);

  PropagationRequest request;
  request.id = PropagationId(1u);
  request.policy_id = built.policy.id;
  request.policy_generation = built.policy.generation;
  request.topology_digest = built.topology.digest();
  request.epoch = Epoch(1);
  request.now = 10;
  request.signals.push_back(signal);

  const PropagationOutcome outcome = PropagationEngine::propagate(ctx, request);
  std::printf("%s", render_outcome(outcome).c_str());
  return outcome.status.ok() ? 0 : 1;
}

std::string render_outcome(const PropagationOutcome& outcome) {
  std::string out;
  char line[256];
  std::snprintf(line, sizeof(line), "status            : %s\n", to_string(outcome.status.code()));
  out += line;
  std::snprintf(line, sizeof(line), "propagation       : %llu\n",
                static_cast<unsigned long long>(outcome.id.value()));
  out += line;
  std::snprintf(line, sizeof(line), "epoch             : %llu\n",
                static_cast<unsigned long long>(outcome.epoch.value()));
  out += line;
  std::snprintf(line, sizeof(line), "topology gen      : %llu\n",
                static_cast<unsigned long long>(outcome.topology_generation.value()));
  out += line;
  std::snprintf(line, sizeof(line), "topology digest   : %s\n", to_hex(outcome.topology_digest).c_str());
  out += line;
  std::snprintf(line, sizeof(line), "result fingerprint: %s\n", to_hex(outcome.fingerprint).c_str());
  out += line;
  std::snprintf(line, sizeof(line), "complete          : %s\n", outcome.complete ? "yes" : "no");
  out += line;

  out += "\nresource outcomes\n";
  for (const NodeOutcome& node : outcome.nodes) {
    std::snprintf(line, sizeof(line),
                  "  %-6llu applied=%5u millis severity=%-9s depth=%u source=%llu parent=%llu\n",
                  static_cast<unsigned long long>(node.resource.value()),
                  node.applied.as_percent_milli(), to_string(node.severity), node.best_depth,
                  static_cast<unsigned long long>(node.best_source.value()),
                  static_cast<unsigned long long>(node.parent.value()));
    out += line;
  }

  for (const PropagationExplanation& explanation : outcome.explanations) {
    out += "\npropagation path\n";
    std::snprintf(line, sizeof(line), "  signal=%llu source=%llu lineage=%s\n",
                  static_cast<unsigned long long>(explanation.signal.value()),
                  static_cast<unsigned long long>(explanation.source.value()),
                  to_hex(explanation.lineage).c_str());
    out += line;
    std::snprintf(line, sizeof(line), "  authority=%s epoch=%llu hops=%u ceiling=%u millis\n",
                  to_string(explanation.authority.level),
                  static_cast<unsigned long long>(explanation.authority.epoch.value()),
                  explanation.authority.max_hops_granted,
                  explanation.effective_ceiling.as_percent_milli());
    out += line;
    std::snprintf(line, sizeof(line), "  granted gain=%u q16 amplification=%s\n",
                  explanation.granted_gain_q16,
                  explanation.amplification_granted ? "granted" : "denied");
    out += line;
    for (const HopRecord& hop : explanation.hops) {
      std::snprintf(line, sizeof(line),
                    "  hop %llu -> %llu edge=%llu depth=%u attenuation=%u millis "
                    "potential %llu -> %llu %s\n",
                    static_cast<unsigned long long>(hop.from.value()),
                    static_cast<unsigned long long>(hop.to.value()),
                    static_cast<unsigned long long>(hop.edge.value()), hop.depth,
                    hop.attenuation.as_percent_milli(),
                    static_cast<unsigned long long>(hop.potential_before.raw()),
                    static_cast<unsigned long long>(hop.potential_after.raw()),
                    to_string(hop.verdict));
      out += line;
    }
    if (!explanation.suppressed.empty()) {
      out += "  suppressed edges\n";
      for (const SuppressedEdgeRecord& record : explanation.suppressed) {
        std::snprintf(line, sizeof(line), "    %llu -> %llu edge=%llu depth=%u reason=%s\n",
                      static_cast<unsigned long long>(record.from.value()),
                      static_cast<unsigned long long>(record.to.value()),
                      static_cast<unsigned long long>(record.edge.value()), record.depth,
                      to_string(record.reason));
        out += line;
      }
    }
  }

  const EngineCounters& counters = outcome.counters;
  out += "\ncounters\n";
  std::snprintf(line, sizeof(line),
                "  accepted=%llu rejected=%llu idempotent=%llu settled=%llu propagated=%llu "
                "suppressed=%llu loops=%llu stale=%llu fences=%llu unauthorized=%llu "
                "budget=%llu\n",
                static_cast<unsigned long long>(counters.signals_accepted),
                static_cast<unsigned long long>(counters.signals_rejected),
                static_cast<unsigned long long>(counters.signals_idempotent),
                static_cast<unsigned long long>(counters.nodes_settled),
                static_cast<unsigned long long>(counters.hops_propagated),
                static_cast<unsigned long long>(counters.hops_suppressed),
                static_cast<unsigned long long>(counters.loop_preventions),
                static_cast<unsigned long long>(counters.stale_rejections),
                static_cast<unsigned long long>(counters.fence_rejections),
                static_cast<unsigned long long>(counters.unauthorized_rejections),
                static_cast<unsigned long long>(counters.budget_exhaustions));
  out += line;
  return out;
}

int command_bench(int argc, char** argv) {
  std::size_t case_limit = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--cases=", 0) == 0) {
      case_limit = static_cast<std::size_t>(std::strtoul(arg.substr(8).c_str(), nullptr, 10));
    }
  }
  BenchConfig config;
  config.cases = default_benchmark_cases();
  if (case_limit > 0 && case_limit < config.cases.size()) {
    config.cases.resize(case_limit);
  }
  std::printf("Backpressure Fabric synthetic propagation benchmark\n");
  std::printf("population: SYNTHETIC generated graphs; no physical network is measured\n\n");
  Result<std::vector<BenchResult>> results = run_benchmark(config);
  if (!results.ok()) {
    std::fprintf(stderr, "benchmark failed: %s\n", to_string(results.status().code()));
    return 1;
  }
  std::printf("%s", format_bench_results(results.value()).c_str());
  for (const BenchResult& result : results.value()) {
    if (!result.valid) {
      return 1;
    }
  }
  return 0;
}

int command_store_inspect(const std::string& directory) {
  DurableConfig config;
  config.directory = directory;
  Result<DurableStore> store = DurableStore::open(config);
  if (!store.ok()) {
    std::fprintf(stderr, "cannot open state directory: %s\n", to_string(store.status().code()));
    return 1;
  }
  const RecoveredState& state = store.value().state();
  std::printf("journal records      : %llu\n",
              static_cast<unsigned long long>(state.records_replayed));
  std::printf("discarded tail       : %llu\n",
              static_cast<unsigned long long>(state.corrupt_records));
  std::printf("repaired on open     : %s\n", state.journal_repaired ? "yes" : "no");
  std::printf("previous epoch       : %llu\n",
              static_cast<unsigned long long>(state.previous_epoch.value()));
  std::printf("epoch in force       : %llu\n",
              static_cast<unsigned long long>(state.epoch.value()));
  std::printf("boot recorded        : %s\n", state.boot_recorded ? "yes" : "no");
  std::printf("policy present       : %s\n", state.has_policy ? "yes" : "no");
  std::printf("topology binding     : %s\n", state.has_topology_binding ? "yes" : "no");
  std::printf("fences               : %zu\n", state.fences.size());
  std::printf("retained lineages    : %zu\n", state.lineages.size());
  std::printf("lineage evictions    : %llu\n",
              static_cast<unsigned long long>(state.lineage_evictions));
  std::printf("revalidation needed  : %zu\n", state.revalidation_required.size());
  std::printf("attempts open        : %llu\n",
              static_cast<unsigned long long>(state.attempts_open));
  std::printf("attempts ambiguous   : %llu\n",
              static_cast<unsigned long long>(state.attempts_ambiguous));
  std::printf("attempts committed   : %llu\n",
              static_cast<unsigned long long>(state.attempts_committed));
  std::printf("attempts aborted     : %llu\n",
              static_cast<unsigned long long>(state.attempts_aborted));
  std::printf("restored liveness    : %s\n", state.restored_liveness ? "yes" : "no");
  return 0;
}

int command_selftest() {
  Demo demo = Demo::make().value();
  PropagationLedger ledger;
  EngineContext ctx;
  ctx.topology = &demo.topology;
  ctx.policy = &demo.policy;
  ctx.ledger = &ledger;
  ctx.live_epoch = Epoch(1);

  struct Step {
    const char* name;
    std::uint32_t magnitude_milli;
    std::uint64_t issued_at;
  };
  const Step steps[3] = {
      {"initial", 92000u, 10u},
      {"replay", 92000u, 10u},
      {"lower", 20000u, 40u},
  };

  std::printf("selftest: bounded propagation over a cyclic demo topology\n\n");
  bool ok = true;
  for (const Step& step : steps) {
    PressureSignal signal;
    signal.id = SignalId(step.issued_at);
    signal.source = SourceId(1u);
    signal.origin = ResourceId(1u);
    signal.observation = PressureObservation::observed(
        Magnitude::from_percent_milli(step.magnitude_milli).value(), demo.policy.thresholds);
    signal.origin_generation = Generation::initial();
    signal.topology_digest = demo.topology.digest();
    signal.epoch = Epoch(1);
    signal.publisher = Incarnation::mint(BootId::from_raw(0x11u, 0x22u), 1u, 0);
    signal.issued_at = step.issued_at;
    signal.valid_until = step.issued_at + 1000u;
    signal.authority = AuthorityVector::make_global(Epoch(1), demo.policy.generation, 8u);
    ProvenanceHop hop;
    hop.publisher = signal.publisher;
    hop.epoch = Epoch(1);
    hop.generation = Generation::initial();
    hop.tick = step.issued_at;
    signal.provenance.push(hop);
    signal.bind_lineage();

    PropagationRequest request;
    request.id = PropagationId(step.issued_at);
    request.policy_id = demo.policy.id;
    request.policy_generation = demo.policy.generation;
    request.topology_digest = demo.topology.digest();
    request.epoch = Epoch(1);
    request.now = step.issued_at;
    request.signals.push_back(signal);

    const PropagationOutcome outcome = PropagationEngine::propagate(ctx, request);
    std::printf("step %-8s status=%-16s accepted=%llu idempotent=%llu settled=%llu loops=%llu\n",
                step.name, to_string(outcome.status.code()),
                static_cast<unsigned long long>(outcome.counters.signals_accepted),
                static_cast<unsigned long long>(outcome.counters.signals_idempotent),
                static_cast<unsigned long long>(outcome.counters.nodes_settled),
                static_cast<unsigned long long>(outcome.counters.loop_preventions));
    if (!outcome.status.ok()) {
      ok = false;
    }
    for (const NodeOutcome& node : outcome.nodes) {
      if (node.applied > signal.observation.magnitude()) {
        ok = false;
      }
    }
  }
  std::printf("\nselftest %s\n", ok ? "PASSED" : "FAILED");
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "version" || command == "--version") {
    return command_version();
  }
  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return 0;
  }
  if (command == "selftest") {
    return command_selftest();
  }
  if (command == "explain") {
    return command_explain();
  }
  if (command == "bench") {
    return command_bench(argc, argv);
  }
  if (command == "store-inspect") {
    if (argc < 3) {
      std::fprintf(stderr, "store-inspect requires a directory\n");
      return 2;
    }
    return command_store_inspect(argv[2]);
  }
  std::fprintf(stderr, "unknown command: %s\n", command.c_str());
  print_usage();
  return 2;
}
