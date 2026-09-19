// Backpressure Fabric - adversarial input tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
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

BPFAB_TEST(adversarial, frame_decoder_survives_random_bytes) {
  SplitMix64 rng(0xADBEADull);
  for (std::uint32_t iteration = 0; iteration < 4000u; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng.next_bounded(96u));
    std::vector<std::byte> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
      bytes[i] = std::byte{static_cast<unsigned char>(rng.next_bounded(256u))};
    }
    const Result<Frame> decoded = decode_frame(bytes);
    if (decoded.ok()) {
      BPFAB_CHECK(decoded.value().type != MessageType::Count);
      BPFAB_CHECK(decoded.value().payload.size() + kFrameHeaderBytes <= bytes.size());
    } else {
      BPFAB_CHECK(decoded.status().code() != ErrorCode::Ok);
    }
    if (bytes.size() >= kFrameHeaderBytes) {
      MessageType type = MessageType::Hello;
      std::uint32_t flags = 0;
      const Result<std::uint32_t> header =
          decode_frame_header(std::span<const std::byte>(bytes.data(), kFrameHeaderBytes), type,
                              flags);
      if (header.ok()) {
        BPFAB_CHECK(header.value() <= kMaxFramePayload);
        BPFAB_CHECK(type != MessageType::Count);
      }
    }
  }
}

BPFAB_TEST(adversarial, journal_rejects_random_content) {
  const std::string directory = bpfab_test::make_scratch_directory("randomjournal");
  const std::string path = (std::filesystem::path(directory) / "random.bpfj").string();
  SplitMix64 rng(0x5EEDBEEFull);
  JournalLimits limits;
  for (std::uint32_t iteration = 0; iteration < 64u; ++iteration) {
    const std::size_t length = 8u + static_cast<std::size_t>(rng.next_bounded(200u));
    std::vector<char> bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
      bytes[i] = static_cast<char>(rng.next_bounded(256u));
    }
    {
      std::ofstream stream(path, std::ios::binary | std::ios::trunc);
      stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const Result<Journal> opened = Journal::open(path, limits, false);
    if (opened.ok()) {
      BPFAB_CHECK(opened.value().records().size() <= limits.max_records);
    } else {
      const ErrorCode code = opened.status().code();
      BPFAB_CHECK(code == ErrorCode::MalformedInput || code == ErrorCode::VersionMismatch ||
                  code == ErrorCode::IntegrityMismatch || code == ErrorCode::TruncatedInput ||
                  code == ErrorCode::IoError || code == ErrorCode::OversizedInput);
    }
  }
}

BPFAB_TEST(adversarial, truncated_durable_state_is_refused_or_repaired) {
  const std::string directory = bpfab_test::make_scratch_directory("truncatedstate");
  DurableConfig config;
  config.directory = directory;
  {
    BPFAB_REQUIRE_RESULT(store, DurableStore::open(config));
    BPFAB_REQUIRE_OK(store.establish_epoch(0));
    PropagationPolicy policy = bpfab_test::test_policy();
    BPFAB_REQUIRE_OK(store.persist_policy(policy, 0));
    for (std::uint32_t i = 0; i < 20u; ++i) {
      BPFAB_REQUIRE_OK(store.record_lineage(digest_of(std::to_string(i)), i));
    }
    BPFAB_REQUIRE_OK(store.sync());
  }
  const std::string path = (std::filesystem::path(directory) / "fabric.bpfj").string();
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(path), ec);
  BPFAB_REQUIRE(!ec);
  std::filesystem::resize_file(std::filesystem::path(path), size / 2u, ec);
  BPFAB_REQUIRE(!ec);

  {
    const Result<DurableStore> reopened = DurableStore::open(config);
    if (reopened.ok()) {
      BPFAB_CHECK(reopened.value().state().records_replayed <= 21u);
    }
  }
  // Regardless of the outcome, the directory must be usable again once the
  // previous owner released it.
  const Result<DurableStore> again = DurableStore::open(config);
  BPFAB_REQUIRE(again.ok());
}

BPFAB_TEST(adversarial, oversized_and_boundary_identities_are_handled) {
  const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max() - 1u;
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology({make_resource(1u), make_resource(huge)},
                                                        {make_edge(1u, 1u, huge, 90000u)}));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal = bpfab_test::make_signal(huge, huge, 1u, topo.digest(), Epoch(1), policy,
                                                  90000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  const NodeOutcome* target = find_node(outcome, huge);
  BPFAB_REQUIRE(target != nullptr);
  BPFAB_CHECK(target->received);

  BPFAB_REQUIRE_CODE(ResourceId::from_u64(huge + 1u).status(), ErrorCode::OutOfRange);
  BPFAB_REQUIRE_CODE(SignalId::from_u64(std::numeric_limits<std::uint64_t>::max()).status(),
                     ErrorCode::OutOfRange);
}

BPFAB_TEST(adversarial, huge_fanout_is_bounded_by_policy) {
  std::vector<Resource> resources;
  std::vector<DependencyEdge> edges;
  const std::uint32_t fanout = 2048u;
  resources.push_back(make_resource(1u));
  for (std::uint32_t i = 0; i < fanout; ++i) {
    resources.push_back(make_resource(2u + i));
    edges.push_back(make_edge(1u + i, 1u, 2u + i, 65536u));
  }
  TopologyLimits limits = bpfab_test::default_limits();
  limits.max_out_degree = 4096;
  BPFAB_REQUIRE_RESULT(topo, Topology::build(resources, edges, limits, true));

  PropagationPolicy policy = bpfab_test::test_policy();
  policy.max_fanout = 16;
  policy.max_expansions = 64;
  policy.max_visited = 64;
  policy.max_records = 2u * policy.max_expansions + policy.max_visited;
  policy.max_path_records = policy.max_records;
  BPFAB_REQUIRE_OK(policy.validate());

  PropagationLedger ledger(256, 256, 256);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                  90000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.counters.hops_propagated <= policy.max_expansions);
  BPFAB_CHECK(outcome.counters.nodes_settled <= policy.max_visited);
  BPFAB_CHECK(outcome.nodes.size() <= policy.max_visited);
  BPFAB_CHECK(outcome.complete);
}

BPFAB_TEST(adversarial, dependency_pins_are_bounded) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(2u, 90000u));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(64, 64, 64);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                  90000u, 100u);
  std::vector<DependencyPin> pins;
  for (std::uint32_t i = 0; i < 4097u; ++i) {
    DependencyPin pin;
    pin.edge = EdgeId(1u);
    pin.generation = Generation::initial();
    pins.push_back(pin);
  }
  const PropagationOutcome outcome = run_propagation(topo, policy, ledger, Epoch(1), 100, {signal},
                                                     nullptr, 1u, 0u, pins);
  BPFAB_REQUIRE_CODE(outcome.status, ErrorCode::LimitExceeded);
}

BPFAB_TEST(adversarial, contradictory_signals_are_refused) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(2u, 90000u));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(64, 64, 64);

  PressureSignal first =
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 60000u, 100u);
  PressureSignal second =
      bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy, 30000u, 100u);
  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {first, second});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.counters.signals_accepted == 1u);
  BPFAB_CHECK(outcome.counters.signals_rejected == 1u);
  BPFAB_REQUIRE(outcome.rejections.size() == 1u);
  BPFAB_CHECK(outcome.rejections[0].status.code() == ErrorCode::Conflict);
}

BPFAB_TEST(adversarial, engine_context_must_be_complete) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(2u, 90000u));
  PropagationPolicy policy = bpfab_test::test_policy();
  PropagationLedger ledger(64, 64, 64);
  EngineContext ctx;
  ctx.topology = nullptr;
  ctx.policy = &policy;
  ctx.ledger = &ledger;
  PropagationRequest request;
  request.id = PropagationId(1u);
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::InvalidArgument);
  ctx.topology = &topo;
  ctx.policy = nullptr;
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::InvalidArgument);
  ctx.policy = &policy;
  ctx.ledger = nullptr;
  BPFAB_REQUIRE_CODE(PropagationEngine::propagate(ctx, request).status, ErrorCode::InvalidArgument);
}

BPFAB_TEST(adversarial, deep_topology_stays_within_the_hop_budget) {
  std::vector<Resource> resources;
  std::vector<DependencyEdge> edges;
  const std::uint32_t depth = 64u;
  for (std::uint32_t i = 1; i <= depth; ++i) {
    resources.push_back(make_resource(i));
  }
  for (std::uint32_t i = 1; i < depth; ++i) {
    edges.push_back(make_edge(i, i, i + 1u, 65536u));
  }
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_topology(resources, edges, true));

  PropagationPolicy policy = bpfab_test::test_policy();
  policy.max_hops = 64;
  policy.max_expansions = 4096;
  policy.max_visited = 4096;
  policy.max_records = 2u * policy.max_expansions + policy.max_visited;
  policy.max_path_records = policy.max_records;
  BPFAB_REQUIRE_OK(policy.validate());
  PropagationLedger ledger(256, 256, 256);
  PressureSignal signal = bpfab_test::make_signal(1u, 1u, 1u, topo.digest(), Epoch(1), policy,
                                                  100000u, 100u, AuthorityLevel::Global, 64u);
  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, {signal});
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.peak_depth <= policy.max_hops);
  BPFAB_CHECK(outcome.counters.nodes_settled <= depth);
  for (const NodeOutcome& node : outcome.nodes) {
    BPFAB_CHECK(node.applied <= signal.observation.magnitude());
  }
}

BPFAB_TEST(adversarial, malformed_signals_never_reach_the_ledger) {
  BPFAB_REQUIRE_RESULT(topo, bpfab_test::build_chain(2u, 90000u));
  PropagationPolicy policy = bpfab_test::test_policy();
  policy.max_sources = 64;
  BPFAB_REQUIRE_OK(policy.validate());
  PropagationLedger ledger(64, 64, 64);
  std::vector<PressureSignal> signals;
  for (std::uint32_t i = 0; i < 64u; ++i) {
    PressureSignal signal = bpfab_test::make_signal(i + 1u, i + 1u, 1u, topo.digest(), Epoch(1),
                                                    policy, 90000u, 100u);
    signals.push_back(signal);
  }
  signals[3].topology_digest = Digest128{};
  signals[3].bind_lineage();
  signals[9].epoch = Epoch::none();
  signals[9].bind_lineage();
  signals[17].observation = PressureObservation::unknown();
  signals[17].bind_lineage();
  signals[25].authority.epoch = Epoch(77);
  signals[25].bind_lineage();
  signals[40].valid_until = 1;
  signals[40].bind_lineage();

  const PropagationOutcome outcome =
      run_propagation(topo, policy, ledger, Epoch(1), 100, signals);
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(outcome.counters.signals_rejected == 5u);
  BPFAB_CHECK(outcome.counters.signals_accepted == 59u);
  BPFAB_CHECK(ledger.lineage_count() == 59u);
  for (const RejectionRecord& rejection : outcome.rejections) {
    BPFAB_CHECK(rejection.signal.valid());
    BPFAB_CHECK(!rejection.status.ok());
  }
}