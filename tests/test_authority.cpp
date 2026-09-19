// Backpressure Fabric - authority, fence and amplification tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;
using bpfab_test::find_hop;
using bpfab_test::find_node;
using bpfab_test::has_suppression;
using bpfab_test::make_edge;
using bpfab_test::make_resource;
using bpfab_test::run_propagation;
using bpfab_test::test_policy;

BPFAB_TEST(authority, unknown_pressure_is_refused_at_the_engine) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(3u, 50000u));
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy);
  signal.observation = PressureObservation::unknown();
  signal.bind_lineage();

  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_CODE(outcome.status, ErrorCode::UnknownPressure);
  BPFAB_CHECK(outcome.counters.signals_rejected == 1u);
  BPFAB_CHECK(outcome.nodes.empty());
  BPFAB_REQUIRE(outcome.rejections.size() == 1u);
  BPFAB_CHECK(outcome.rejections[0].status.code() == ErrorCode::UnknownPressure);
  BPFAB_CHECK(ledger.lineage_count() == 0u);
}

BPFAB_TEST(authority, scoped_authority_refuses_unlisted_targets) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u), make_resource(3u)},
                                 {make_edge(1u, 1u, 2u, 90000u), make_edge(2u, 2u, 3u, 90000u)}));
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                  90000u, 100u, AuthorityLevel::Scoped, 4u);
  signal.authority.scope = AuthorityScope{};
  BPFAB_REQUIRE_OK(signal.authority.scope.add(ResourceId(2u)));
  signal.bind_lineage();

  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(find_node(outcome, 2u) != nullptr);
  BPFAB_CHECK(find_node(outcome, 3u) == nullptr);
  BPFAB_CHECK(has_suppression(outcome.explanations[0], SuppressionReason::UnauthorizedScope));
  BPFAB_CHECK(outcome.counters.unauthorized_rejections >= 1u);
}

BPFAB_TEST(authority, amplification_requires_explicit_authorization_and_a_bound) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u)},
                                 {make_edge(1u, 1u, 2u, 100000u, 0u, 0u,
                                            EdgeFlags::of(EdgeFlagKind::Amplifying), 150000u)}));

  // 1. Policy does not authorize amplification: the edge must be refused.
  {
    PropagationPolicy policy = test_policy();
    PropagationLedger ledger(64, 64, 64);
    PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                    40000u, 100u);
    const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
    BPFAB_REQUIRE_OK(outcome.status);
    BPFAB_CHECK(find_node(outcome, 2u) == nullptr);
    BPFAB_CHECK(has_suppression(outcome.explanations[0],
                                SuppressionReason::AmplificationNotAuthorized));
    for (const NodeOutcome& node : outcome.nodes) {
      BPFAB_CHECK(node.applied <= signal.observation.magnitude());
    }
  }

  // 2. Policy authorizes it but the signal's authority does not.
  {
    PropagationPolicy policy = test_policy();
    policy.allow_amplification = true;
    policy.max_cumulative_gain = Gain::from_percent_milli(150000u).value();
    BPFAB_REQUIRE_OK(policy.validate());
    PropagationLedger ledger(64, 64, 64);
    PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                    40000u, 100u);
    const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
    BPFAB_REQUIRE_OK(outcome.status);
    BPFAB_CHECK(find_node(outcome, 2u) == nullptr);
    BPFAB_CHECK(has_suppression(outcome.explanations[0],
                                SuppressionReason::AmplificationNotAuthorized));
  }

  // 3. Both authorize it: pressure may rise, but only within the granted bound.
  {
    PropagationPolicy policy = test_policy();
    policy.allow_amplification = true;
    policy.max_cumulative_gain = Gain::from_percent_milli(200000u).value();
    BPFAB_REQUIRE_OK(policy.validate());
    PropagationLedger ledger(64, 64, 64);
    PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                    40000u, 100u);
    signal.authority.amplification_authorized = true;
    signal.authority.max_gain = Gain::from_percent_milli(150000u).value();
    signal.bind_lineage();
    const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
    BPFAB_REQUIRE_OK(outcome.status);
    const NodeOutcome* target = find_node(outcome, 2u);
    BPFAB_REQUIRE(target != nullptr);
    BPFAB_CHECK(target->applied > signal.observation.magnitude());
    BPFAB_CHECK(outcome.counters.amplifications == 1u);
    BPFAB_CHECK(outcome.explanations[0].amplification_used);
    const HopRecord* hop = find_hop(outcome.explanations[0], 1u, 2u);
    BPFAB_REQUIRE(hop != nullptr);
    BPFAB_CHECK(hop->verdict == HopVerdict::Amplified);
    BPFAB_CHECK(hop->cumulative_gain_q16 <= 150000u);
    // 0.4 x 1.0 x 1.5 = 0.60, above the source magnitude but within the bound.
    BPFAB_CHECK(target->applied.as_percent_milli() >= 59000u);
    BPFAB_CHECK(target->applied.as_percent_milli() <= 61000u);
  }

  // 4. The cumulative gain bound refuses a second amplifying hop.
  {
    BPFAB_REQUIRE_RESULT(chain, bpfab_test::build_topology(
                                    {make_resource(1u), make_resource(2u), make_resource(3u)},
                                    {make_edge(1u, 1u, 2u, 65536u, 0u, 0u,
                                               EdgeFlags::of(EdgeFlagKind::Amplifying), 120000u),
                                     make_edge(2u, 2u, 3u, 65536u, 0u, 0u,
                                               EdgeFlags::of(EdgeFlagKind::Amplifying), 120000u)}));
    PropagationPolicy policy = test_policy();
    policy.allow_amplification = true;
    policy.max_cumulative_gain = Gain::from_percent_milli(130000u).value();
    BPFAB_REQUIRE_OK(policy.validate());
    PropagationLedger ledger(64, 64, 64);
    PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, chain.digest(), Epoch(1), policy,
                                                    20000u, 100u);
    signal.authority.amplification_authorized = true;
    signal.authority.max_gain = Gain::from_percent_milli(130000u).value();
    signal.bind_lineage();
    const PropagationOutcome outcome = run_propagation(chain, policy, ledger, Epoch(1), 100, {signal});
    BPFAB_REQUIRE_OK(outcome.status);
    BPFAB_CHECK(find_node(outcome, 2u) != nullptr);
    BPFAB_CHECK(find_node(outcome, 3u) == nullptr);
    BPFAB_CHECK(has_suppression(outcome.explanations[0],
                                SuppressionReason::AmplificationBudgetExhausted));
    for (const NodeOutcome& node : outcome.nodes) {
      BPFAB_CHECK(node.applied.as_percent_milli() <= 26000u);
    }
  }
}

BPFAB_TEST(authority, fences_override_every_other_rule) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u), make_resource(3u)},
                                 {make_edge(1u, 1u, 2u, 90000u), make_edge(2u, 2u, 3u, 90000u)}));
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 90000u, 100u);

  // Refuse ingress at the origin: nothing happens at all.
  {
    FenceTable fences;
    Fence fence;
    fence.id = FenceId(1u);
    fence.resource = ResourceId(1u);
    fence.kind = FenceKind::RefuseIngress;
    fence.epoch = Epoch(1);
    BPFAB_REQUIRE_OK(fences.add(fence));
    const PropagationOutcome outcome =
        run_propagation(topo, policy, ledger, Epoch(1), 100, {signal}, &fences);
    BPFAB_REQUIRE_OK(outcome.status);
    const NodeOutcome* origin = find_node(outcome, 1u);
    BPFAB_REQUIRE(origin != nullptr);
    BPFAB_CHECK(!origin->received);
    BPFAB_CHECK(origin->blocked_reason == SuppressionReason::Fenced);
    BPFAB_CHECK(find_node(outcome, 2u) == nullptr);
    BPFAB_CHECK(outcome.counters.fence_rejections >= 1u);
  }

  // Refuse egress at the middle: it receives but does not forward.
  {
    FenceTable fences;
    Fence fence;
    fence.id = FenceId(2u);
    fence.resource = ResourceId(2u);
    fence.kind = FenceKind::RefuseEgress;
    fence.epoch = Epoch(1);
    BPFAB_REQUIRE_OK(fences.add(fence));
    PropagationLedger inner(64, 64, 64);
    const PropagationOutcome outcome =
        run_propagation(topo, policy, inner, Epoch(1), 100, {signal}, &fences);
    BPFAB_REQUIRE_OK(outcome.status);
    const NodeOutcome* middle = find_node(outcome, 2u);
    BPFAB_REQUIRE(middle != nullptr);
    BPFAB_CHECK(middle->received);
    BPFAB_CHECK(!middle->forwarded);
    BPFAB_CHECK(middle->blocked_reason == SuppressionReason::Fenced);
    BPFAB_CHECK(middle->governing_fence == FenceId(2u));
    BPFAB_CHECK(find_node(outcome, 3u) == nullptr);
  }

  // Expired fences no longer apply.
  {
    FenceTable fences;
    Fence fence;
    fence.id = FenceId(3u);
    fence.resource = ResourceId(2u);
    fence.kind = FenceKind::Barrier;
    fence.epoch = Epoch(1);
    fence.expires_at = 50;
    BPFAB_REQUIRE_OK(fences.add(fence));
    BPFAB_CHECK(fences.governing(ResourceId(2u), 40) != nullptr);
    BPFAB_CHECK(fences.governing(ResourceId(2u), 60) == nullptr);
    PropagationLedger inner(64, 64, 64);
    const PropagationOutcome outcome =
        run_propagation(topo, policy, inner, Epoch(1), 60, {signal}, &fences);
    BPFAB_REQUIRE_OK(outcome.status);
    BPFAB_CHECK(find_node(outcome, 3u) != nullptr);
  }
}

BPFAB_TEST(authority, fence_table_purges_volatile_and_rebinds_durable) {
  FenceTable fences;
  Fence volatile_fence;
  volatile_fence.id = FenceId(1u);
  volatile_fence.resource = ResourceId(1u);
  volatile_fence.kind = FenceKind::Barrier;
  volatile_fence.epoch = Epoch(1);
  BPFAB_REQUIRE_OK(fences.add(volatile_fence));

  Fence durable_fence;
  durable_fence.id = FenceId(2u);
  durable_fence.resource = ResourceId(2u);
  durable_fence.kind = FenceKind::Barrier;
  durable_fence.epoch = Epoch(1);
  durable_fence.durable = true;
  BPFAB_REQUIRE_OK(fences.add(durable_fence));

  BPFAB_CHECK(fences.purge_stale(Epoch(2)) == 1u);
  BPFAB_CHECK(fences.size() == 1u);
  BPFAB_CHECK(fences.rebind_durable(Epoch(2)) == 1u);
  const Fence* remaining = fences.governing(ResourceId(2u), 0);
  BPFAB_REQUIRE(remaining != nullptr);
  BPFAB_CHECK(remaining->epoch == Epoch(2));

  Fence expiring = durable_fence;
  expiring.id = FenceId(3u);
  expiring.expires_at = 10;
  expiring.epoch = Epoch(2);
  BPFAB_REQUIRE_OK(fences.add(expiring));
  BPFAB_CHECK(fences.expire(11) == 1u);
  BPFAB_REQUIRE_OK(fences.remove(FenceId(2u)));
  BPFAB_REQUIRE_CODE(fences.remove(FenceId(2u)), ErrorCode::NotFound);

  Fence unbound;
  BPFAB_REQUIRE_CODE(fences.add(unbound), ErrorCode::InvalidArgument);
}

BPFAB_TEST(authority, aggregation_ceiling_bounds_the_aggregate) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u), make_resource(2u)},
                                 {make_edge(1u, 1u, 2u, 65536u)}));
  PropagationPolicy policy = test_policy();
  policy.aggregation_ceiling = bpfab_test::mag(50000u);
  BPFAB_REQUIRE_OK(policy.validate());
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 100000u, 100u);
  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  for (const NodeOutcome& node : outcome.nodes) {
    BPFAB_CHECK(node.applied <= policy.aggregation_ceiling);
  }
}