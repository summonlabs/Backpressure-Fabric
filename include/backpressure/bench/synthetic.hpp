#pragma once

// Backpressure Fabric - synthetic propagation benchmark.
//
// Every graph produced here is SYNTHETIC. These measurements describe the
// runtime's behaviour on generated topologies and are never evidence about any
// physical network, switch, NIC or link. The benchmark measures completed
// propagation work, not submission cost.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/dependency.hpp"
#include "backpressure/model/resource.hpp"

namespace backpressure {

struct SyntheticGraphSpec {
  std::uint32_t nodes = 512;
  std::uint32_t depth = 6;
  std::uint32_t fanout = 3;
  std::uint32_t sources = 1;
  /// Extra edges that close cycles, exercising loop prevention.
  std::uint32_t cycle_edges = 0;
  /// Deterministic attenuation applied to every generated edge, in parts per
  /// 100000 (100000 = unity).
  std::uint32_t attenuation_per_100k = 85000;
  std::uint32_t seed = 0x5EEDu;
};

struct SyntheticGraph {
  std::vector<Resource> resources{};
  std::vector<DependencyEdge> edges{};
  std::vector<ResourceId> origins{};
  std::uint32_t depth = 0;
  std::uint32_t fanout = 0;
  std::uint32_t cycle_edges = 0;
  std::uint32_t sources = 0;
};

[[nodiscard]] Result<SyntheticGraph> make_synthetic_graph(const SyntheticGraphSpec& spec);

struct BenchCase {
  std::string name{};
  SyntheticGraphSpec spec{};
  std::uint32_t iterations = 8;
  std::uint32_t publish_batch = 1;
};

struct BenchResult {
  std::string name{};
  Status status{};
  bool valid = false;
  /// Always true: the population is generated, not measured from hardware.
  bool synthetic = true;
  std::uint64_t iterations = 0;
  std::uint64_t completed_propagations = 0;
  std::uint64_t nodes_settled = 0;
  std::uint64_t hops_propagated = 0;
  std::uint64_t hops_suppressed = 0;
  std::uint64_t loop_preventions = 0;
  std::uint64_t lineage_evictions = 0;
  double nanos_total = 0.0;
  double propagations_per_second = 0.0;
  double nodes_per_second = 0.0;
  std::uint64_t graph_nodes = 0;
  std::uint64_t graph_edges = 0;
  std::uint32_t depth = 0;
  std::uint32_t fanout = 0;
  std::uint32_t sources = 0;
  std::uint32_t cycle_edges = 0;
};

struct BenchConfig {
  std::vector<BenchCase> cases{};
  Tick start_tick = 1000;
  std::uint32_t seed = 0x5EEDu;
  bool build_topology = true;
};

/// Run the configured cases. Completed work is what is counted.
[[nodiscard]] Result<std::vector<BenchResult>> run_benchmark(const BenchConfig& config);

/// The default suite: graph size, depth, fan-out, source count, cycle density.
[[nodiscard]] std::vector<BenchCase> default_benchmark_cases();

/// Render results as a fixed-width table.
[[nodiscard]] std::string format_bench_results(const std::vector<BenchResult>& results);

}  // namespace backpressure