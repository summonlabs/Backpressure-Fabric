// Backpressure Fabric - real multiprocess validation.
//
// These tests launch real child OS processes of this same binary and exchange
// real framed messages over loopback TCP. Nothing here simulates a peer.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"
#include "support.hpp"

using namespace backpressure;
using bpfab_test::find_node;
using bpfab_test::make_edge;
using bpfab_test::make_resource;

namespace {

constexpr std::uint8_t kModeNormal = 0;
constexpr std::uint8_t kModeStaleEpoch = 1;
constexpr std::uint8_t kModeHoldOpen = 2;

struct WireHandshake {
  std::uint32_t pid = 0;
  BootId boot{};
};

std::vector<std::byte> encode_handshake(const WireHandshake& handshake) {
  ByteWriter writer(64);
  (void)writer.u32(handshake.pid);
  (void)writer.u64(handshake.boot.hi);
  (void)writer.u64(handshake.boot.lo);
  return writer.take();
}

Result<WireHandshake> decode_handshake(const Frame& frame) {
  ByteReader reader(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  WireHandshake handshake;
  BPFAB_TRY_ASSIGN(handshake.pid, reader.u32());
  BPFAB_TRY_ASSIGN(handshake.boot.hi, reader.u64());
  BPFAB_TRY_ASSIGN(handshake.boot.lo, reader.u64());
  BPFAB_TRY(reader.expect_end());
  return Result<WireHandshake>(handshake);
}

std::vector<std::byte> encode_ack(Status status, std::uint64_t accepted, Digest128 fingerprint) {
  ByteWriter writer(64);
  (void)writer.u16(static_cast<std::uint16_t>(status.code()));
  (void)writer.u64(accepted);
  (void)writer.digest(fingerprint);
  return writer.take();
}

struct WireAck {
  ErrorCode code = ErrorCode::Internal;
  std::uint64_t accepted = 0;
  Digest128 fingerprint{};
};

Result<WireAck> decode_ack(const Frame& frame) {
  ByteReader reader(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  WireAck ack;
  BPFAB_TRY_DECL(const std::uint16_t, raw_code, reader.u16());
  if (raw_code > static_cast<std::uint16_t>(ErrorCode::Internal)) {
    return fail<WireAck>(ErrorCode::MalformedInput, "ack code", raw_code);
  }
  ack.code = static_cast<ErrorCode>(raw_code);
  BPFAB_TRY_ASSIGN(ack.accepted, reader.u64());
  BPFAB_TRY_ASSIGN(ack.fingerprint, reader.digest());
  BPFAB_TRY(reader.expect_end());
  return Result<WireAck>(ack);
}

struct HelloAckPayload {
  Epoch epoch{};
  Generation topology_generation{};
  Digest128 topology_digest{};
  ResourceId origin{};
  Generation policy_generation{};
  std::uint8_t mode = kModeNormal;
};

std::vector<std::byte> encode_hello_ack(const HelloAckPayload& payload) {
  ByteWriter writer(128);
  (void)writer.u64(payload.epoch.value());
  (void)writer.u64(payload.topology_generation.value());
  (void)writer.digest(payload.topology_digest);
  (void)writer.u64(payload.origin.value());
  (void)writer.u64(payload.policy_generation.value());
  (void)writer.u8(payload.mode);
  return writer.take();
}

Result<HelloAckPayload> decode_hello_ack(const Frame& frame) {
  ByteReader reader(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  HelloAckPayload payload;
  BPFAB_TRY_DECL(const std::uint64_t, epoch, reader.u64());
  payload.epoch = Epoch::from_raw(epoch);
  BPFAB_TRY_DECL(const std::uint64_t, generation, reader.u64());
  payload.topology_generation = Generation::from_raw(generation);
  BPFAB_TRY_ASSIGN(payload.topology_digest, reader.digest());
  BPFAB_TRY_DECL(const std::uint64_t, origin, reader.u64());
  BPFAB_TRY_ASSIGN(payload.origin, ResourceId::from_u64(origin));
  BPFAB_TRY_DECL(const std::uint64_t, policy_generation, reader.u64());
  payload.policy_generation = Generation::from_raw(policy_generation);
  BPFAB_TRY_ASSIGN(payload.mode, reader.u8());
  BPFAB_TRY(reader.expect_end());
  return Result<HelloAckPayload>(payload);
}

std::vector<std::byte> encode_signal(const PressureSignal& signal) {
  ByteWriter writer(512);
  (void)writer.u64(signal.id.value());
  (void)writer.u64(signal.source.value());
  (void)writer.u64(signal.origin.value());
  (void)writer.u8(static_cast<std::uint8_t>(signal.observation.severity()));
  (void)writer.u32(signal.observation.magnitude().raw());
  (void)writer.u64(signal.origin_generation.value());
  (void)writer.digest(signal.topology_digest);
  (void)writer.u64(signal.epoch.value());
  (void)writer.u64(signal.publisher.boot.hi);
  (void)writer.u64(signal.publisher.boot.lo);
  (void)writer.u32(signal.publisher.pid);
  (void)writer.u32(signal.publisher.index);
  (void)writer.u64(signal.publisher.seq);
  (void)writer.u64(signal.issued_at);
  (void)writer.u64(signal.valid_until);
  (void)writer.u8(static_cast<std::uint8_t>(signal.authority.level));
  (void)writer.u64(signal.authority.epoch.value());
  (void)writer.u64(signal.authority.policy_generation.value());
  (void)writer.u32(signal.authority.max_hops_granted);
  (void)writer.u64(signal.provenance.count);
  return writer.take();
}

Result<PressureSignal> decode_signal(const Frame& frame) {
  ByteReader reader(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  PressureSignal signal;
  BPFAB_TRY_DECL(const std::uint64_t, id, reader.u64());
  BPFAB_TRY_ASSIGN(signal.id, SignalId::from_u64(id));
  BPFAB_TRY_DECL(const std::uint64_t, source, reader.u64());
  BPFAB_TRY_ASSIGN(signal.source, SourceId::from_u64(source));
  BPFAB_TRY_DECL(const std::uint64_t, origin, reader.u64());
  BPFAB_TRY_ASSIGN(signal.origin, ResourceId::from_u64(origin));
  BPFAB_TRY_DECL(const std::uint8_t, severity, reader.u8());
  if (severity > static_cast<std::uint8_t>(Severity::Exhausted)) {
    return fail<PressureSignal>(ErrorCode::MalformedInput, "severity", severity);
  }
  BPFAB_TRY_DECL(const std::uint32_t, magnitude, reader.u32());
  signal.observation = PressureObservation::observed_with_severity(
      Magnitude::from_raw_q16_saturating(magnitude), static_cast<Severity>(severity),
      SeverityThresholds::defaults());
  BPFAB_TRY_DECL(const std::uint64_t, origin_generation, reader.u64());
  signal.origin_generation = Generation::from_raw(origin_generation);
  BPFAB_TRY_ASSIGN(signal.topology_digest, reader.digest());
  BPFAB_TRY_DECL(const std::uint64_t, epoch, reader.u64());
  signal.epoch = Epoch::from_raw(epoch);
  BPFAB_TRY_ASSIGN(signal.publisher.boot.hi, reader.u64());
  BPFAB_TRY_ASSIGN(signal.publisher.boot.lo, reader.u64());
  BPFAB_TRY_ASSIGN(signal.publisher.pid, reader.u32());
  BPFAB_TRY_ASSIGN(signal.publisher.index, reader.u32());
  BPFAB_TRY_ASSIGN(signal.publisher.seq, reader.u64());
  BPFAB_TRY_ASSIGN(signal.issued_at, reader.u64());
  BPFAB_TRY_ASSIGN(signal.valid_until, reader.u64());
  BPFAB_TRY_DECL(const std::uint8_t, level, reader.u8());
  if (level > static_cast<std::uint8_t>(AuthorityLevel::Global)) {
    return fail<PressureSignal>(ErrorCode::MalformedInput, "authority level", level);
  }
  signal.authority.level = static_cast<AuthorityLevel>(level);
  BPFAB_TRY_DECL(const std::uint64_t, authority_epoch, reader.u64());
  signal.authority.epoch = Epoch::from_raw(authority_epoch);
  BPFAB_TRY_DECL(const std::uint64_t, policy_generation, reader.u64());
  signal.authority.policy_generation = Generation::from_raw(policy_generation);
  BPFAB_TRY_ASSIGN(signal.authority.max_hops_granted, reader.u32());
  BPFAB_TRY_DECL(const std::uint64_t, provenance_count, reader.u64());
  if (provenance_count == 0 || provenance_count > Provenance::kMaxChain) {
    return fail<PressureSignal>(ErrorCode::MalformedInput, "provenance count", provenance_count);
  }
  ProvenanceHop hop;
  hop.publisher = signal.publisher;
  hop.epoch = signal.epoch;
  hop.generation = signal.origin_generation;
  hop.tick = signal.issued_at;
  signal.provenance.push(hop);
  BPFAB_TRY(reader.expect_end());
  signal.bind_lineage();
  return Result<PressureSignal>(signal);
}

struct ChildSession {
  ProcessHandle handle{};
  Listener listener{};
  Connection connection{};
};

Result<Connection> accept_from_child(Listener& listener, ProcessHandle& child, const char* role) {
  for (;;) {
    Result<Connection> accepted = listener.accept_within(2000u);
    if (accepted.ok()) {
      return accepted;
    }
    if (accepted.status().code() != ErrorCode::NotReady) {
      return accepted;
    }
    Result<bool> running = process_running(child);
    if (!running.ok()) {
      return fail<Connection>(running.status().code(), role);
    }
    if (!running.value()) {
      return fail<Connection>(ErrorCode::NotFound, "child exited before connecting");
    }
  }
}

Result<ChildSession> launch_child(const std::string& role) {
  BPFAB_TRY_DECL(const std::string, executable, current_executable_path());
  BPFAB_TRY_DECL(Listener, listener, Listener::bind_loopback(0));
  const std::uint16_t port = listener.port();

  ChildSession session;
  session.listener = std::move(listener);

  std::vector<std::string> argv = {executable, "--role=" + role, "--port=" + std::to_string(port)};
  SpawnOptions options;
  options.inherit_stdio = true;
  BPFAB_TRY_ASSIGN(session.handle, spawn_process(argv, options));
  BPFAB_TRY_ASSIGN(session.connection,
                   accept_from_child(session.listener, session.handle, role.c_str()));
  return Result<ChildSession>(std::move(session));
}

void finish_child(ChildSession& session, int expected_exit) {
  const Result<int> code = wait_process(session.handle);
  BPFAB_REQUIRE(code.ok());
  BPFAB_CHECK_MSG(code.value() == expected_exit,
                  "child exit code " + std::to_string(code.value()));
}

[[noreturn]] void child_fail(const char* message) {
  std::fprintf(stderr, "child: %s\n", message);
  std::fflush(stderr);
  std::exit(2);
}

int run_publisher_child(const std::string& role, std::uint16_t port, std::uint8_t mode) {
  (void)role;
  Result<Connection> connected = Connection::connect_loopback(port);
  if (!connected.ok()) {
    child_fail("connect");
  }
  Connection connection = std::move(connected).value();

  WireHandshake handshake;
  handshake.pid = 1234;
  handshake.boot = BootId::from_seed(0xC0FFEEull, 1234u, 99u);
  Frame hello;
  hello.type = MessageType::Hello;
  hello.payload = encode_handshake(handshake);
  if (!connection.send(hello).ok()) {
    child_fail("send hello");
  }

  Result<Frame> reply = connection.receive();
  if (!reply.ok()) {
    child_fail("receive hello ack");
  }
  if (reply.value().type != MessageType::HelloAck) {
    child_fail("unexpected reply type");
  }
  Result<HelloAckPayload> payload = decode_hello_ack(reply.value());
  if (!payload.ok()) {
    child_fail("decode hello ack");
  }

  PressureSignal signal;
  signal.id = SignalId(1u);
  signal.source = SourceId(1u);
  signal.origin = payload.value().origin;
  signal.observation = PressureObservation::observed(bpfab_test::mag(90000u),
                                                     SeverityThresholds::defaults());
  signal.origin_generation = Generation::initial();
  signal.topology_digest = payload.value().topology_digest;
  signal.epoch = payload.value().epoch;
  if (mode == kModeStaleEpoch) {
    signal.epoch = Epoch::from_raw(payload.value().epoch.value() + 1000u);
  }
  signal.publisher = Incarnation::mint(handshake.boot, handshake.pid, 0);
  signal.issued_at = 100;
  signal.valid_until = 0;
  signal.authority = AuthorityVector::make_global(signal.epoch, Generation::initial(), 8u);
  ProvenanceHop hop;
  hop.publisher = signal.publisher;
  hop.epoch = payload.value().epoch;
  hop.generation = Generation::initial();
  hop.tick = 100;
  signal.provenance.push(hop);
  signal.bind_lineage();

  Frame publish;
  publish.type = MessageType::Publish;
  publish.payload = encode_signal(signal);
  if (!connection.send(publish).ok()) {
    child_fail("send publish");
  }

  Result<Frame> ack_frame = connection.receive();
  if (!ack_frame.ok()) {
    child_fail("receive ack");
  }
  Result<WireAck> ack = decode_ack(ack_frame.value());
  if (!ack.ok()) {
    child_fail("decode ack");
  }

  const ErrorCode expected = mode == kModeStaleEpoch ? ErrorCode::StaleEpoch : ErrorCode::Ok;
  if (ack.value().code != expected) {
    child_fail("unexpected ack code");
  }

  if (mode == kModeHoldOpen) {
    // Stay alive so the parent can hard-kill this process.
    for (;;) {
      std::this_thread::sleep_for(std::chrono::hours(1));
    }
  }
  (void)connection.close();
  return 0;
}

int run_corrupt_child(std::uint16_t port) {
  Result<Connection> connected = Connection::connect_loopback(port);
  if (!connected.ok()) {
    child_fail("connect");
  }
  Connection connection = std::move(connected).value();
  Frame frame;
  frame.type = MessageType::Publish;
  frame.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  std::vector<std::byte> bytes;
  if (!encode_frame(frame, bytes).ok()) {
    child_fail("encode");
  }
  // Corrupt one payload byte after the CRC was computed, then put the raw bytes
  // on the wire without re-encoding.
  bytes[kFrameHeaderBytes + 1u] = std::byte{static_cast<unsigned char>(
      std::to_integer<std::uint8_t>(bytes[kFrameHeaderBytes + 1u]) ^ 0x7Fu)};
  if (!connection.send_raw(std::span<const std::byte>(bytes.data(), bytes.size())).ok()) {
    child_fail("send raw");
  }
  (void)connection.close();
  return 0;
}

}  // namespace

/// Entry point used when this binary is re-executed as a child process.
int bpfab_multiprocess_child_main(int argc, char** argv) {
  std::string role;
  std::string state_directory;
  std::uint16_t port = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--role=", 0) == 0) {
      role = arg.substr(7);
    } else if (arg.rfind("--port=", 0) == 0) {
      const unsigned long parsed = std::strtoul(arg.substr(7).c_str(), nullptr, 10);
      port = static_cast<std::uint16_t>(parsed);
    } else if (arg.rfind("--dir=", 0) == 0) {
      state_directory = arg.substr(6);
    }
  }
  if (role == "store-lock") {
    if (state_directory.empty()) {
      child_fail("missing --dir");
    }
    DurableConfig config;
    config.directory = state_directory;
    const Result<DurableStore> second = DurableStore::open(config);
    if (second.ok()) {
      child_fail("a second writer was admitted");
    }
    // 0 means the second writer was correctly refused.
    return second.status().code() == ErrorCode::Conflict ? 0 : 3;
  }
  if (port == 0) {
    child_fail("missing --port");
  }
  if (role == "publisher") {
    return run_publisher_child(role, port, kModeNormal);
  }
  if (role == "stale-epoch") {
    return run_publisher_child(role, port, kModeStaleEpoch);
  }
  if (role == "hold-open") {
    return run_publisher_child(role, port, kModeHoldOpen);
  }
  if (role == "corrupt") {
    return run_corrupt_child(port);
  }
  child_fail("unknown --role");
}

namespace {

struct ParentFabric {
  std::string directory = bpfab_test::make_scratch_directory("multiprocess");
  std::unique_ptr<Fabric> fabric{};
  std::shared_ptr<const Topology> topology{};
  PropagationPolicy policy = bpfab_test::test_policy();

  Status configure() {
    FabricConfig config;
    config.state_directory = directory;
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
    status = fabric->install_policy(policy, 0);
    if (!status.ok()) {
      return status;
    }
    topology = fabric->topology();
    return Status::success();
  }
};

HelloAckPayload make_hello_ack(const ParentFabric& parent, std::uint8_t mode) {
  HelloAckPayload payload;
  payload.epoch = parent.fabric->epoch();
  payload.topology_generation = parent.topology->generation();
  payload.topology_digest = parent.topology->digest();
  payload.origin = ResourceId(1u);
  payload.policy_generation = parent.policy.generation;
  payload.mode = mode;
  return payload;
}

}  // namespace

BPFAB_TEST(multiprocess, real_child_publishes_over_framed_transport) {
  ParentFabric parent;
  BPFAB_REQUIRE_OK(parent.configure());

  BPFAB_REQUIRE_RESULT(session, launch_child("publisher"));
  BPFAB_REQUIRE_RESULT(hello, session.connection.receive());
  BPFAB_CHECK(hello.type == MessageType::Hello);
  BPFAB_REQUIRE_RESULT(handshake, decode_handshake(hello));
  BPFAB_CHECK(handshake.pid == 1234u);
  BPFAB_CHECK(handshake.boot.valid());

  Frame hello_ack;
  hello_ack.type = MessageType::HelloAck;
  hello_ack.payload = encode_hello_ack(make_hello_ack(parent, kModeNormal));
  BPFAB_REQUIRE_OK(session.connection.send(hello_ack));

  // The child replies with a Publish frame built from real wire data.
  BPFAB_REQUIRE_RESULT(publish_frame, session.connection.receive());
  BPFAB_CHECK(publish_frame.type == MessageType::Publish);
  BPFAB_REQUIRE_RESULT(signal, decode_signal(publish_frame));
  BPFAB_CHECK(signal.topology_digest == parent.topology->digest());
  BPFAB_CHECK(signal.epoch == parent.fabric->epoch());
  BPFAB_CHECK(signal.publisher.pid == 1234u);

  BPFAB_REQUIRE_OK(parent.fabric->register_publisher(signal.source, signal.publisher, 100));
  BPFAB_REQUIRE_RESULT(outcome, parent.fabric->publish(signal, 100));
  BPFAB_REQUIRE_OK(outcome.status);
  BPFAB_CHECK(find_node(outcome, 2u) != nullptr);

  Frame ack;
  ack.type = MessageType::PublishAck;
  ack.payload = encode_ack(outcome.status, outcome.counters.signals_accepted, outcome.fingerprint);
  BPFAB_REQUIRE_OK(session.connection.send(ack));

  finish_child(session, 0);
  const FabricStatus status = parent.fabric->status();
  BPFAB_CHECK(status.signals_accepted >= 1u);
  BPFAB_CHECK(status.live_publishers == 1u);
}

BPFAB_TEST(multiprocess, stale_epoch_from_a_real_process_is_refused) {
  ParentFabric parent;
  BPFAB_REQUIRE_OK(parent.configure());
  BPFAB_REQUIRE_RESULT(session, launch_child("stale-epoch"));
  BPFAB_REQUIRE_RESULT(hello, session.connection.receive());
  BPFAB_CHECK(hello.type == MessageType::Hello);

  Frame hello_ack;
  hello_ack.type = MessageType::HelloAck;
  hello_ack.payload = encode_hello_ack(make_hello_ack(parent, kModeStaleEpoch));
  BPFAB_REQUIRE_OK(session.connection.send(hello_ack));

  BPFAB_REQUIRE_RESULT(publish_frame, session.connection.receive());
  BPFAB_REQUIRE_RESULT(signal, decode_signal(publish_frame));
  BPFAB_CHECK(signal.epoch != parent.fabric->epoch());

  BPFAB_REQUIRE_OK(parent.fabric->register_publisher(signal.source, signal.publisher, 100));
  const Result<PropagationOutcome> outcome = parent.fabric->publish(signal, 100);
  BPFAB_REQUIRE_CODE(outcome.status(), ErrorCode::StaleEpoch);

  Frame ack;
  ack.type = MessageType::PublishAck;
  ack.payload = encode_ack(outcome.status(), 0, Digest128{});
  BPFAB_REQUIRE_OK(session.connection.send(ack));
  finish_child(session, 0);
}

BPFAB_TEST(multiprocess, corrupted_frame_from_a_real_process_is_detected) {
  ParentFabric parent;
  BPFAB_REQUIRE_OK(parent.configure());
  BPFAB_REQUIRE_RESULT(session, launch_child("corrupt"));

  // The child sends a valid outer frame whose payload was tampered with after
  // encoding, so the CRC no longer matches.
  const Result<Frame> received = session.connection.receive();
  BPFAB_REQUIRE_CODE(received.status(), ErrorCode::IntegrityMismatch);
  BPFAB_REQUIRE_OK(session.connection.close());
  finish_child(session, 0);
  (void)parent;
}

BPFAB_TEST(multiprocess, hard_kill_then_restart_fences_the_old_incarnation) {
  std::string directory = bpfab_test::make_scratch_directory("hardkill");
  Epoch first_epoch;
  Incarnation killed_incarnation;
  SourceId source(1u);

  {
    FabricConfig config;
    config.state_directory = directory;
    BPFAB_REQUIRE_RESULT(fabric, Fabric::open(config));
    BPFAB_REQUIRE_OK(fabric->install_topology({make_resource(1u), make_resource(2u)},
                                              {make_edge(1u, 1u, 2u, 90000u)}, 0));
    PropagationPolicy policy = bpfab_test::test_policy();
    BPFAB_REQUIRE_OK(fabric->install_policy(policy, 0));

    BPFAB_REQUIRE_RESULT(session, launch_child("hold-open"));
    BPFAB_REQUIRE_RESULT(hello, session.connection.receive());
    BPFAB_CHECK(hello.type == MessageType::Hello);
    Frame hello_ack;
    hello_ack.type = MessageType::HelloAck;
    hello_ack.payload = encode_hello_ack([&]() {
      HelloAckPayload payload;
      payload.epoch = fabric->epoch();
      payload.topology_generation = fabric->topology()->generation();
      payload.topology_digest = fabric->topology()->digest();
      payload.origin = ResourceId(1u);
      payload.policy_generation = policy.generation;
      return payload;
    }());
    BPFAB_REQUIRE_OK(session.connection.send(hello_ack));

    BPFAB_REQUIRE_RESULT(publish_frame, session.connection.receive());
    BPFAB_REQUIRE_RESULT(signal, decode_signal(publish_frame));
    killed_incarnation = signal.publisher;
    BPFAB_REQUIRE_OK(fabric->register_publisher(source, killed_incarnation, 100));
    BPFAB_REQUIRE_RESULT(outcome, fabric->publish(signal, 100));
    BPFAB_REQUIRE_OK(outcome.status);

    Frame ack;
    ack.type = MessageType::PublishAck;
    ack.payload = encode_ack(outcome.status, outcome.counters.signals_accepted,
                             outcome.fingerprint);
    BPFAB_REQUIRE_OK(session.connection.send(ack));

    BPFAB_REQUIRE_RESULT(running, process_running(session.handle));
    BPFAB_CHECK(running);
    BPFAB_REQUIRE_OK(terminate_process(session.handle));
    BPFAB_CHECK(!session.handle.valid());
    (void)session.connection.close();
    (void)session.listener.close();

    first_epoch = fabric->epoch();
    BPFAB_REQUIRE_OK(fabric->flush(100));
    BPFAB_REQUIRE_OK(fabric->shutdown(100));
    BPFAB_CHECK(fabric->shutting_down());
    BPFAB_REQUIRE_CODE(fabric->publish(signal, 101).status(), ErrorCode::ShuttingDown);
    fabric.reset();
  }

  {
    FabricConfig config;
    config.state_directory = directory;
    BPFAB_REQUIRE_RESULT(fabric, Fabric::open(config));
    const FabricStatus status = fabric->status();
    BPFAB_CHECK(status.epoch.value() == first_epoch.value() + 1u);
    BPFAB_CHECK(status.previous_epoch == first_epoch);
    // No liveness, lease or publisher authority survives the restart.
    BPFAB_CHECK(!status.restored_liveness);
    BPFAB_CHECK(status.publishers == 0u);
    BPFAB_CHECK(status.live_publishers == 0u);
    BPFAB_CHECK(status.ambiguous_attempts == 0u);

    // The old epoch is fenced.
    BPFAB_REQUIRE_OK(fabric->install_topology({make_resource(1u), make_resource(2u)},
                                              {make_edge(1u, 1u, 2u, 90000u)}, 0));
    PropagationPolicy policy = bpfab_test::test_policy();
    BPFAB_REQUIRE_OK(fabric->install_policy(policy, 0));
    const auto topology = fabric->topology();
    PressureSignal stale =
        bpfab_test::make_signal(1u, 1u, 1u, topology->digest(), first_epoch, policy, 90000u, 0u,
                                AuthorityLevel::Global, 8u, killed_incarnation);
    // Nothing is known about the killed publisher after a restart.
    BPFAB_REQUIRE_CODE(fabric->publish(stale, 0).status(), ErrorCode::NotFound);

    // Even after the same incarnation is explicitly re-registered, the epoch it
    // was killed under remains fenced.
    BPFAB_REQUIRE_OK(fabric->register_publisher(source, killed_incarnation, 0));
    BPFAB_REQUIRE_CODE(fabric->publish(stale, 0).status(), ErrorCode::StaleEpoch);

    PressureSignal after_restart =
        bpfab_test::make_signal(2u, 1u, 1u, topology->digest(), fabric->epoch(), policy, 90000u, 0u,
                                AuthorityLevel::Global, 8u, killed_incarnation);
    BPFAB_REQUIRE_RESULT(outcome, fabric->publish(after_restart, 0));
    BPFAB_REQUIRE_OK(outcome.status);
    BPFAB_CHECK(find_node(outcome, 2u) != nullptr);

    // Resources that carried pressure before the restart require revalidation.
    BPFAB_CHECK(!fabric->revalidation_required().empty());
    (void)fabric->clear_revalidation(ResourceId(1u), 0);
    (void)fabric->clear_revalidation(ResourceId(2u), 0);
    BPFAB_CHECK(fabric->revalidation_required().empty());
    BPFAB_REQUIRE_OK(fabric->flush(0));
  }
}
BPFAB_TEST(multiprocess, durable_store_admits_one_writer_across_real_processes) {
  const std::string directory = bpfab_test::make_scratch_directory("storelock");
  FabricConfig config;
  config.state_directory = directory;

  {
    BPFAB_REQUIRE_RESULT(fabric, Fabric::open(config));
    BPFAB_CHECK(fabric->durable());

    BPFAB_REQUIRE_RESULT(executable, current_executable_path());
    SpawnOptions quiet;
    quiet.inherit_stdio = false;
    const std::vector<std::string> argv = {executable, "--role=store-lock", "--dir=" + directory};
    BPFAB_REQUIRE_RESULT(handle, spawn_process(argv, quiet));
    BPFAB_REQUIRE_RESULT(exit_code, wait_process(handle));
    // The child exits 0 only when the operating system refused it the lock.
    BPFAB_CHECK_MSG(exit_code == 0, "second writer exit code " + std::to_string(exit_code));

    // The writer that holds the lock keeps working normally.
    BPFAB_REQUIRE_OK(fabric->install_topology({make_resource(1u), make_resource(2u)},
                                              {make_edge(1u, 1u, 2u, 90000u)}, 0));
    BPFAB_CHECK(fabric->status().has_topology);
    BPFAB_REQUIRE_OK(fabric->shutdown(0));
    fabric.reset();
  }

  // Once released, the directory is usable again.
  BPFAB_REQUIRE_RESULT(reopened, Fabric::open(config));
  BPFAB_CHECK(reopened->status().epoch.value() > 1u);
  BPFAB_REQUIRE_OK(reopened->shutdown(0));
}
