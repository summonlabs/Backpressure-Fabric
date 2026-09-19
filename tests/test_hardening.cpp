// Backpressure Fabric - hardening tests.
//
// Exercises the failure and boundary paths that only appear once the happy path
// is green: lifecycle misuse, capacity exhaustion, refusing origins, lock
// ownership, and cancellation boundaries.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;
using bpfab_test::find_node;
using bpfab_test::make_edge;
using bpfab_test::make_resource;
using bpfab_test::run_propagation;

namespace {

struct Fixture {
  FabricConfig config;
  std::unique_ptr<Fabric> fabric;

  Status configure(std::size_t workers = 0, std::size_t queue = 8) {
    config.worker_threads = workers;
    config.worker_queue_capacity = queue;
    config.ledger_lineage_capacity = 128;
    config.ledger_application_capacity = 8;
    Result<std::unique_ptr<Fabric>> opened = Fabric::open(config);
    if (!opened.ok()) {
      return opened.status();
    }
    fabric = std::move(opened).value();
    Status status = fabric->install_topology({make_resource(1u), make_resource(2u)},
                                             {make_edge(1u, 1u, 2u, 90000u)}, 0);
    if (!status.ok()) {
      return status;
    }
    return fabric->install_policy(bpfab_test::test_policy(), 0);
  }
};

}  // namespace

BPFAB_TEST(hardening, durable_store_admits_only_one_writer_per_process) {
  const std::string directory = bpfab_test::make_scratch_directory("onelock");
  DurableConfig config;
  config.directory = directory;
  BPFAB_REQUIRE_RESULT(first, DurableStore::open(config));
  BPFAB_CHECK(first.locked());
  const Result<DurableStore> second = DurableStore::open(config);
  BPFAB_REQUIRE_CODE(second.status(), ErrorCode::Conflict);
  BPFAB_REQUIRE_OK(first.sync());
  {
    DurableStore moved = std::move(first);
    BPFAB_CHECK(moved.locked());
  }
  // The move destroyed the first owner, so the directory is free again.
  BPFAB_REQUIRE_RESULT(third, DurableStore::open(config));
  BPFAB_CHECK(third.locked());
}

BPFAB_TEST(hardening, durable_directory_must_be_a_directory) {
  const std::string directory = bpfab_test::make_scratch_directory("notadir");
  const std::string file_path = (std::filesystem::path(directory) / "blocker").string();
  {
    std::ofstream stream(file_path, std::ios::binary);
    stream << "not a directory";
  }
  DurableConfig config;
  config.directory = file_path;
  BPFAB_REQUIRE_CODE(DurableStore::open(config).status(), ErrorCode::IoError);

  FabricConfig fabric_config;
  fabric_config.state_directory = file_path;
  BPFAB_REQUIRE_CODE(Fabric::open(fabric_config).status(), ErrorCode::IoError);
}

BPFAB_TEST(hardening, worker_pool_lifecycle_is_checked) {
  WorkerPool pool;
  BPFAB_REQUIRE_CODE(pool.start(0, 4), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_CODE(pool.start(1, 0), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_OK(pool.start(2, 4));
  BPFAB_REQUIRE_CODE(pool.start(2, 4), ErrorCode::Conflict);
  BPFAB_REQUIRE_CODE(pool.submit(std::function<void()>()), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_OK(pool.shutdown());
  BPFAB_REQUIRE_CODE(pool.submit([]() {}), ErrorCode::NotReady);
  BPFAB_REQUIRE_OK(pool.shutdown());
}

BPFAB_TEST(hardening, asynchronous_publish_requires_workers) {
  Fixture fixture;
  BPFAB_REQUIRE_OK(fixture.configure(0, 8));
  const Incarnation incarnation = bpfab_test::test_incarnation(1u, 1u);
  BPFAB_REQUIRE_OK(fixture.fabric->register_publisher(SourceId(1u), incarnation, 0));
  const auto topology = fixture.fabric->topology();
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topology->digest(),
                                                  fixture.fabric->epoch(),
                                                  bpfab_test::test_policy(), 90000u, 0u,
                                                  AuthorityLevel::Global, 8u, incarnation);
  BPFAB_REQUIRE_CODE(fixture.fabric->publish_async(signal, 0).status(), ErrorCode::NotReady);
}

BPFAB_TEST(hardening, cancellation_after_completion_cannot_undo_a_commit) {
  Fixture fixture;
  BPFAB_REQUIRE_OK(fixture.configure(1, 8));
  const Incarnation incarnation = bpfab_test::test_incarnation(2u, 2u);
  BPFAB_REQUIRE_OK(fixture.fabric->register_publisher(SourceId(1u), incarnation, 0));
  const auto topology = fixture.fabric->topology();
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topology->digest(),
                                                  fixture.fabric->epoch(),
                                                  bpfab_test::test_policy(), 90000u, 0u,
                                                  AuthorityLevel::Global, 8u, incarnation);
  BPFAB_REQUIRE_RESULT(ticket_id, fixture.fabric->publish_async(signal, 0));
  BPFAB_REQUIRE_OK(fixture.fabric->drain());
  BPFAB_REQUIRE_RESULT(committed, fixture.fabric->ticket(ticket_id));
  BPFAB_CHECK(committed.committed);
  BPFAB_CHECK(committed.state == TicketState::Completed);
  BPFAB_REQUIRE_OK(fixture.fabric->cancel(ticket_id));
  BPFAB_REQUIRE_RESULT(after, fixture.fabric->ticket(ticket_id));
  BPFAB_CHECK(after.committed);
  BPFAB_CHECK(after.state == TicketState::Completed);
  BPFAB_CHECK(after.status.ok());
  BPFAB_REQUIRE_OK(fixture.fabric->release_ticket(ticket_id));
  // The pressure applied by the committed publish is still recorded.
  BPFAB_CHECK(fixture.fabric->status().signals_accepted == 1u);
}

BPFAB_TEST(hardening, application_ledger_capacity_is_enforced) {
  Fixture fixture;
  BPFAB_REQUIRE_OK(fixture.configure(0, 8));
  const PropagationPolicy policy = bpfab_test::test_policy();
  const auto topology = fixture.fabric->topology();
  for (std::uint32_t s = 1; s <= 16u; ++s) {
    const Incarnation incarnation = bpfab_test::test_incarnation(s, s);
    BPFAB_REQUIRE_OK(fixture.fabric->register_publisher(SourceId(s), incarnation, 0));
    PressureSignal signal = bpfab_test::make_signal(s, s, 1u, topology->digest(),
                                                    fixture.fabric->epoch(), policy, 90000u, 0u,
                                                    AuthorityLevel::Global, 8u, incarnation);
    const Result<PropagationOutcome> outcome = fixture.fabric->publish(signal, 0);
    if (bpfab_test::outcome_code(outcome) == ErrorCode::Ok) {
      continue;
    }
    BPFAB_CHECK(bpfab_test::outcome_code(outcome) == ErrorCode::LimitExceeded);
    BPFAB_CHECK(fixture.fabric->status().signals_accepted < 16u);
    return;
  }
  BPFAB_CHECK(false);
}

BPFAB_TEST(hardening, refusing_origin_resource_blocks_everything) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(
                                 {make_resource(1u, ProtectionClass::None,
                                                ResourceClass::QueueGroup, false),
                                  make_resource(2u)},
                                 {make_edge(1u, 1u, 2u, 90000u)}));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                  90000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* origin = find_node(outcome, 1u);
  BPFAB_REQUIRE(origin != nullptr);
  BPFAB_CHECK(!origin->received);
  BPFAB_CHECK(origin->blocked_reason == SuppressionReason::ObservedOnlyResource);
  BPFAB_CHECK(find_node(outcome, 2u) == nullptr);
  BPFAB_CHECK(!outcome.explanations[0].suppressed.empty());
  BPFAB_CHECK(outcome.explanations[0].suppressed[0].count == 1u);
}

BPFAB_TEST(hardening, fence_table_capacity_is_bounded) {
  FenceTable fences;
  for (std::uint32_t i = 0; i < FenceTable::kMaxFences; ++i) {
    Fence fence;
    fence.id = FenceId(i + 1u);
    fence.resource = ResourceId(i + 1u);
    fence.kind = FenceKind::Barrier;
    fence.epoch = Epoch(1);
    BPFAB_REQUIRE(fences.add(fence).ok());
  }
  Fence overflow;
  overflow.id = FenceId(FenceTable::kMaxFences + 1u);
  overflow.resource = ResourceId(1u);
  overflow.kind = FenceKind::Barrier;
  overflow.epoch = Epoch(1);
  BPFAB_REQUIRE_CODE(fences.add(overflow), ErrorCode::LimitExceeded);
  // Replacing an existing fence is always allowed.
  overflow.id = FenceId(1u);
  BPFAB_REQUIRE_OK(fences.add(overflow));
  BPFAB_CHECK(fences.size() == FenceTable::kMaxFences);
}

BPFAB_TEST(hardening, journal_file_size_is_bounded) {
  const std::string directory = bpfab_test::make_scratch_directory("filebound");
  const std::string path = (std::filesystem::path(directory) / "bounded.bpfj").string();
  JournalLimits limits;
  limits.max_file_bytes = Journal::kHeaderBytes + Journal::kRecordHeaderBytes + 4u;
  BPFAB_REQUIRE_RESULT(journal, Journal::open(path, limits, true));
  const std::vector<std::byte> payload(4u, std::byte{1});
  BPFAB_REQUIRE_OK(journal.append(RecordType::Lineage, 1, Epoch(1), payload));
  BPFAB_REQUIRE_CODE(journal.append(RecordType::Lineage, 2, Epoch(1), payload),
                     ErrorCode::LimitExceeded);
}

BPFAB_TEST(hardening, recovery_dry_run_does_not_mutate_applied_pressure) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(3u, 90000u));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(64, 64, 64);
  const Epoch epoch(1);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), epoch, policy,
                                                  90000u, 0u);
  BPFAB_REQUIRE_OK(run_propagation(topo, policy, ledger, epoch, 0, {signal}, nullptr, 1u).status);
  const auto before = ledger.application(SourceId(1u), ResourceId(1u));
  BPFAB_REQUIRE(before.present);

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
  request.now = 100;
  request.residual = Magnitude::zero();
  request.dry_run = true;

  const PropagationOutcome planned = PropagationEngine::recover(ctx, request);
  BPFAB_REQUIRE_OK(planned.status);
  BPFAB_CHECK(!planned.recovery_nodes.empty());
  const auto after = ledger.application(SourceId(1u), ResourceId(1u));
  BPFAB_CHECK(after.magnitude == before.magnitude);

  request.dry_run = false;
  const PropagationOutcome applied = PropagationEngine::recover(ctx, request);
  BPFAB_REQUIRE_OK(applied.status);
  BPFAB_CHECK(ledger.application(SourceId(1u), ResourceId(1u)).magnitude.is_zero());
}

BPFAB_TEST(hardening, topology_edge_limit_is_enforced_before_allocation) {
  TopologyLimits limits = bpfab_test::default_limits();
  limits.max_edges = 2;
  std::vector<DependencyEdge> edges;
  for (std::uint32_t i = 0; i < 4u; ++i) {
    edges.push_back(make_edge(i + 1u, 1u, 2u + i));
  }
  std::vector<Resource> resources = {make_resource(1u), make_resource(2u), make_resource(3u),
                                     make_resource(4u), make_resource(5u)};
  BPFAB_REQUIRE_CODE(Topology::build(resources, edges, limits, true).status(),
                     ErrorCode::LimitExceeded);
}

BPFAB_TEST(hardening, fabric_shutdown_is_idempotent_and_refuses_new_work) {
  Fixture fixture;
  BPFAB_REQUIRE_OK(fixture.configure(2, 8));
  BPFAB_REQUIRE_OK(fixture.fabric->shutdown(1));
  BPFAB_REQUIRE_OK(fixture.fabric->shutdown(2));
  BPFAB_REQUIRE_OK(fixture.fabric->shutdown(3));
  BPFAB_CHECK(fixture.fabric->status().shutting_down);
  BPFAB_REQUIRE_CODE(fixture.fabric->install_topology({make_resource(1u)}, {}, 4),
                     ErrorCode::ShuttingDown);
  BPFAB_REQUIRE_CODE(fixture.fabric->install_policy(bpfab_test::test_policy(), 4),
                     ErrorCode::ShuttingDown);
  BPFAB_REQUIRE_CODE(fixture.fabric->register_publisher(SourceId(1u),
                                                        bpfab_test::test_incarnation(),
                                                        4),
                     ErrorCode::ShuttingDown);
  BPFAB_REQUIRE_CODE(fixture.fabric->put_fence(Fence{}, 4), ErrorCode::ShuttingDown);
  BPFAB_REQUIRE_CODE(fixture.fabric->mark_revalidation_required(ResourceId(1u), 4),
                     ErrorCode::ShuttingDown);
  // Clearing a stale marker stays available: it is cleanup, not new work.
  BPFAB_REQUIRE_OK(fixture.fabric->clear_revalidation(ResourceId(1u), 4));
}

BPFAB_TEST(hardening, revalidation_is_bounded_and_tracked) {
  FabricConfig config;
  config.max_revalidation_entries = 2;
  BPFAB_REQUIRE_RESULT(fabric, Fabric::open(config));
  BPFAB_REQUIRE_OK(fabric->mark_revalidation_required(ResourceId(1u), 0));
  BPFAB_REQUIRE_OK(fabric->mark_revalidation_required(ResourceId(1u), 0));
  BPFAB_CHECK(fabric->revalidation_required().size() == 1u);
  BPFAB_REQUIRE_OK(fabric->mark_revalidation_required(ResourceId(2u), 0));
  BPFAB_REQUIRE_CODE(fabric->mark_revalidation_required(ResourceId(3u), 0),
                     ErrorCode::LimitExceeded);
  BPFAB_REQUIRE_OK(fabric->clear_revalidation(ResourceId(1u), 0));
  BPFAB_CHECK(fabric->revalidation_required().size() == 1u);
  BPFAB_REQUIRE_OK(fabric->clear_revalidation(ResourceId(99u), 0));
  BPFAB_REQUIRE_OK(fabric->shutdown(1));
}