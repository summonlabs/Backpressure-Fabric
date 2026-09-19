// Backpressure Fabric - simultaneous source and idempotency tests.
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
using bpfab_test::mag;
using bpfab_test::make_edge;
using bpfab_test::make_resource;
using bpfab_test::run_propagation;
using bpfab_test::test_policy;

namespace {

Topology build_shared() {
  auto built = bpfab_test::build_topology(
      {make_resource(1u), make_resource(2u), make_resource(3u), make_resource(4u)},
      {make_edge(1u, 1u, 4u, 90000u), make_edge(2u, 2u, 4u, 90000u), make_edge(3u, 3u, 4u, 90000u)});
  return std::move(built).value();
}

}  // namespace

BPFAB_TEST(multisource, max_aggregation_takes_the_strongest_contribution) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  policy.aggregation = AggregationRule::Max;
  PropagationLedger ledger(256, 256, 256);

  std::vector<PressureSignal> signals;
  signals.push_back(
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 80000u, 100u));
  signals.push_back(
      bpfab_test::make_signal(2u, 2u, 2u, topo.digest(), Epoch(1), policy, 40000u, 100u));
  signals.push_back(
      bpfab_test::make_signal(3u, 3u, 3u, topo.digest(), Epoch(1), policy, 60000u, 100u));

  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, signals);
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.counters.signals_accepted == 3u);
  BPFAB_CHECK(outcome.counters.propagations == 3u);

  const NodeOutcome* shared = find_node(outcome, 4u);
  BPFAB_REQUIRE(shared != nullptr);
  BPFAB_CHECK(shared->contribution_count == 3u);
  BPFAB_CHECK(shared->contributions.size() == 3u);
  BPFAB_CHECK(shared->applied == shared->strongest_single);
  BPFAB_CHECK(shared->applied <= mag(80000u));
  BPFAB_CHECK(shared->best_source == SourceId(1u));
  for (const NodeOutcome& node : outcome.nodes) {
    BPFAB_CHECK(node.applied <= mag(80000u));
  }
}

BPFAB_TEST(multisource, saturating_sum_is_clamped_by_the_ceiling) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  policy.aggregation = AggregationRule::SaturatingSum;
  policy.aggregation_ceiling = mag(70000u);
  BPFAB_REQUIRE_OK(policy.validate());
  PropagationLedger ledger(256, 256, 256);

  std::vector<PressureSignal> signals;
  signals.push_back(
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 50000u, 100u));
  signals.push_back(
      bpfab_test::make_signal(2u, 2u, 2u, topo.digest(), Epoch(1), policy, 50000u, 100u));
  signals.push_back(
      bpfab_test::make_signal(3u, 3u, 3u, topo.digest(), Epoch(1), policy, 50000u, 100u));

  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, signals);
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* shared = find_node(outcome, 4u);
  BPFAB_REQUIRE(shared != nullptr);
  BPFAB_CHECK(shared->applied <= policy.aggregation_ceiling);
  BPFAB_CHECK(shared->applied >= shared->strongest_single);
  BPFAB_CHECK(outcome.counters.clamps >= 1u);
}

BPFAB_TEST(multisource, priority_first_picks_the_highest_priority_source) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  policy.aggregation = AggregationRule::PriorityFirst;
  PropagationLedger ledger(256, 256, 256);

  std::vector<PressureSignal> signals;
  PressureSignal low = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 90000u,
                                               100u);
  low.source_priority = 5;
  low.bind_lineage();
  PressureSignal high = bpfab_test::make_signal(2u, 2u, 2u, topo.digest(), Epoch(1), policy, 30000u,
                                                100u);
  high.source_priority = 1;
  high.bind_lineage();
  signals.push_back(low);
  signals.push_back(high);

  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, signals);
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* shared = find_node(outcome, 4u);
  BPFAB_REQUIRE(shared != nullptr);
  BPFAB_CHECK(shared->applied <= mag(30000u));
}

BPFAB_TEST(multisource, weighted_max_applies_the_source_weight) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  policy.aggregation = AggregationRule::WeightedMax;
  PropagationLedger ledger(256, 256, 256);

  std::vector<PressureSignal> signals;
  PressureSignal strong = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                  80000u, 100u);
  strong.weight_q16 = 32768u;  // half weight
  strong.bind_lineage();
  PressureSignal weak = bpfab_test::make_signal(2u, 2u, 2u, topo.digest(), Epoch(1), policy, 60000u,
                                                100u);
  weak.weight_q16 = 65536u;
  weak.bind_lineage();
  signals.push_back(strong);
  signals.push_back(weak);

  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, signals);
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* shared = find_node(outcome, 4u);
  BPFAB_REQUIRE(shared != nullptr);
  // 0.8 x 0.9 x 0.5 = 0.36 loses to 0.6 x 0.9 x 1.0 = 0.54.
  BPFAB_CHECK(shared->applied.as_percent_milli() <= 55000u);
  BPFAB_CHECK(shared->applied.as_percent_milli() >= 53000u);
  BPFAB_CHECK(shared->applied <= mag(60000u));
}

BPFAB_TEST(multisource, duplicate_sources_in_one_request_are_refused) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(256, 256, 256);
  std::vector<PressureSignal> signals;
  signals.push_back(
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 50000u, 100u));
  signals.push_back(
      bpfab_test::make_signal(2u, 1u, 1u, topo.digest(), Epoch(1), policy, 50000u, 200u));
  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, signals);
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.counters.signals_rejected == 1u);
  BPFAB_CHECK(outcome.counters.signals_accepted == 1u);
  BPFAB_REQUIRE(outcome.rejections.size() == 1u);
  BPFAB_CHECK(outcome.rejections[0].status.code() == ErrorCode::Conflict);
}

BPFAB_TEST(multisource, replay_is_idempotent) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(256, 256, 256);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                  80000u, 100u);

  const PropagationOutcome first =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {signal}, nullptr, 1u);
  BPFAB_REQUIRE_OK(first.status);
  BPFAB_CHECK(first.counters.signals_accepted == 1u);
  const std::size_t applications_after_first = ledger.application_count();

  const PropagationOutcome second =
      run_propagation(topo, policy, ledger, Epoch(1), 101, {signal}, nullptr, 2u);
  BPFAB_REQUIRE_OK(second.status);
  BPFAB_CHECK(second.counters.signals_accepted == 0u);
  BPFAB_CHECK(second.counters.signals_idempotent == 1u);
  BPFAB_CHECK(second.nodes.empty());
  BPFAB_CHECK(ledger.application_count() == applications_after_first);
  BPFAB_REQUIRE(second.explanations.size() == 1u);
  BPFAB_CHECK(second.explanations[0].hops.empty());
  BPFAB_CHECK(!second.explanations[0].fingerprint.is_zero());
}

BPFAB_TEST(multisource, lineage_ledger_evicts_oldest_and_counts_it) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(2, 64, 64);
  for (std::uint64_t i = 0; i < 5; ++i) {
    PressureSignal signal = bpfab_test::make_signal(i + 1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                    static_cast<std::uint32_t>(50000u + i),
                                                    100u + i);
    const PropagationOutcome outcome =
        run_propagation(topo, policy, ledger, Epoch(1), 100u + i, {signal}, nullptr, i + 1u);
    BPFAB_REQUIRE_OK(outcome.status);
  }
  BPFAB_CHECK(ledger.lineage_count() == 2u);
  BPFAB_CHECK(ledger.lineage_evictions() == 3u);
}

BPFAB_TEST(multisource, cross_source_pressure_does_not_multiply_through_a_shared_node) {
  Topology topo = build_shared();
  PropagationPolicy policy = test_policy();
  policy.aggregation = AggregationRule::SaturatingSum;
  PropagationLedger ledger(256, 256, 256);

  std::vector<PressureSignal> signals;
  for (std::uint64_t s = 1; s <= 3; ++s) {
    signals.push_back(bpfab_test::make_signal(s, s, s, topo.digest(), Epoch(1), policy, 40000u,
                                              100u));
  }
  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, signals);
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* shared = find_node(outcome, 4u);
  BPFAB_REQUIRE(shared != nullptr);
  BPFAB_CHECK(shared->contribution_count == 3u);
  // Three contributions of 0.36 each: 1.08 raw, clamped to 1.0, then to the ceiling.
  BPFAB_CHECK(shared->applied.as_percent_milli() <= 100000u);
  BPFAB_CHECK(shared->applied <= policy.aggregation_ceiling);
  for (const Contribution& contribution : shared->contributions) {
    BPFAB_CHECK(contribution.magnitude <= mag(40000u));
  }
}