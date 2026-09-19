// Backpressure Fabric - framed transport tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"

using namespace backpressure;

namespace {

Frame text_frame(MessageType type, const std::string& text) {
  Frame frame;
  frame.type = type;
  frame.payload.assign(reinterpret_cast<const std::byte*>(text.data()),
                       reinterpret_cast<const std::byte*>(text.data()) + text.size());
  return frame;
}

std::string payload_text(const Frame& frame) {
  return std::string(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
}

}  // namespace

BPFAB_TEST(ipc, frames_round_trip) {
  Frame frame = text_frame(MessageType::Publish, "hello fabric");
  frame.flags = 0xABCDEF01u;
  std::vector<std::byte> bytes;
  BPFAB_REQUIRE_OK(encode_frame(frame, bytes));
  BPFAB_CHECK(bytes.size() == kFrameHeaderBytes + 12u);

  BPFAB_REQUIRE_RESULT(decoded, decode_frame(bytes));
  BPFAB_CHECK(decoded.type == MessageType::Publish);
  BPFAB_CHECK(decoded.flags == 0xABCDEF01u);
  BPFAB_CHECK(payload_text(decoded) == "hello fabric");

  Frame empty;
  empty.type = MessageType::Heartbeat;
  std::vector<std::byte> empty_bytes;
  BPFAB_REQUIRE_OK(encode_frame(empty, empty_bytes));
  BPFAB_REQUIRE_RESULT(decoded_empty, decode_frame(empty_bytes));
  BPFAB_CHECK(decoded_empty.payload.empty());
  BPFAB_CHECK(decoded_empty.type == MessageType::Heartbeat);

  BPFAB_CHECK(std::string(to_string(MessageType::PublishAck)) == "PublishAck");
}

BPFAB_TEST(ipc, decoders_refuse_malformed_frames) {
  Frame frame = text_frame(MessageType::Result, "payload");
  std::vector<std::byte> bytes;
  BPFAB_REQUIRE_OK(encode_frame(frame, bytes));

  std::vector<std::byte> bad_magic = bytes;
  bad_magic[0] = std::byte{0};
  BPFAB_CHECK(decode_frame(bad_magic).status().code() == ErrorCode::MalformedInput);

  std::vector<std::byte> bad_version = bytes;
  bad_version[4] = std::byte{9};
  BPFAB_CHECK(decode_frame(bad_version).status().code() == ErrorCode::VersionMismatch);

  std::vector<std::byte> bad_type = bytes;
  bad_type[6] = std::byte{0};
  BPFAB_CHECK(decode_frame(bad_type).status().code() == ErrorCode::MalformedInput);

  std::vector<std::byte> bad_crc = bytes;
  bad_crc[kFrameHeaderBytes] = std::byte{0x5A};
  BPFAB_CHECK(decode_frame(bad_crc).status().code() == ErrorCode::IntegrityMismatch);

  std::vector<std::byte> trailing = bytes;
  trailing.push_back(std::byte{1});
  BPFAB_CHECK(decode_frame(trailing).status().code() == ErrorCode::MalformedInput);

  std::vector<std::byte> truncated(bytes.begin(), bytes.end() - 2);
  BPFAB_CHECK(decode_frame(truncated).status().code() == ErrorCode::MalformedInput ||
              decode_frame(truncated).status().code() == ErrorCode::TruncatedInput);

  std::vector<std::byte> short_header(kFrameHeaderBytes - 1u, std::byte{0});
  MessageType type = MessageType::Hello;
  std::uint32_t flags = 0;
  BPFAB_CHECK(decode_frame_header(short_header, type, flags).status().code() ==
              ErrorCode::TruncatedInput);

  std::vector<std::byte> oversized = bytes;
  const std::uint32_t huge = 0x00FFFFFFu;
  for (unsigned i = 0; i < 4u; ++i) {
    oversized[12u + i] =
        std::byte{static_cast<unsigned char>((huge >> (8u * i)) & 0xFFu)};
  }
  BPFAB_CHECK(decode_frame(oversized).status().code() == ErrorCode::OversizedInput);

  Frame too_big = text_frame(MessageType::Publish, std::string(64, 'x'));
  std::vector<std::byte> out;
  BPFAB_CHECK(encode_frame(too_big, out, 8u).code() == ErrorCode::OversizedInput);

  Frame invalid_type;
  invalid_type.type = MessageType::Count;
  BPFAB_CHECK(encode_frame(invalid_type, out).code() == ErrorCode::InvalidArgument);
}

BPFAB_TEST(ipc, loopback_transport_delivers_frames) {
  BPFAB_REQUIRE_RESULT(listener, Listener::bind_loopback(0));
  BPFAB_CHECK(listener.port() != 0u);
  const std::uint16_t port = listener.port();

  Status client_status = Status::error(ErrorCode::Internal, "client never ran");
  std::thread client([port, &client_status]() {
    Result<Connection> connected = Connection::connect_loopback(port);
    if (!connected.ok()) {
      client_status = connected.status();
      return;
    }
    Connection connection = std::move(connected).value();
    client_status = connection.send(text_frame(MessageType::Publish, "from client"));
    if (!client_status.ok()) {
      return;
    }
    Result<Frame> reply = connection.receive();
    if (!reply.ok()) {
      client_status = reply.status();
      return;
    }
    if (payload_text(reply.value()) != "from server") {
      client_status = Status::error(ErrorCode::IntegrityMismatch, "client reply payload");
      return;
    }
    client_status = Status::success();
    (void)connection.close();
  });

  BPFAB_REQUIRE_RESULT(server, listener.accept_one());
  BPFAB_REQUIRE_RESULT(received, server.receive());
  BPFAB_CHECK(received.type == MessageType::Publish);
  BPFAB_CHECK(payload_text(received) == "from client");
  BPFAB_REQUIRE_RESULT(peer, server.peer_address());
  BPFAB_CHECK(peer == "127.0.0.1");
  BPFAB_REQUIRE_OK(server.send(text_frame(MessageType::Result, "from server")));
  client.join();
  BPFAB_REQUIRE_OK(client_status);
  BPFAB_REQUIRE_OK(server.close());
  BPFAB_REQUIRE_OK(listener.close());
  BPFAB_CHECK(!server.valid());

  // A closed connection refuses further work instead of blocking.
  BPFAB_CHECK(server.send(text_frame(MessageType::Hello, "x")).code() == ErrorCode::NotReady);
  BPFAB_CHECK(server.receive().status().code() == ErrorCode::NotReady);
}

BPFAB_TEST(ipc, accept_window_elapses_without_a_peer) {
  BPFAB_REQUIRE_RESULT(listener, Listener::bind_loopback(0));
  const Result<Connection> none = listener.accept_within(50u);
  BPFAB_REQUIRE_CODE(none.status(), ErrorCode::NotReady);
  BPFAB_REQUIRE_OK(listener.close());
}

BPFAB_TEST(ipc, process_utilities_report_real_paths) {
  BPFAB_REQUIRE_RESULT(executable, current_executable_path());
  BPFAB_CHECK(!executable.empty());
  BPFAB_REQUIRE_RESULT(directory, current_executable_directory());
  BPFAB_CHECK(!directory.empty());
  BPFAB_CHECK(executable.find(directory) == 0u);
  BPFAB_REQUIRE_RESULT(cwd, current_working_directory());
  BPFAB_CHECK(!cwd.empty());

  SpawnOptions quiet;
  quiet.inherit_stdio = false;
  const std::vector<std::string> argv = {executable, "--filter=no-such-test"};

  BPFAB_REQUIRE_RESULT(handle, spawn_process(argv, quiet));
  BPFAB_REQUIRE_RESULT(running, process_running(handle));
  BPFAB_CHECK(running || !running);
  BPFAB_REQUIRE_RESULT(exit_code, wait_process(handle));
  BPFAB_CHECK(exit_code == 0);
  BPFAB_CHECK(!handle.valid());

  BPFAB_REQUIRE_CODE(spawn_process({}, SpawnOptions{}).status(), ErrorCode::InvalidArgument);
  ProcessHandle invalid;
  BPFAB_REQUIRE_CODE(process_running(invalid).status(), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_CODE(wait_process(invalid).status(), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_CODE(terminate_process(invalid), ErrorCode::InvalidArgument);
}