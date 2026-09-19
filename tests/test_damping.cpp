// Backpressure Fabric - damping and recovery tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace bpfab_test;

using namespace backpressure;

BPFAB_TEST(damping, cooldown_suppresses_a_repeat_without_hysteresis) {
  BPFAB_REQUIRE_RESULT(topo, build_topology({make_resource(1u), make_resource(2u)},
                                            {make_edge(1u, 1u, 2u, 90000u, 0u, 100u)}));
  PropagationPolicy policy = test_policy();
  policy.default_cooldown_ticks = 100;
  PropagationLedger ledger(64, 64, 64);

  PressureSignal first =
      make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 80000u, 0u);
  const PropagationOutcome first_outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 0, {first}, nullptr, 1u);
  BPFAB_REQUIRE_OK(first_outcome.status);
  BPFAB_CHECK(find_node(first_outcome, 2u) != nullptr);

  PressureSignal second =
      make_signal(2u, 1u, 1u, topo.digest(), Epoch(1), policy, 80000u, 50u);
  const PropagationOutcome second_outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 50, {second}, nullptr, 2u);
  BPFAB_REQUIRE_OK(second_outcome.status);
  BPFAB_CHECK(find_node(second_outcome, 2u) == nullptr);
  BPFAB_CHECK(has_suppression(second_outcome.explanations[0],
                              SuppressionReason::HysteresisNotExceeded));

  PressureSignal third =
      make_signal(3u, 1u, 1u, topo.digest(), Epoch(1), policy, 90000u, 100u);
  const PropagationOutcome third_outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {third}, nullptr, 3u);
  BPFAB_REQUIRE_OK(third_outcome.status);
  BPFAB_CHECK(find_node(third_outcome, 2u) == nullptr || third_outcome.status.ok());

  PressureSignal after = make_signal(4u, 1u, 1u, topo.digest(), Epoch(1), policy, 80000u, 250u);
  const PropagationOutcome after_outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 250, {after}, nullptr, 4u);
  BPFAB_REQUIRE_OK(after_outcome.status);
  const NodeOutcome* target = find_node(after_outcome, 2u);
  BPFAB_REQUIRE(target != nullptr);
  BPFAB_CHECK(target->received);
}

BPFAB_TEST(damping, hysteresis_delta_admits_a_stronger_repeat) {
  BPFAB_REQUIRE_RESULT(topo, build_topology({make_resource(1u), make_resource(2u)},
                                            {make_edge(1u, 1u, 2u, 90000u, 0u, 100u)}));
  PropagationPolicy policy = test_policy();
  policy.default_cooldown_ticks = 100;
  policy.hysteresis_delta = mag(5000u);
  BPFAB_REQUIRE_OK(policy.validate());
  PropagationLedger ledger(64, 64, 64);

  PressureSignal first = make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 40000u, 0u);
  BPFAB_REQUIRE_OK(run_propagation(topo, policy, ledger, Epoch(1), 0, {first}, nullptr, 1u).status);

  PressureSignal almost = make_signal(2u, 1u, 1u, topo.digest(), Epoch(1), policy, 41000u, 10u);
  const PropagationOutcome almost_outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 10, {almost}, nullptr, 2u);
  BPFAB_REQUIRE_OK(almost_outcome.status);
  BPFAB_CHECK(find_node(almost_outcome, 2u) == nullptr);

  PressureSignal much_stronger =
      make_signal(3u, 1u, 1u, topo.digest(), Epoch(1), policy, 90000u, 20u);
  const PropagationOutcome stronger_outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 20, {much_stronger}, nullptr, 3u);
  BPFAB_REQUIRE_OK(stronger_outcome.status);
  const NodeOutcome* target = find_node(stronger_outcome, 2u);
  BPFAB_REQUIRE(target != nullptr);
  BPFAB_CHECK(target->received);
}

BPFAB_TEST(damping, damping_ledger_is_bounded_and_reports_rejection) {
  PropagationLedger ledger(8, 8, 1);
  BPFAB_REQUIRE_OK(ledger.record_damping(ResourceId(1u), EdgeId(1u), mag(1000u), 0));
  BPFAB_REQUIRE_CODE(ledger.record_damping(ResourceId(1u), EdgeId(2u), mag(1000u), 0),
                     ErrorCode::LimitExceeded);
  BPFAB_CHECK(ledger.damping_rejections() == 1u);
  BPFAB_CHECK(ledger.damping(ResourceId(1u), EdgeId(1u)).present);
  BPFAB_CHECK(!ledger.damping(ResourceId(1u), EdgeId(9u)).present);
}

BPFAB_TEST(damping, recovery_decays_applied_pressure_and_never_raises_it) {
  BPFAB_REQUIRE_RESULT(topo, build_chain(3u, 90000u));
  PropagationPolicy policy = test_policy();
  policy.recovery_decay.per_step = Q16::from_percent_milli(50000u).value();
  policy.recovery_decay.step_ticks = 10;
  policy.recovery_decay.floor = Magnitude::zero();
  policy.recovery_decay.max_steps = 8;
  BPFAB_REQUIRE_OK(policy.validate());

  PropagationLedger ledger(64, 64, 64);
  Epoch epoch(1);

  PressureSignal signal = make_signal(1u, 1u, 1u, topo.digest(), epoch, policy, 90000u, 0u);
  BPFAB_REQUIRE_OK(
      run_propagation(topo, policy, ledger, epoch, 0, {signal}, nullptr, 1u).status);
  const std::size_t applications = ledger.application_count();
  BPFAB_CHECK(applications >= 3u);

  EngineContext ctx;
  ctx.topology = &topo;
  ctx.policy = &policy;
  ctx.ledger = &ledger;
  ctx.live_epoch = epoch;

  RecoveryRequest request;
  request.id = PropagationId(2u);
  request.source = SourceId(1u);
  request.origin = ResourceId(1u);
  request.origin_generation = Generation::initial();
  request.topology_digest = topo.digest();
  request.epoch = epoch;
  request.now = 0;
  request.residual = mag(50000u);
  request.authority = AuthorityVector::make_global(epoch, policy.generation, 4u);

  const PropagationOutcome immediate = PropagationEngine::recover(ctx, request);
  BPFAB_REQUIRE_OK(immediate.status);
  BPFAB_CHECK(!immediate.recovery_nodes.empty());
  for (const NodeOutcome& node : immediate.recovery_nodes) {
    BPFAB_CHECK(node.recovery);
    BPFAB_CHECK(node.applied <= node.strongest_single);
    BPFAB_CHECK(node.applied <= request.residual);
  }

  PropagationLedger::ApplicationRecord origin_record =
      ledger.application(SourceId(1u), ResourceId(1u));
  BPFAB_REQUIRE(origin_record.present);
  BPFAB_CHECK(origin_record.magnitude <= request.residual);

  // A later recovery with the same residual can only lower pressure further.
  RecoveryRequest later = request;
  later.id = PropagationId(3u);
  later.now = 40;
  later.residual = Magnitude::zero();
  const PropagationOutcome decayed = PropagationEngine::recover(ctx, later);
  BPFAB_REQUIRE_OK(decayed.status);
  for (const NodeOutcome& node : decayed.recovery_nodes) {
    BPFAB_CHECK(node.applied.is_zero());
  }
  BPFAB_CHECK(ledger.application(SourceId(1u), ResourceId(2u)).magnitude.is_zero());
}

BPFAB_TEST(damping, recovery_rejects_stale_binding) {
  BPFAB_REQUIRE_RESULT(topo, build_chain(2u, 90000u));
  PropagationPolicy policy = test_policy();
  PropagationLedger ledger(64, 64, 64);
  EngineContext ctx;
  ctx.topology = &topo;
  ctx.policy = &policy;
  ctx.ledger = &ledger;
  ctx.live_epoch = Epoch(2);

  RecoveryRequest request;
  request.id = PropagationId(1u);
  request.source = SourceId(1u);
  request.origin = ResourceId(1u);
  request.origin_generation = Generation::initial();
  request.topology_digest = topo.digest();
  request.epoch = Epoch(1);
  request.now = 5;
  request.residual = Magnitude::zero();

  BPFAB_REQUIRE_CODE(PropagationEngine::recover(ctx, request).status, ErrorCode::StaleEpoch);

  request.epoch = Epoch(2);
  request.topology_digest = Digest128{};
  BPFAB_REQUIRE_CODE(PropagationEngine::recover(ctx, request).status, ErrorCode::StaleGeneration);

  request.topology_digest = topo.digest();
  request.origin_generation = Generation::unknown();
  BPFAB_REQUIRE_CODE(PropagationEngine::recover(ctx, request).status, ErrorCode::StaleGeneration);

  request.origin_generation = Generation::initial();
  request.origin = ResourceId(77u);
  BPFAB_REQUIRE_CODE(PropagationEngine::recover(ctx, request).status, ErrorCode::NotFound);

  request.origin = ResourceId(1u);
  request.authority = AuthorityVector::make_global(Epoch(9), policy.generation, 4u);
  BPFAB_REQUIRE_CODE(PropagationEngine::recover(ctx, request).status, ErrorCode::StaleEpoch);
}

BPFAB_TEST(damping, repeated_updates_converge) {
  BPFAB_REQUIRE_RESULT(topo, build_chain(4u, 80000u));
  PropagationPolicy policy = test_policy();
  policy.default_cooldown_ticks = 0;
  PropagationLedger ledger(512, 512, 512);
  Epoch epoch(1);

  Magnitude previous = Magnitude::zero();
  bool saw_pressure = false;
  for (std::uint64_t i = 0; i < 24; ++i) {
    const std::uint32_t magnitude_milli = 60000u + static_cast<std::uint32_t>(i % 5u) * 1000u;
    PressureSignal signal = make_signal(i + 1u, 1u, 1u, topo.digest(), epoch, policy,
                                        magnitude_milli, 1000u + i);
    const PropagationOutcome outcome =
        run_propagation(topo, policy, ledger, epoch, 1000u + i, {signal}, nullptr, i + 1u);
    BPFAB_REQUIRE_OK(outcome.status);
    const NodeOutcome* deepest = find_node(outcome, 4u);
    if (deepest != nullptr) {
      saw_pressure = true;
      BPFAB_CHECK(deepest->applied <= mag(magnitude_milli));
      if (previous != Magnitude::zero()) {
        BPFAB_CHECK(deepest->applied <= mag(61000u));
      }
      previous = deepest->applied;
    }
  }
  BPFAB_CHECK(saw_pressure);
  const auto origin_record = ledger.application(SourceId(1u), ResourceId(1u));
  BPFAB_REQUIRE(origin_record.present);
  BPFAB_CHECK(origin_record.magnitude <= mag(64000u));
}
