// Backpressure Fabric - concurrency, cancellation and shutdown tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;
using bpfab_test::make_edge;
using bpfab_test::make_resource;

namespace {

struct Harness {
  FabricConfig config;
  std::unique_ptr<Fabric> fabric;

  Status configure(std::size_t workers = 0, std::size_t queue = 16) {
    config.worker_threads = workers;
    config.worker_queue_capacity = queue;
    config.ledger_lineage_capacity = 1u << 14;
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

BPFAB_TEST(concurrency, parallel_publishes_are_serialised_and_all_accounted_for) {
  Harness harness;
  BPFAB_REQUIRE_OK(harness.configure());

  const Incarnation incarnation = bpfab_test::test_incarnation(1u, 900u);
  const std::uint32_t thread_count = 8;
  for (std::uint32_t t = 0; t < thread_count; ++t) {
    BPFAB_REQUIRE_OK(harness.fabric->register_publisher(SourceId(t + 1u), incarnation, 0));
  }

  const auto topology = harness.fabric->topology();
  const Epoch epoch = harness.fabric->epoch();
  const PropagationPolicy policy = bpfab_test::test_policy();

  std::atomic<std::uint32_t> accepted{0};
  std::atomic<std::uint32_t> rejected{0};
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (std::uint32_t t = 0; t < thread_count; ++t) {
    threads.emplace_back([&, t]() {
      for (std::uint32_t i = 0; i < 24u; ++i) {
        PressureSignal signal = bpfab_test::make_signal(
            static_cast<std::uint64_t>(t) * 1000u + i + 1u, t + 1u, 1u, topology->digest(), epoch,
            policy, 70000u + i, 100u + i, AuthorityLevel::Global, 8u, incarnation);
        const Result<PropagationOutcome> outcome = harness.fabric->publish(signal, 100u + i);
        if (outcome.ok() && outcome.value().status.ok()) {
          accepted.fetch_add(1);
        } else {
          rejected.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  BPFAB_CHECK(accepted.load() == thread_count * 24u);
  BPFAB_CHECK(rejected.load() == 0u);
  const FabricStatus status = harness.fabric->status();
  BPFAB_CHECK(status.signals_accepted == thread_count * 24u);
  BPFAB_CHECK(status.signals_rejected == 0u);
  BPFAB_CHECK(status.live_publishers == thread_count);
}

BPFAB_TEST(concurrency, topology_reinstall_races_with_publishers_without_corruption) {
  Harness harness;
  BPFAB_REQUIRE_OK(harness.configure());
  const Incarnation incarnation = bpfab_test::test_incarnation(2u, 901u);
  BPFAB_REQUIRE_OK(harness.fabric->register_publisher(SourceId(1u), incarnation, 0));
  const PropagationPolicy policy = bpfab_test::test_policy();

  constexpr std::uint32_t kPublishers = 4u;
  constexpr std::uint32_t kIterations = 256u;
  constexpr std::uint32_t kInstallTarget = 128u;

  std::atomic<std::uint32_t> attempted{0};
  std::atomic<std::uint32_t> ok_count{0};
  std::atomic<std::uint32_t> stale_count{0};
  std::atomic<std::uint32_t> other_count{0};

  std::vector<std::thread> publishers;
  publishers.reserve(kPublishers);
  for (std::uint32_t t = 0; t < kPublishers; ++t) {
    publishers.emplace_back([&, t]() {
      for (std::uint32_t counter = 0; counter < kIterations; ++counter) {
        const auto topology = harness.fabric->topology();
        PressureSignal signal = bpfab_test::make_signal(
            static_cast<std::uint64_t>(t) * 100000u + counter + 1u, 1u, 1u, topology->digest(),
            harness.fabric->epoch(), policy, 80000u, 100u, AuthorityLevel::Global, 8u, incarnation);
        const Result<PropagationOutcome> outcome = harness.fabric->publish(signal, 100u);
        if (outcome.ok() && outcome.value().status.ok()) {
          ok_count.fetch_add(1);
        } else if (outcome.status().code() == ErrorCode::StaleGeneration) {
          stale_count.fetch_add(1);
        } else {
          other_count.fetch_add(1);
        }
        attempted.fetch_add(1);
      }
    });
  }

  // Reinstall topologies while the publishers are running, then stop changing
  // the topology so that the tail of every publisher's run must succeed.
  std::uint32_t installs = 0;
  while (attempted.load() < kInstallTarget && installs < 256u) {
    std::vector<Resource> resources = {make_resource(1u), make_resource(2u)};
    std::vector<DependencyEdge> edges = {make_edge(1u, 1u, 2u, 90000u)};
    if (installs % 2u == 1u) {
      resources.push_back(make_resource(3u));
      edges.push_back(make_edge(2u, 2u, 3u, 90000u));
    }
    BPFAB_REQUIRE_OK(harness.fabric->install_topology(std::move(resources), std::move(edges),
                                                      static_cast<Tick>(installs)));
    ++installs;
    std::this_thread::yield();
  }
  for (std::thread& thread : publishers) {
    thread.join();
  }
  BPFAB_CHECK_MSG(ok_count.load() > 0u,
                  "ok=" + std::to_string(ok_count.load()) +
                      " stale=" + std::to_string(stale_count.load()) +
                      " other=" + std::to_string(other_count.load()));
  BPFAB_CHECK(other_count.load() == 0u);
  BPFAB_CHECK(ok_count.load() + stale_count.load() == kPublishers * kIterations);
  const FabricStatus status = harness.fabric->status();
  BPFAB_CHECK(status.topologies_installed == 1u + installs);
}

BPFAB_TEST(concurrency, worker_pool_bounds_queue_and_stops_cleanly) {
  WorkerPool pool;
  BPFAB_REQUIRE_CODE(pool.submit([]() {}), ErrorCode::NotReady);
  BPFAB_REQUIRE_OK(pool.start(2, 4));

  std::atomic<std::uint32_t> ran{0};
  std::atomic<bool> release{false};
  for (std::uint32_t i = 0; i < 4u; ++i) {
    BPFAB_REQUIRE_OK(pool.submit([&ran, &release]() {
      while (!release.load()) {
        std::this_thread::yield();
      }
      ran.fetch_add(1);
    }));
  }
  Status overflow = Status::success();
  for (std::uint32_t i = 0; i < 32u && overflow.ok(); ++i) {
    overflow = pool.submit([]() {});
  }
  BPFAB_CHECK(overflow.code() == ErrorCode::LimitExceeded);
  release.store(true);
  BPFAB_REQUIRE_OK(pool.drain());
  BPFAB_CHECK(ran.load() == 4u);

  BPFAB_REQUIRE_OK(pool.shutdown());
  BPFAB_REQUIRE_CODE(pool.submit([]() {}), ErrorCode::NotReady);
  const std::uint64_t completed = pool.completed();
  BPFAB_REQUIRE_OK(pool.drain());
  BPFAB_CHECK(pool.completed() >= completed);

  WorkerPool second;
  BPFAB_REQUIRE_OK(second.start(1, 8));
  BPFAB_REQUIRE_OK(second.abort());
  BPFAB_REQUIRE_CODE(second.submit([]() {}), ErrorCode::NotReady);

  WorkerPool third;
  BPFAB_REQUIRE_CODE(third.start(0, 8), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_CODE(third.start(1, 0), ErrorCode::InvalidArgument);
}

BPFAB_TEST(concurrency, asynchronous_publishes_complete_and_commit) {
  Harness harness;
  BPFAB_REQUIRE_OK(harness.configure(3, 32));
  const Incarnation incarnation = bpfab_test::test_incarnation(3u, 902u);
  for (std::uint32_t s = 1; s <= 8u; ++s) {
    BPFAB_REQUIRE_OK(harness.fabric->register_publisher(SourceId(s), incarnation, 0));
  }
  const auto topology = harness.fabric->topology();
  const Epoch epoch = harness.fabric->epoch();
  const PropagationPolicy policy = bpfab_test::test_policy();

  std::vector<TicketId> tickets;
  for (std::uint32_t s = 1; s <= 8u; ++s) {
    for (std::uint32_t i = 0; i < 4u; ++i) {
      PressureSignal signal = bpfab_test::make_signal(s * 100u + i, s, 1u, topology->digest(), epoch,
                                                      policy, 80000u, 100u, AuthorityLevel::Global,
                                                      8u, incarnation);
      BPFAB_REQUIRE_RESULT(ticket, harness.fabric->publish_async(signal, 100u));
      tickets.push_back(ticket);
    }
  }
  BPFAB_REQUIRE_OK(harness.fabric->drain());
  std::uint32_t committed = 0;
  for (const TicketId id : tickets) {
    BPFAB_REQUIRE_RESULT(ticket, harness.fabric->ticket(id));
    BPFAB_CHECK(ticket.state == TicketState::Completed || ticket.state == TicketState::Failed);
    if (ticket.committed) {
      ++committed;
    }
    BPFAB_REQUIRE_OK(harness.fabric->release_ticket(id));
  }
  BPFAB_CHECK(committed == tickets.size());
  BPFAB_CHECK(harness.fabric->pending_tickets() == 0u);
  BPFAB_CHECK(harness.fabric->status().signals_accepted == tickets.size());
  BPFAB_REQUIRE_CODE(harness.fabric->ticket(99999u).status(), ErrorCode::NotFound);
  BPFAB_REQUIRE_CODE(harness.fabric->release_ticket(99999u), ErrorCode::NotFound);
  BPFAB_REQUIRE_CODE(harness.fabric->cancel(99999u), ErrorCode::NotFound);
}

BPFAB_TEST(concurrency, cancelled_work_never_commits_and_never_reports_success) {
  Harness harness;
  BPFAB_REQUIRE_OK(harness.configure(4, 64));
  const Incarnation incarnation = bpfab_test::test_incarnation(4u, 903u);
  for (std::uint32_t s = 1; s <= 32u; ++s) {
    BPFAB_REQUIRE_OK(harness.fabric->register_publisher(SourceId(s), incarnation, 0));
  }
  const auto topology = harness.fabric->topology();
  const Epoch epoch = harness.fabric->epoch();
  const PropagationPolicy policy = bpfab_test::test_policy();

  std::vector<TicketId> tickets;
  for (std::uint32_t s = 1; s <= 32u; ++s) {
    PressureSignal signal = bpfab_test::make_signal(s, s, 1u, topology->digest(), epoch, policy,
                                                    90000u, 100u, AuthorityLevel::Global, 8u,
                                                    incarnation);
    Result<TicketId> ticket = harness.fabric->publish_async(signal, 100u);
    BPFAB_REQUIRE(ticket.ok());
    tickets.push_back(ticket.value());
    if (s % 2u == 0u) {
      BPFAB_REQUIRE_OK(harness.fabric->cancel(tickets.back()));
    }
  }
  BPFAB_REQUIRE_OK(harness.fabric->drain());

  std::uint32_t committed = 0;
  for (const TicketId id : tickets) {
    BPFAB_REQUIRE_RESULT(ticket, harness.fabric->ticket(id));
    if (ticket.cancelled) {
      BPFAB_CHECK(!ticket.committed);
      BPFAB_CHECK(ticket.state == TicketState::Cancelled);
      BPFAB_CHECK(ticket.status.code() == ErrorCode::Cancelled);
    }
    if (ticket.committed) {
      ++committed;
      BPFAB_CHECK(ticket.state == TicketState::Completed);
      BPFAB_CHECK(ticket.status.ok());
    }
    BPFAB_CHECK(!(ticket.cancelled && ticket.committed));
  }
  // Every committed ticket corresponded to exactly one accepted signal, and no
  // cancelled ticket contributed to the accepted count.
  BPFAB_CHECK(harness.fabric->status().signals_accepted == committed);
  BPFAB_CHECK(committed <= tickets.size());
}

BPFAB_TEST(concurrency, shutdown_stops_work_and_returns_accounting_to_baseline) {
  Harness harness;
  BPFAB_REQUIRE_OK(harness.configure(2, 16));
  const Incarnation incarnation = bpfab_test::test_incarnation(5u, 904u);
  for (std::uint32_t s = 1; s <= 16u; ++s) {
    BPFAB_REQUIRE_OK(harness.fabric->register_publisher(SourceId(s), incarnation, 0));
  }
  const auto topology = harness.fabric->topology();
  const Epoch epoch = harness.fabric->epoch();
  const PropagationPolicy policy = bpfab_test::test_policy();

  std::vector<TicketId> tickets;
  for (std::uint32_t s = 1; s <= 16u; ++s) {
    PressureSignal signal = bpfab_test::make_signal(s, s, 1u, topology->digest(), epoch, policy,
                                                    80000u, 100u, AuthorityLevel::Global, 8u,
                                                    incarnation);
    Result<TicketId> ticket = harness.fabric->publish_async(signal, 100u);
    BPFAB_REQUIRE(ticket.ok());
    tickets.push_back(ticket.value());
  }

  BPFAB_REQUIRE_OK(harness.fabric->shutdown(200));
  BPFAB_CHECK(harness.fabric->shutting_down());
  BPFAB_REQUIRE_CODE(harness.fabric->publish_async(bpfab_test::make_signal(
                         999u, 1u, 1u, topology->digest(), epoch, policy, 80000u, 100u,
                         AuthorityLevel::Global, 8u, incarnation),
                     200),
                     ErrorCode::ShuttingDown);

  for (const TicketId id : tickets) {
    Result<PublishTicket> ticket = harness.fabric->ticket(id);
    if (ticket.ok() && ticket.value().cancelled) {
      BPFAB_CHECK(!ticket.value().committed);
    }
  }
  const FabricStatus status = harness.fabric->status();
  BPFAB_CHECK(status.shutting_down);
  BPFAB_CHECK(status.live_publishers == 0u);
  BPFAB_CHECK(status.publishers == 16u);
  // Shutdown is idempotent and never deadlocks.
  BPFAB_REQUIRE_OK(harness.fabric->shutdown(201));
  BPFAB_REQUIRE_OK(harness.fabric->shutdown(202));
}

BPFAB_TEST(concurrency, fabric_shutdown_without_workers_is_safe) {
  Harness harness;
  BPFAB_REQUIRE_OK(harness.configure(0, 0));
  BPFAB_REQUIRE_OK(harness.fabric->shutdown(1));
  BPFAB_CHECK(harness.fabric->status().shutting_down);
  BPFAB_REQUIRE_OK(harness.fabric->drain());
}