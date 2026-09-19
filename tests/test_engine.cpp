// Backpressure Fabric - propagation engine tests.
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
using bpfab_test::find_hop;
using bpfab_test::find_node;
using bpfab_test::has_suppression;
using bpfab_test::mag;
using bpfab_test::make_edge;
using bpfab_test::make_resource;
using bpfab_test::run_propagation;
using bpfab_test::test_policy;

namespace {

struct Fixture {
  Topology topology;
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger{1024, 1024, 1024};
  Epoch epoch{1};

  explicit Fixture(Topology built) : topology(std::move(built)) {}
};

}  // namespace

BPFAB_TEST(engine, single_source_attenuates_along_a_chain) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(4u, 50000u));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              80000u, 100u);

  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.counters.signals_accepted == 1u);
  BPFAB_CHECK(outcome.counters.signals_rejected == 0u);
  BPFAB_CHECK(outcome.peak_depth == 3u);
  BPFAB_CHECK(outcome.nodes.size() == 4u);

  const NodeOutcome* first = find_node(outcome, 1u);
  const NodeOutcome* second = find_node(outcome, 2u);
  const NodeOutcome* third = find_node(outcome, 3u);
  const NodeOutcome* fourth = find_node(outcome, 4u);
  BPFAB_REQUIRE(first != nullptr && second != nullptr && third != nullptr && fourth != nullptr);
  BPFAB_CHECK(first->received);
  BPFAB_CHECK(first->applied == signal.observation.magnitude());
  BPFAB_CHECK(second->applied < first->applied);
  BPFAB_CHECK(third->applied < second->applied);
  BPFAB_CHECK(fourth->applied < third->applied);
  BPFAB_CHECK(second->parent == ResourceId(1u));
  BPFAB_CHECK(second->parent_edge == EdgeId(1u));
  BPFAB_CHECK(first->forwarded);
  BPFAB_CHECK(!fourth->forwarded);

  BPFAB_REQUIRE(outcome.explanations.size() == 1u);
  const PropagationExplanation& explanation = outcome.explanations[0];
  BPFAB_CHECK(explanation.hops.size() == 3u);
  const HopRecord* hop = find_hop(explanation, 1u, 2u);
  BPFAB_REQUIRE(hop != nullptr);
  BPFAB_CHECK(hop->verdict == HopVerdict::Propagated);
  BPFAB_CHECK(hop->potential_after < hop->potential_before);
  BPFAB_CHECK(hop->attenuation.q16().raw() == 32768u);
  BPFAB_CHECK(!explanation.fingerprint.is_zero());
  BPFAB_CHECK(!outcome.fingerprint.is_zero());
}

BPFAB_TEST(engine, hops_and_fanout_are_bounded) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(8u, 90000u));
  Fixture fixture(std::move(topo));
  fixture.policy.max_hops = 3;

  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              95000u, 100u, AuthorityLevel::Global, 3u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.peak_depth == 3u);
  BPFAB_CHECK(has_suppression(outcome.explanations[0], SuppressionReason::MaxHops));
  BPFAB_CHECK(find_node(outcome, 5u) == nullptr);

  BPFAB_REQUIRE_RESULT(star, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u), make_resource(3u),
                                  make_resource(4u), make_resource(5u)},
                                 {make_edge(1u, 1u, 2u, 90000u), make_edge(2u, 1u, 3u, 90000u),
                                  make_edge(3u, 1u, 4u, 90000u), make_edge(4u, 1u, 5u, 90000u)}));
  Fixture star_fixture(std::move(star));
  star_fixture.policy.max_fanout = 2;
  PressureSignal star_signal = bpfab_test::make_signal(
      1u, 1u, 1u, star_fixture.topology.digest(), star_fixture.epoch, star_fixture.policy, 95000u,
      100u);
  const PropagationOutcome star_outcome = run_propagation(star_fixture.topology, star_fixture.policy,
                                                          star_fixture.ledger, star_fixture.epoch,
                                                          100, {star_signal});
  BPFAB_REQUIRE_OK(star_outcome.status);
  BPFAB_CHECK(star_outcome.counters.hops_propagated == 2u);
  BPFAB_CHECK(has_suppression(star_outcome.explanations[0], SuppressionReason::MaxFanout));
}

BPFAB_TEST(engine, protected_boundaries_stop_propagation) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u),
                                  make_resource(3u, ProtectionClass::Barrier), make_resource(4u),
                                  make_resource(5u, ProtectionClass::Sealed),
                                  make_resource(6u, ProtectionClass::ObservedOnly)},
                                 {make_edge(1u, 1u, 2u, 90000u), make_edge(2u, 2u, 3u, 90000u),
                                  make_edge(3u, 3u, 4u, 90000u), make_edge(4u, 2u, 5u, 90000u),
                                  make_edge(5u, 2u, 6u, 90000u)}));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              95000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);

  const NodeOutcome* barrier = find_node(outcome, 3u);
  BPFAB_REQUIRE(barrier != nullptr);
  BPFAB_CHECK(barrier->received);
  BPFAB_CHECK(!barrier->forwarded);
  BPFAB_CHECK(barrier->blocked_reason == SuppressionReason::ProtectedBoundary);
  BPFAB_CHECK(find_node(outcome, 4u) == nullptr);
  BPFAB_CHECK(find_node(outcome, 5u) == nullptr);
  BPFAB_CHECK(find_node(outcome, 6u) == nullptr);
  BPFAB_CHECK(has_suppression(outcome.explanations[0], SuppressionReason::SealedResource));
  BPFAB_CHECK(has_suppression(outcome.explanations[0], SuppressionReason::ObservedOnlyResource));
}

BPFAB_TEST(engine, cycles_never_amplify_or_repeat) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u), make_resource(3u)},
                                 {make_edge(1u, 1u, 2u, 90000u), make_edge(2u, 2u, 3u, 90000u),
                                  make_edge(3u, 3u, 1u, 90000u)}));
  Fixture fixture(std::move(topo));
  BPFAB_CHECK(fixture.topology.has_cycles());

  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              95000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.counters.nodes_settled == 3u);
  BPFAB_CHECK(outcome.counters.signals_accepted == 1u);

  const NodeOutcome* origin = find_node(outcome, 1u);
  BPFAB_REQUIRE(origin != nullptr);
  BPFAB_CHECK(origin->applied == signal.observation.magnitude());
  BPFAB_CHECK(origin->contribution_count == 1u);
  BPFAB_CHECK(outcome.counters.loop_preventions >= 1u);
  BPFAB_CHECK(has_suppression(outcome.explanations[0], SuppressionReason::NodeAlreadySettled));

  for (const NodeOutcome& node : outcome.nodes) {
    BPFAB_CHECK(node.applied <= signal.observation.magnitude());
    BPFAB_CHECK(node.contribution_count == 1u);
  }
}

BPFAB_TEST(engine, self_loop_is_refused) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u)},
                                 {make_edge(1u, 1u, 1u, 65536u), make_edge(2u, 1u, 2u, 50000u)}));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              95000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* origin = find_node(outcome, 1u);
  BPFAB_REQUIRE(origin != nullptr);
  BPFAB_CHECK(origin->applied == signal.observation.magnitude());
  BPFAB_CHECK(outcome.counters.loop_preventions >= 1u);
  const NodeOutcome* next = find_node(outcome, 2u);
  BPFAB_REQUIRE(next != nullptr);
  BPFAB_CHECK(next->applied < origin->applied);
}

BPFAB_TEST(engine, duplicate_paths_settle_once_with_the_strongest_potential) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u), make_resource(3u),
                                  make_resource(4u)},
                                 {make_edge(1u, 1u, 2u, 50000u), make_edge(2u, 2u, 4u, 90000u),
                                  make_edge(3u, 1u, 3u, 90000u), make_edge(4u, 3u, 4u, 50000u)}));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              90000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* target = find_node(outcome, 4u);
  BPFAB_REQUIRE(target != nullptr);
  BPFAB_CHECK(target->contribution_count == 1u);
  BPFAB_CHECK(target->best_depth == 2u);
  BPFAB_CHECK(target->parent == ResourceId(3u));
  BPFAB_CHECK(target->applied <= mag(90000u));
}

BPFAB_TEST(engine, below_floor_pressure_damps_out) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(8u, 20000u));
  Fixture fixture(std::move(topo));
  fixture.policy.max_hops = 8;
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              50000u, 100u, AuthorityLevel::Global, 8u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(has_suppression(outcome.explanations[0], SuppressionReason::BelowFloor));
  BPFAB_CHECK(outcome.nodes.size() < 8u);
  for (const NodeOutcome& node : outcome.nodes) {
    BPFAB_CHECK(node.applied <= signal.observation.magnitude());
  }
}

BPFAB_TEST(engine, zero_attenuation_edges_never_carry_pressure) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u)},
                                 {make_edge(1u, 1u, 2u, 0u)}));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              90000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(has_suppression(outcome.explanations[0], SuppressionReason::ZeroAttenuation));
  BPFAB_CHECK(find_node(outcome, 2u) == nullptr);
}

BPFAB_TEST(engine, record_budget_truncates_without_partial_effects) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(6u, 90000u));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              95000u, 100u, AuthorityLevel::Global, 6u);
  const PropagationOutcome outcome = run_propagation(fixture.topology, fixture.policy,
                                                     fixture.ledger, fixture.epoch, 100, {signal},
                                                     nullptr, 1u, 2u);
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(!outcome.complete);
  BPFAB_CHECK(outcome.counters.record_truncations >= 1u);
  BPFAB_CHECK(outcome.nodes.size() < 6u);
  for (const PropagationExplanation& explanation : outcome.explanations) {
    BPFAB_CHECK(explanation.truncated);
  }
}

BPFAB_TEST(engine, dry_run_decides_without_mutating_ledger) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(3u, 50000u));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy,
                              80000u, 100u);
  const PropagationOutcome planned = run_propagation(fixture.topology, fixture.policy,
                                                     fixture.ledger, fixture.epoch, 100, {signal},
                                                     nullptr, 1u, 0u, {}, true);
  BPFAB_REQUIRE_OK(planned.status);
  BPFAB_CHECK(fixture.ledger.lineage_count() == 0u);
  BPFAB_CHECK(fixture.ledger.application_count() == 0u);
  BPFAB_CHECK(planned.counters.signals_accepted == 1u);

  const PropagationOutcome committed = run_propagation(
      fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_OK(committed.status);
  BPFAB_CHECK(committed.fingerprint == planned.fingerprint);
  BPFAB_CHECK(fixture.ledger.lineage_count() == 1u);
  BPFAB_CHECK(fixture.ledger.application_count() > 0u);
}

BPFAB_TEST(engine, request_validation_rejects_stale_and_malformed_requests) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(2u, 50000u));
  Fixture fixture(std::move(topo));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, fixture.topology.digest(), fixture.epoch, fixture.policy);

  PropagationOutcome none = run_propagation(fixture.topology, fixture.policy, fixture.ledger,
                                            fixture.epoch, 100, {});
  BPFAB_REQUIRE_CODE(none.status, ErrorCode::InvalidArgument);

  EngineContext ctx;
  ctx.topology = &fixture.topology;
  ctx.policy = &fixture.policy;
  ctx.ledger = &fixture.ledger;
  ctx.live_epoch = fixture.epoch;

  PropagationRequest request;
  request.id = PropagationId(1u);
  request.policy_id = fixture.policy.id;
  request.policy_generation = fixture.policy.generation;
  request.topology_digest = Digest128{};
  request.epoch = fixture.epoch;
  request.now = 100;
  request.signals = {signal};
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::StaleGeneration);

  request.topology_digest = fixture.topology.digest();
  request.epoch = Epoch(9);
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::StaleEpoch);

  request.epoch = fixture.epoch;
  request.policy_generation = Generation::from_raw(42u);
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::StaleGeneration);

  request.policy_generation = fixture.policy.generation;
  request.id = PropagationId{};
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::InvalidArgument);

  request.id = PropagationId(1u);
  request.policy_id = PolicyId(99u);
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::NotFound);

  request.policy_id = fixture.policy.id;
  std::vector<PressureSignal> too_many;
  for (std::uint32_t i = 0; i < fixture.policy.max_sources + 1u; ++i) {
    too_many.push_back(signal);
  }
  request.signals = too_many;
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::LimitExceeded);

  request.signals = {signal};
  const Status engine_status = PropagationEngine::propagate(ctx, request).status;
  BPFAB_REQUIRE_OK(engine_status);
}

BPFAB_TEST(engine, unknown_origin_resource_is_refused) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(2u, 50000u));
  Fixture fixture(std::move(topo));
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 99u, fixture.topology.digest(),
                                                  fixture.epoch, fixture.policy, 80000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(fixture.topology, fixture.policy, fixture.ledger, fixture.epoch, 100, {signal});
  BPFAB_REQUIRE_CODE(outcome.status, ErrorCode::NotFound);
  BPFAB_CHECK(outcome.counters.signals_rejected == 1u);
}