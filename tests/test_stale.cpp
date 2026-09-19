// Backpressure Fabric - staleness, generation binding and fencing tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;
using bpfab_test::find_node;
using bpfab_test::has_suppression;
using bpfab_test::make_edge;
using bpfab_test::make_resource;
using bpfab_test::run_propagation;
using bpfab_test::test_policy;

BPFAB_TEST(stale, topology_change_invalidates_in_flight_propagation) {
  BPFAB_REQUIRE_RESULT(before, bpfab_test::build_topology(
                                   {make_resource(1u), make_resource(2u)},
                                   {make_edge(1u, 1u, 2u, 90000u)}));
  BPFAB_REQUIRE_RESULT(after, bpfab_test::build_topology(
                                  {make_resource(1u), make_resource(2u), make_resource(3u)},
                                  {make_edge(1u, 1u, 2u, 90000u), make_edge(2u, 2u, 3u, 90000u)}));
  BPFAB_CHECK(before.digest() != after.digest());
  BPFAB_CHECK(before.generation() != after.generation());

  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(64, 64, 64);

  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, before.digest(), Epoch(1), policy, 90000u, 100u);

  EngineContext ctx;
  ctx.topology = &after;
  ctx.policy = &policy;
  ctx.ledger = &ledger;
  ctx.live_epoch = Epoch(1);

  PropagationRequest request;
  request.id = PropagationId(1u);
  request.policy_id = policy.id;
  request.policy_generation = policy.generation;
  request.topology_digest = before.digest();
  request.epoch = Epoch(1);
  request.now = 100;
  request.signals = {signal};

  const PropagationOutcome outcome = PropagationEngine::propagate(ctx, request);
  BPFAB_REQUIRE_CODE(outcome.status, ErrorCode::StaleGeneration);
  BPFAB_CHECK(ledger.lineage_count() == 0u);
}

BPFAB_TEST(stale, dependency_pins_refuse_superseded_edges) {
  std::vector<Resource> resources = {make_resource(1u), make_resource(2u)};
  std::vector<DependencyEdge> edges = {make_edge(1u, 1u, 2u, 90000u)};
  BPFAB_REQUIRE_RESULT(original, bpfab_test::build_topology(resources, edges));

  edges[0].dependency_generation = Generation::from_raw(2u);
  BPFAB_REQUIRE_RESULT(rebuilt, bpfab_test::build_topology(resources, edges));
  BPFAB_CHECK(rebuilt.digest() != original.digest());

  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, rebuilt.digest(), Epoch(1), policy, 90000u, 100u);

  DependencyPin stale_pin;
  stale_pin.edge = EdgeId(1u);
  stale_pin.generation = Generation::initial();

  const PropagationOutcome outcome = run_propagation(rebuilt, policy, ledger, Epoch(1), 100,
                                                     {signal}, nullptr, 1u, 0u, {stale_pin});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(find_node(outcome, 2u) == nullptr);
  BPFAB_CHECK(has_suppression(outcome.explanations[0],
                              SuppressionReason::StaleDependencyGeneration));
  BPFAB_CHECK(outcome.counters.stale_rejections >= 1u);

  DependencyPin fresh_pin;
  fresh_pin.edge = EdgeId(1u);
  fresh_pin.generation = Generation::from_raw(2u);
  PropagationLedger second_ledger(64, 64, 64);
  const PropagationOutcome accepted = run_propagation(rebuilt, policy, second_ledger, Epoch(1), 100,
                                                      {signal}, nullptr, 2u, 0u, {fresh_pin});
  BPFAB_REQUIRE_OK(accepted.status);
  BPFAB_CHECK(find_node(accepted, 2u) != nullptr);

  DependencyPin unknown_pin;
  unknown_pin.edge = EdgeId(99u);
  unknown_pin.generation = Generation::initial();
  const PropagationOutcome missing = run_propagation(rebuilt, policy, second_ledger, Epoch(1), 100,
                                                     {signal}, nullptr, 3u, 0u, {unknown_pin});
  BPFAB_REQUIRE_CODE(missing.status, ErrorCode::NotFound);
}

BPFAB_TEST(stale, epoch_advance_fences_previous_authority) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(2u, 90000u));
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 90000u, 100u);

  const PropagationOutcome stale =
      run_propagation(topo, policy, ledger, Epoch(2), 100, {signal});
  BPFAB_REQUIRE_CODE(stale.status, ErrorCode::StaleEpoch);
  BPFAB_CHECK(stale.counters.stale_rejections >= 1u);
  BPFAB_CHECK(ledger.lineage_count() == 0u);

  PressureSignal fresh =
      bpfab_test::make_signal(2u, 1u, 1u, topo.digest(), Epoch(2), policy, 90000u, 100u);
  const PropagationOutcome accepted = run_propagation(topo, policy, ledger, Epoch(2), 100, {fresh});
  BPFAB_REQUIRE_OK(accepted.status);
  BPFAB_CHECK(find_node(accepted, 2u) != nullptr);
}

BPFAB_TEST(stale, fabric_refuses_stale_incarnations_and_missing_publishers) {
  FabricConfig config;
  config.state_directory.clear();
  BPFAB_REQUIRE_RESULT(fabric, Fabric::open(config));
  BPFAB_REQUIRE_OK(fabric->install_topology({make_resource(1u), make_resource(2u)},
                                            {make_edge(1u, 1u, 2u, 90000u)}, 0));

  PropagationPolicy policy = test_policy();
  BPFAB_REQUIRE_OK(fabric->install_policy(policy, 0));

  const Incarnation first = bpfab_test::test_incarnation(1u, 100u);
  BPFAB_REQUIRE_OK(fabric->register_publisher(SourceId(1u), first, 0));

  const auto topo = fabric->topology();
  BPFAB_REQUIRE(topo != nullptr);

  PressureSignal unknown_source =
      bpfab_test::make_signal(1u, 42u, 1u, topo->digest(), fabric->epoch(), policy, 90000u, 0u,
                              AuthorityLevel::Global, 8u, first);
  BPFAB_REQUIRE_CODE(fabric->publish(unknown_source, 0).status(), ErrorCode::NotFound);

  PressureSignal wrong_incarnation =
      bpfab_test::make_signal(2u, 1u, 1u, topo->digest(), fabric->epoch(), policy, 90000u, 0u,
                              AuthorityLevel::Global, 8u, bpfab_test::test_incarnation(9u, 777u));
  BPFAB_REQUIRE_CODE(fabric->publish(wrong_incarnation, 0).status(), ErrorCode::StaleIncarnation);

  PressureSignal stale_epoch =
      bpfab_test::make_signal(3u, 1u, 1u, topo->digest(), Epoch(99), policy, 90000u, 0u,
                              AuthorityLevel::Global, 8u, first);
  BPFAB_REQUIRE_CODE(fabric->publish(stale_epoch, 0).status(), ErrorCode::StaleEpoch);

  PressureSignal stale_topology =
      bpfab_test::make_signal(4u, 1u, 1u, Digest128{1u, 2u}, fabric->epoch(), policy, 90000u, 0u,
                              AuthorityLevel::Global, 8u, first);
  BPFAB_REQUIRE_CODE(fabric->publish(stale_topology, 0).status(), ErrorCode::StaleGeneration);

  PressureSignal good = bpfab_test::make_signal(5u, 1u, 1u, topo->digest(), fabric->epoch(), policy,
                                                90000u, 0u, AuthorityLevel::Global, 8u, first);
  BPFAB_REQUIRE_RESULT(outcome, fabric->publish(good, 0));
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(find_node(outcome, 2u) != nullptr);

  const Incarnation second = first.reincarnate().value();
  BPFAB_REQUIRE_OK(fabric->register_publisher(SourceId(1u), second, 1));
  PressureSignal from_first_again =
      bpfab_test::make_signal(6u, 1u, 1u, topo->digest(), fabric->epoch(), policy, 90000u, 1u,
                              AuthorityLevel::Global, 8u, first);
  BPFAB_REQUIRE_CODE(fabric->publish(from_first_again, 1).status(), ErrorCode::StaleIncarnation);

  Incarnation regressed = second;
  regressed.seq = 0;
  BPFAB_REQUIRE_CODE(fabric->register_publisher(SourceId(1u), regressed, 1),
                     ErrorCode::InvalidArgument);

  BPFAB_REQUIRE_OK(fabric->retire_publisher(SourceId(1u), second, 2));
  PressureSignal from_retired =
      bpfab_test::make_signal(7u, 1u, 1u, topo->digest(), fabric->epoch(), policy, 90000u, 2u,
                              AuthorityLevel::Global, 8u, second);
  BPFAB_REQUIRE_CODE(fabric->publish(from_retired, 2).status(), ErrorCode::NotReady);
}

BPFAB_TEST(stale, fabric_topology_reinstall_fences_old_digest) {
  FabricConfig config;
  BPFAB_REQUIRE_RESULT(fabric, Fabric::open(config));
  PropagationPolicy policy = test_policy();
  BPFAB_REQUIRE_OK(fabric->install_policy(policy, 0));
  const Incarnation incarnation = bpfab_test::test_incarnation(3u, 55u);
  BPFAB_REQUIRE_OK(fabric->register_publisher(SourceId(7u), incarnation, 0));

  BPFAB_REQUIRE_OK(fabric->install_topology({make_resource(1u), make_resource(2u)},
                                            {make_edge(1u, 1u, 2u, 90000u)}, 0));
  const auto first = fabric->topology();
  BPFAB_REQUIRE(first != nullptr);
  const Digest128 first_digest = first->digest();

  BPFAB_REQUIRE_OK(fabric->install_topology(
      {make_resource(1u), make_resource(2u), make_resource(3u)},
      {make_edge(1u, 1u, 2u, 90000u), make_edge(2u, 2u, 3u, 90000u)}, 1));
  const auto second = fabric->topology();
  BPFAB_REQUIRE(second != nullptr);
  BPFAB_CHECK(second->digest() != first_digest);

  PressureSignal stale = bpfab_test::make_signal(1u, 7u, 1u, first_digest, fabric->epoch(), policy,
                                                 90000u, 1u, AuthorityLevel::Global, 8u,
                                                 incarnation);
  BPFAB_REQUIRE_CODE(fabric->publish(stale, 1).status(), ErrorCode::StaleGeneration);

  PressureSignal fresh =
      bpfab_test::make_signal(2u, 7u, 1u, second->digest(), fabric->epoch(), policy, 90000u, 1u,
                              AuthorityLevel::Global, 8u, incarnation);
  BPFAB_REQUIRE_RESULT(outcome, fabric->publish(fresh, 1));
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(find_node(outcome, 3u) != nullptr);
  const FabricStatus status = fabric->status();
  BPFAB_CHECK(status.restored_liveness == false);
  BPFAB_CHECK(status.live_publishers == 1u);
}

BPFAB_TEST(stale, install_policy_refuses_a_superseded_generation) {
  FabricConfig config;
  BPFAB_REQUIRE_RESULT(fabric, Fabric::open(config));
  PropagationPolicy policy = test_policy(5u, 2u);
  BPFAB_REQUIRE_OK(fabric->install_policy(policy, 0));
  // The identical policy is idempotent rather than stale.
  BPFAB_REQUIRE_OK(fabric->install_policy(policy, 1));
  PropagationPolicy changed = policy;
  changed.max_fanout = policy.max_fanout + 1u;
  BPFAB_REQUIRE_CODE(fabric->install_policy(changed, 1), ErrorCode::StaleGeneration);
  policy.generation = Generation::from_raw(3u);
  BPFAB_REQUIRE_OK(fabric->install_policy(policy, 1));
  PropagationPolicy invalid = test_policy(6u, 1u);
  invalid.max_hops = 0;
  BPFAB_REQUIRE_CODE(fabric->install_policy(invalid, 2), ErrorCode::OutOfRange);
}