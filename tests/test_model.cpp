// Backpressure Fabric - model layer tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;
using bpfab_test::make_edge;
using bpfab_test::make_resource;
using bpfab_test::test_policy;

BPFAB_TEST(model, severity_bands_are_ordered) {
  const SeverityThresholds thresholds = SeverityThresholds::defaults();
  BPFAB_REQUIRE_OK(thresholds.validate());
  BPFAB_CHECK(severity_from_magnitude(bpfab_test::mag(0u), thresholds) == Severity::Nominal);
  BPFAB_CHECK(severity_from_magnitude(bpfab_test::mag(10000u), thresholds) == Severity::Nominal);
  BPFAB_CHECK(severity_from_magnitude(bpfab_test::mag(50000u), thresholds) == Severity::Elevated);
  BPFAB_CHECK(severity_from_magnitude(bpfab_test::mag(70000u), thresholds) == Severity::High);
  BPFAB_CHECK(severity_from_magnitude(bpfab_test::mag(90000u), thresholds) == Severity::Critical);
  BPFAB_CHECK(severity_from_magnitude(bpfab_test::mag(100000u), thresholds) == Severity::Exhausted);

  SeverityThresholds broken = thresholds;
  broken.high_milli = broken.elevated_milli;
  BPFAB_REQUIRE_CODE(broken.validate(), ErrorCode::InvalidArgument);
}

BPFAB_TEST(model, unknown_pressure_never_authorizes) {
  const PressureObservation unknown = PressureObservation::unknown();
  BPFAB_CHECK(unknown.is_unknown());
  BPFAB_CHECK(!unknown.can_authorize_propagation());
  BPFAB_CHECK(unknown.severity() == Severity::Unknown);
  BPFAB_CHECK(unknown.magnitude().is_zero());

  const PressureObservation zero =
      PressureObservation::observed(Magnitude::zero(), SeverityThresholds::defaults());
  BPFAB_CHECK(!zero.is_unknown());
  BPFAB_CHECK(!zero.can_authorize_propagation());

  const PressureObservation real = PressureObservation::observed(
      bpfab_test::mag(50000u), SeverityThresholds::defaults());
  BPFAB_CHECK(real.can_authorize_propagation());
  BPFAB_CHECK(real.severity() == Severity::Elevated);

  const PressureObservation overstated =
      PressureObservation::observed_with_severity(bpfab_test::mag(10000u), Severity::Exhausted,
                                                  SeverityThresholds::defaults());
  BPFAB_CHECK(overstated.severity() == Severity::Nominal);
  BPFAB_CHECK(overstated.severity_downgraded());

  const PressureObservation understated =
      PressureObservation::observed_with_severity(bpfab_test::mag(90000u), Severity::Nominal,
                                                  SeverityThresholds::defaults());
  BPFAB_CHECK(understated.severity() == Severity::Nominal);
  BPFAB_CHECK(!understated.severity_downgraded());
}

BPFAB_TEST(model, authority_requires_epoch_scope_and_budget) {
  AuthorityVector none;
  BPFAB_REQUIRE_CODE(none.validate(), ErrorCode::Unauthorized);

  AuthorityVector unbound = AuthorityVector::make_global(Epoch::none(), Generation::initial(), 4);
  BPFAB_REQUIRE_CODE(unbound.validate(), ErrorCode::Unauthorized);

  AuthorityVector scoped;
  scoped.level = AuthorityLevel::Scoped;
  scoped.epoch = Epoch(1);
  scoped.policy_generation = Generation::initial();
  scoped.max_hops_granted = 4;
  BPFAB_REQUIRE_CODE(scoped.validate(), ErrorCode::Unauthorized);
  BPFAB_REQUIRE_OK(scoped.scope.add(ResourceId(7u)));
  BPFAB_REQUIRE_OK(scoped.validate());
  BPFAB_CHECK(scoped.covers(ResourceId(7u)));
  BPFAB_CHECK(!scoped.covers(ResourceId(8u)));
  BPFAB_REQUIRE_CODE(scoped.check(0, Epoch(2), ResourceId(7u)), ErrorCode::StaleEpoch);
  BPFAB_REQUIRE_CODE(scoped.check(0, Epoch(1), ResourceId(9u)), ErrorCode::Unauthorized);
  BPFAB_REQUIRE_OK(scoped.check(0, Epoch(1), ResourceId(7u)));

  AuthorityVector expiring = scoped;
  expiring.expires_at = 50;
  BPFAB_REQUIRE_CODE(expiring.check(51, Epoch(1), ResourceId(7u)), ErrorCode::Expired);

  AuthorityVector amplifying = AuthorityVector::make_global(Epoch(1), Generation::initial(), 4);
  amplifying.amplification_authorized = true;
  BPFAB_REQUIRE_CODE(amplifying.validate(), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_OK(amplifying.scope.add(ResourceId(1u)));
  amplifying.max_gain = Gain::from_percent_milli(150000u).value();
  BPFAB_REQUIRE_OK(amplifying.validate());

  AuthorityVector local = AuthorityVector::make_global(Epoch(1), Generation::initial(), 2);
  local.level = AuthorityLevel::Local;
  BPFAB_REQUIRE_OK(local.validate());
  BPFAB_CHECK(!local.covers(ResourceId(1u)));

  AuthorityVector observer = local;
  observer.level = AuthorityLevel::Observer;
  BPFAB_CHECK(!observer.permits_propagation());

  BPFAB_CHECK(scoped.digest() != expiring.digest());
  std::uint32_t accepted_entries = 0;
  for (std::uint32_t i = 0; i < AuthorityScope::kMaxEntries + 8u; ++i) {
    if (scoped.scope.add(ResourceId(100u + i)).ok()) {
      ++accepted_entries;
    }
  }
  BPFAB_CHECK(scoped.scope.size() == AuthorityScope::kMaxEntries);
  BPFAB_CHECK(accepted_entries + 1u == AuthorityScope::kMaxEntries);
  BPFAB_REQUIRE_CODE(scoped.scope.add(ResourceId(9999u)), ErrorCode::LimitExceeded);
  BPFAB_REQUIRE_OK(scoped.scope.add(ResourceId(7u)));
}

BPFAB_TEST(model, protection_classes_gate_receipt_and_forwarding) {
  const Resource plain = make_resource(1u);
  BPFAB_CHECK(plain.can_receive());
  BPFAB_CHECK(plain.can_forward());

  const Resource observed = make_resource(2u, ProtectionClass::ObservedOnly);
  BPFAB_CHECK(!observed.can_receive());
  BPFAB_CHECK(!observed.can_forward());

  const Resource barrier = make_resource(3u, ProtectionClass::Barrier);
  BPFAB_CHECK(barrier.can_receive());
  BPFAB_CHECK(!barrier.can_forward());

  const Resource sealed = make_resource(4u, ProtectionClass::Sealed);
  BPFAB_CHECK(!sealed.can_receive());

  Resource refusing = make_resource(5u);
  refusing.accepts_propagation = false;
  BPFAB_CHECK(!refusing.can_receive());

  Resource unbound = make_resource(6u);
  unbound.definition_generation = Generation::unknown();
  BPFAB_REQUIRE_CODE(unbound.validate(), ErrorCode::StaleGeneration);

  Resource bad_class = make_resource(7u);
  bad_class.klass = ResourceClass::Count;
  BPFAB_REQUIRE_CODE(bad_class.validate(), ErrorCode::InvalidArgument);
}

BPFAB_TEST(model, edges_validate_and_order_deterministically) {
  DependencyEdge edge = make_edge(1u, 10u, 20u, 50000u);
  BPFAB_REQUIRE_OK(edge.validate());
  DependencyEdge unbound = edge;
  unbound.dependency_generation = Generation::unknown();
  BPFAB_REQUIRE_CODE(unbound.validate(), ErrorCode::StaleGeneration);

  DependencyEdge barrier_amp = make_edge(
      2u, 10u, 20u, 50000u, 0u, 0u,
      EdgeFlags::of(EdgeFlagKind::Barrier) | EdgeFlagKind::Amplifying, 150000u);
  BPFAB_REQUIRE_CODE(barrier_amp.validate(), ErrorCode::Conflict);

  DependencyEdge too_many_hops = make_edge(3u, 10u, 20u);
  too_many_hops.max_hops = 1000u;
  BPFAB_REQUIRE_CODE(too_many_hops.validate(), ErrorCode::OutOfRange);

  DependencyEdge first = make_edge(5u, 1u, 9u);
  DependencyEdge second = make_edge(6u, 1u, 9u);
  second.priority = 1;
  BPFAB_CHECK(edge_precedes(first, second));
  DependencyEdge earlier_target = make_edge(7u, 1u, 3u);
  BPFAB_CHECK(edge_precedes(earlier_target, first));
  const DependencyEdge self = make_edge(8u, 4u, 4u);
  BPFAB_CHECK(self.is_self_loop());
  BPFAB_CHECK(!edge.is_self_loop());
}

BPFAB_TEST(model, topology_is_canonical_and_validated) {
  std::vector<Resource> resources = {make_resource(3u), make_resource(1u), make_resource(2u),
                                     make_resource(4u)};
  std::vector<DependencyEdge> edges = {make_edge(2u, 1u, 2u), make_edge(1u, 1u, 3u),
                                       make_edge(3u, 2u, 4u)};
  BPFAB_REQUIRE_RESULT(first, bpfab_test::build_topology(resources, edges));

  std::reverse(resources.begin(), resources.end());
  std::reverse(edges.begin(), edges.end());
  BPFAB_REQUIRE_RESULT(second, bpfab_test::build_topology(resources, edges));
  BPFAB_CHECK(first.digest() == second.digest());
  BPFAB_CHECK(first.generation() == second.generation());
  BPFAB_CHECK(first.stats().resource_count == 4u);
  BPFAB_CHECK(first.stats().edge_count == 3u);
  BPFAB_CHECK(first.out_degree(ResourceId(1u)) == 2u);
  BPFAB_CHECK(first.out_edge_indices(ResourceId(4u)).empty());
  BPFAB_CHECK(first.find_resource(ResourceId(2u)) != nullptr);
  BPFAB_CHECK(first.find_resource(ResourceId(99u)) == nullptr);
  BPFAB_CHECK(first.find_edge(EdgeId(2u)) != nullptr);
  BPFAB_CHECK(first.index_of(ResourceId(99u)) == Topology::kInvalidIndex);
  BPFAB_CHECK(!first.has_cycles());

  // The outgoing edges of resource 1 must be ordered by target identity.
  const auto outgoing = first.out_edge_indices(ResourceId(1u));
  BPFAB_REQUIRE(outgoing.size() == 2u);
  BPFAB_CHECK(first.edge_at(outgoing[0])->to == ResourceId(2u));
  BPFAB_CHECK(first.edge_at(outgoing[1])->to == ResourceId(3u));

  std::vector<Resource> duplicated = {make_resource(1u), make_resource(1u)};
  BPFAB_REQUIRE_CODE(bpfab_test::build_topology(duplicated, {}).status(), ErrorCode::Duplicate);

  std::vector<DependencyEdge> duplicated_edges = {make_edge(1u, 1u, 2u), make_edge(1u, 1u, 2u)};
  BPFAB_REQUIRE_CODE(
      bpfab_test::build_topology({make_resource(1u), make_resource(2u)}, duplicated_edges).status(),
      ErrorCode::Duplicate);

  BPFAB_REQUIRE_CODE(
      bpfab_test::build_topology({make_resource(1u)}, {make_edge(1u, 1u, 77u)}).status(),
      ErrorCode::NotFound);
  BPFAB_REQUIRE_CODE(
      bpfab_test::build_topology({make_resource(1u)}, {make_edge(1u, 77u, 1u)}).status(),
      ErrorCode::NotFound);

  TopologyLimits tight = bpfab_test::default_limits();
  tight.max_out_degree = 1;
  BPFAB_REQUIRE_CODE(Topology::build({make_resource(1u), make_resource(2u), make_resource(3u)},
                                     {make_edge(1u, 1u, 2u), make_edge(2u, 1u, 3u)}, tight, true)
                         .status(),
                     ErrorCode::LimitExceeded);

  TopologyLimits small = bpfab_test::default_limits();
  small.max_resources = 1;
  BPFAB_REQUIRE_CODE(Topology::build({make_resource(1u), make_resource(2u)}, {}, small, true).status(),
                     ErrorCode::LimitExceeded);
}

BPFAB_TEST(model, topology_reports_cycles_and_can_refuse_them) {
  std::vector<Resource> resources = {make_resource(1u), make_resource(2u), make_resource(3u),
                                     make_resource(4u)};
  std::vector<DependencyEdge> edges = {make_edge(1u, 1u, 2u), make_edge(2u, 2u, 3u),
                                       make_edge(3u, 3u, 1u), make_edge(4u, 3u, 4u)};
  BPFAB_REQUIRE_CODE(bpfab_test::build_topology(resources, edges, false).status(),
                     ErrorCode::CycleDetected);
  BPFAB_REQUIRE_RESULT(cyclic, bpfab_test::build_topology(resources, edges, true));
  BPFAB_CHECK(cyclic.has_cycles());
  BPFAB_CHECK(cyclic.stats().cyclic_resource_count == 3u);
  BPFAB_CHECK(cyclic.stats().strongly_connected_components == 1u);
  const std::vector<ResourceId>& members = cyclic.cyclic_resources();
  BPFAB_CHECK(std::find(members.begin(), members.end(), ResourceId(4u)) == members.end());
  BPFAB_CHECK(std::find(members.begin(), members.end(), ResourceId(1u)) != members.end());

  std::vector<Resource> self_resources = {make_resource(1u)};
  std::vector<DependencyEdge> self_edges = {make_edge(1u, 1u, 1u)};
  BPFAB_REQUIRE_RESULT(self_loop, bpfab_test::build_topology(self_resources, self_edges, true));
  BPFAB_CHECK(self_loop.stats().self_loop_count == 1u);
  BPFAB_CHECK(self_loop.has_cycles());
  BPFAB_CHECK(self_loop.stats().cyclic_resource_count == 1u);

  bool saw_self_loop_issue = false;
  for (const TopologyIssue& issue : self_loop.issues()) {
    if (issue.kind == TopologyIssueKind::SelfLoop) {
      saw_self_loop_issue = true;
    }
  }
  BPFAB_CHECK(saw_self_loop_issue);
}

BPFAB_TEST(model, policy_validation_bounds_every_budget) {
  PropagationPolicy policy = test_policy();
  BPFAB_REQUIRE_OK(policy.validate());

  PropagationPolicy no_id = policy;
  no_id.id = PolicyId{};
  BPFAB_REQUIRE_CODE(no_id.validate(), ErrorCode::InvalidArgument);

  PropagationPolicy no_generation = policy;
  no_generation.generation = Generation::unknown();
  BPFAB_REQUIRE_CODE(no_generation.validate(), ErrorCode::StaleGeneration);

  PropagationPolicy hops = policy;
  hops.max_hops = 0;
  BPFAB_REQUIRE_CODE(hops.validate(), ErrorCode::OutOfRange);
  hops.max_hops = 65;
  BPFAB_REQUIRE_CODE(hops.validate(), ErrorCode::OutOfRange);

  PropagationPolicy budget = policy;
  budget.max_records = 1;
  budget.max_path_records = 1;
  BPFAB_REQUIRE_CODE(budget.validate(), ErrorCode::InvalidArgument);

  PropagationPolicy gain = policy;
  gain.max_cumulative_gain = Gain::from_percent_milli(200000u).value();
  BPFAB_REQUIRE_CODE(gain.validate(), ErrorCode::Conflict);

  PropagationPolicy unknown = policy;
  unknown.require_known_pressure = false;
  BPFAB_REQUIRE_CODE(unknown.validate(), ErrorCode::InvalidArgument);

  PropagationPolicy ceiling = policy;
  ceiling.aggregation_ceiling = Magnitude::zero();
  BPFAB_REQUIRE_CODE(ceiling.validate(), ErrorCode::InvalidArgument);

  PropagationPolicy rule = policy;
  rule.aggregation = AggregationRule::Count;
  BPFAB_REQUIRE_CODE(rule.validate(), ErrorCode::InvalidArgument);

  BPFAB_CHECK(policy.digest() != hops.digest());
}

BPFAB_TEST(model, decay_schedule_only_lowers_pressure) {
  DecaySchedule schedule;
  schedule.per_step = Q16::from_percent_milli(50000u).value();
  schedule.step_ticks = 5;
  schedule.floor = bpfab_test::mag(1000u);
  schedule.max_steps = 8;
  BPFAB_REQUIRE_OK(schedule.validate());

  const Magnitude start = Magnitude::full();
  Magnitude previous = start;
  for (std::uint32_t step = 0; step <= 12; ++step) {
    const Magnitude decayed = schedule.decay(start, step);
    BPFAB_CHECK(decayed <= previous);
    BPFAB_CHECK(decayed >= schedule.floor);
    previous = decayed;
  }
  BPFAB_CHECK(schedule.decay(start, 0) == start);
  BPFAB_CHECK(schedule.decay(start, 8) == schedule.floor);

  DecaySchedule invalid = schedule;
  invalid.step_ticks = 0;
  BPFAB_REQUIRE_CODE(invalid.validate(), ErrorCode::InvalidArgument);
  invalid = schedule;
  invalid.max_steps = 0;
  BPFAB_REQUIRE_CODE(invalid.validate(), ErrorCode::OutOfRange);
}

BPFAB_TEST(model, signal_lineage_is_content_addressed) {
  const PropagationPolicy policy = test_policy();
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology({make_resource(1u), make_resource(2u)},
                                                        {make_edge(1u, 1u, 2u)}));
  PressureSignal signal =
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy);
  BPFAB_CHECK(!signal.lineage.is_zero());
  BPFAB_REQUIRE_OK(signal.validate(policy, 0));

  const Digest128 original = signal.lineage;
  signal.bind_lineage();
  BPFAB_CHECK(signal.lineage == original);

  PressureSignal replay = signal;
  replay.bind_lineage();
  BPFAB_CHECK(replay.lineage == original);

  PressureSignal later = signal;
  later.issued_at = signal.issued_at + 1u;
  later.bind_lineage();
  BPFAB_CHECK(later.lineage != original);

  PressureSignal stronger = signal;
  stronger.observation = PressureObservation::observed(bpfab_test::mag(90000u), policy.thresholds);
  stronger.bind_lineage();
  BPFAB_CHECK(stronger.lineage != original);

  PressureSignal unknown = signal;
  unknown.observation = PressureObservation::unknown();
  unknown.bind_lineage();
  BPFAB_REQUIRE_CODE(unknown.validate(policy, 0), ErrorCode::UnknownPressure);

  PressureSignal stale_epoch = signal;
  stale_epoch.authority.epoch = Epoch(2);
  stale_epoch.bind_lineage();
  BPFAB_REQUIRE_CODE(stale_epoch.validate(policy, 0), ErrorCode::StaleEpoch);

  PressureSignal wrong_policy = signal;
  wrong_policy.authority.policy_generation = Generation::from_raw(99u);
  wrong_policy.bind_lineage();
  BPFAB_REQUIRE_CODE(wrong_policy.validate(policy, 0), ErrorCode::StaleGeneration);

  PressureSignal no_topology = signal;
  no_topology.topology_digest = Digest128{};
  no_topology.bind_lineage();
  BPFAB_REQUIRE_CODE(no_topology.validate(policy, 0), ErrorCode::StaleGeneration);

  PressureSignal no_provenance = signal;
  no_provenance.provenance = Provenance{};
  no_provenance.bind_lineage();
  BPFAB_REQUIRE_CODE(no_provenance.validate(policy, 0), ErrorCode::InvalidArgument);

  PressureSignal no_publisher = signal;
  no_publisher.publisher = Incarnation{};
  no_publisher.bind_lineage();
  BPFAB_REQUIRE_CODE(no_publisher.validate(policy, 0), ErrorCode::StaleIncarnation);

  PressureSignal expired = signal;
  expired.valid_until = 10;
  expired.bind_lineage();
  BPFAB_REQUIRE_CODE(expired.validate(policy, 11), ErrorCode::Expired);
  BPFAB_REQUIRE_OK(expired.validate(policy, 10));
  BPFAB_CHECK(expired.deadline(policy) == 10u);

  PressureSignal observer = signal;
  observer.authority.level = AuthorityLevel::Observer;
  observer.bind_lineage();
  BPFAB_REQUIRE_CODE(observer.validate(policy, 0), ErrorCode::Unauthorized);
}

BPFAB_TEST(model, provenance_chain_is_bounded_and_commits_to_history) {
  Provenance provenance;
  BPFAB_CHECK(provenance.empty());
  for (std::uint32_t i = 0; i < Provenance::kMaxChain + 4u; ++i) {
    ProvenanceHop hop;
    hop.publisher = bpfab_test::test_incarnation(1u, 100u + i);
    hop.epoch = Epoch(1);
    hop.generation = Generation::initial();
    hop.tick = i;
    provenance.push(hop);
  }
  BPFAB_CHECK(provenance.count == Provenance::kMaxChain);
  BPFAB_CHECK(provenance.evictions == 4u);
  BPFAB_CHECK(!provenance.dropped_prefix.is_zero());
  BPFAB_CHECK(provenance.latest().tick == Provenance::kMaxChain + 3u);
  const Digest128 first = provenance.chain_digest();
  ProvenanceHop extra;
  extra.tick = 999;
  provenance.push(extra);
  BPFAB_CHECK(provenance.chain_digest() != first);
}