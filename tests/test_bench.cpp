// Backpressure Fabric - synthetic benchmark smoke tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"

using namespace backpressure;

BPFAB_TEST(bench, synthetic_graphs_are_reproducible) {
  SyntheticGraphSpec spec;
  spec.nodes = 200;
  spec.depth = 5;
  spec.fanout = 3;
  spec.sources = 2;
  spec.cycle_edges = 10;

  BPFAB_REQUIRE_RESULT(first, make_synthetic_graph(spec));
  BPFAB_REQUIRE_RESULT(second, make_synthetic_graph(spec));
  BPFAB_CHECK(first.resources.size() == second.resources.size());
  BPFAB_CHECK(first.edges.size() == second.edges.size());
  BPFAB_CHECK(first.resources.size() <= spec.nodes);
  BPFAB_CHECK(first.origins.size() == spec.sources);
  BPFAB_CHECK(first.cycle_edges > 0u);

  spec.seed = 999u;
  BPFAB_REQUIRE_RESULT(third, make_synthetic_graph(spec));
  BPFAB_CHECK(third.edges.size() == first.edges.size());
  bool differs = third.cycle_edges != first.cycle_edges;
  if (!differs) {
    for (std::size_t i = 0; i < third.edges.size() && !differs; ++i) {
      differs = third.edges[i].from != first.edges[i].from ||
                third.edges[i].to != first.edges[i].to;
    }
  }
  BPFAB_CHECK(differs);

  SyntheticGraphSpec invalid = spec;
  invalid.nodes = 0;
  BPFAB_REQUIRE_CODE(make_synthetic_graph(invalid).status(), ErrorCode::InvalidArgument);
}

BPFAB_TEST(bench, benchmark_measures_completed_work_only) {
  BenchConfig config;
  BenchCase small;
  small.name = "unit-small";
  small.spec.nodes = 64;
  small.spec.depth = 4;
  small.spec.fanout = 3;
  small.iterations = 4;
  config.cases.push_back(small);
  BenchCase cyclic;
  cyclic.name = "unit-cyclic";
  cyclic.spec.nodes = 64;
  cyclic.spec.depth = 4;
  cyclic.spec.fanout = 3;
  cyclic.spec.cycle_edges = 16;
  cyclic.iterations = 4;
  config.cases.push_back(cyclic);

  BPFAB_REQUIRE_RESULT(results, run_benchmark(config));
  BPFAB_REQUIRE(results.size() == 2u);
  for (const BenchResult& result : results) {
    BPFAB_REQUIRE_OK(result.status);
    BPFAB_CHECK(result.valid);
    BPFAB_CHECK(result.synthetic);
    BPFAB_CHECK(result.iterations == 4u);
    BPFAB_CHECK(result.completed_propagations == 4u);
    BPFAB_CHECK_MSG(result.nodes_settled > 0u,
                    result.name + " settled=" + std::to_string(result.nodes_settled));
    BPFAB_CHECK_MSG(result.hops_propagated > 0u,
                    result.name + " hops=" + std::to_string(result.hops_propagated) +
                        " nodes=" + std::to_string(result.graph_nodes) +
                        " edges=" + std::to_string(result.graph_edges));
    BPFAB_CHECK(result.nanos_total >= 0.0);
    BPFAB_CHECK(result.propagations_per_second >= 0.0);
    BPFAB_CHECK(result.graph_nodes <= result.graph_nodes);
  }
  const std::string formatted = format_bench_results(results);
  BPFAB_CHECK(formatted.find("unit-small") != std::string::npos);
  BPFAB_CHECK(formatted.find("INVALID") == std::string::npos);

  BenchConfig empty;
  BPFAB_REQUIRE_CODE(run_benchmark(empty).status(), ErrorCode::InvalidArgument);

  const std::vector<BenchCase> defaults = default_benchmark_cases();
  BPFAB_CHECK(defaults.size() >= 6u);
  bool saw_cycles = false;
  bool saw_sources = false;
  for (const BenchCase& bench_case : defaults) {
    if (bench_case.spec.cycle_edges > 0u) {
      saw_cycles = true;
    }
    if (bench_case.spec.sources > 1u) {
      saw_sources = true;
    }
  }
  BPFAB_CHECK(saw_cycles);
  BPFAB_CHECK(saw_sources);
}