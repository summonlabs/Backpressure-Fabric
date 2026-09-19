#pragma once

// Backpressure Fabric - framed point-to-point transport.
//
// Distributed claims are tested over real OS processes and real framed
// transport: a loopback TCP stream carrying length-delimited, versioned,
// CRC-protected frames. Every decoder refuses truncated, oversized,
// wrong-version and integrity-failing input rather than guessing.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"

namespace backpressure {

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Publish = 3,
  PublishAck = 4,
  Result = 5,
  Heartbeat = 6,
  Retire = 7,
  Shutdown = 8,
  Error = 9,
  Count,
};

[[nodiscard]] constexpr const char* to_string(MessageType t) noexcept {
  switch (t) {
    case MessageType::Hello: return "Hello";
    case MessageType::HelloAck: return "HelloAck";
    case MessageType::Publish: return "Publish";
    case MessageType::PublishAck: return "PublishAck";
    case MessageType::Result: return "Result";
    case MessageType::Heartbeat: return "Heartbeat";
    case MessageType::Retire: return "Retire";
    case MessageType::Shutdown: return "Shutdown";
    case MessageType::Error: return "Error";
    case MessageType::Count: break;
  }
  return "Invalid";
}

inline constexpr std::uint32_t kFrameMagic = 0x46504242u;
inline constexpr std::uint16_t kFrameVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 20;
inline constexpr std::size_t kMaxFramePayload = 1u << 20;

struct Frame {
  MessageType type = MessageType::Hello;
  std::uint32_t flags = 0;
  std::vector<std::byte> payload{};
};

/// Encode a complete frame (header plus payload).
[[nodiscard]] Status encode_frame(const Frame& frame, std::vector<std::byte>& out,
                                  std::size_t max_payload = kMaxFramePayload);

/// Decode exactly kFrameHeaderBytes and report the declared payload length.
[[nodiscard]] Result<std::uint32_t> decode_frame_header(std::span<const std::byte> header,
                                                        MessageType& type, std::uint32_t& flags,
                                                        std::size_t max_payload = kMaxFramePayload);

/// Decode a complete frame; refuses trailing bytes.
[[nodiscard]] Result<Frame> decode_frame(std::span<const std::byte> bytes,
                                         std::size_t max_payload = kMaxFramePayload);

/// Sentinel for an unbound socket handle.
inline constexpr std::uintptr_t kInvalidSocketHandle = ~static_cast<std::uintptr_t>(0);

/// Process-wide socket runtime lifetime.
class SocketRuntime {
 public:
  static Status ensure_initialised();
};

/// Connected stream socket.
class Connection {
 public:
  Connection() = default;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;
  ~Connection();

  [[nodiscard]] static Connection adopt(std::uintptr_t socket) noexcept;

  /// Connect to a listener bound on 127.0.0.1.
  [[nodiscard]] static Result<Connection> connect_loopback(std::uint16_t port);

  [[nodiscard]] Status send(const Frame& frame, std::size_t max_payload = kMaxFramePayload);
  /// Send already-framed bytes verbatim. Exists so adversarial transport tests
  /// can put malformed bytes on a real wire instead of trusting the encoder.
  [[nodiscard]] Status send_raw(std::span<const std::byte> bytes);
  [[nodiscard]] Result<Frame> receive(std::size_t max_payload = kMaxFramePayload);
  /// Wait until at least one byte is available. Returns false on timeout.
  [[nodiscard]] Result<bool> wait_readable(std::uint32_t milliseconds);
  [[nodiscard]] Status close() noexcept;
  void shutdown_send() noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<std::string> peer_address() const;

 private:
  std::uintptr_t socket_ = kInvalidSocketHandle;
};

/// Loopback listener restricted to 127.0.0.1.
class Listener {
 public:
  Listener() = default;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  ~Listener();

  /// Bind to 127.0.0.1:\p port. Port 0 selects an ephemeral port.
  [[nodiscard]] static Result<Listener> bind_loopback(std::uint16_t port = 0);
  [[nodiscard]] Result<Connection> accept_one();
  [[nodiscard]] Result<Connection> accept_within(std::uint32_t milliseconds);
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] Status close() noexcept;
  [[nodiscard]] bool valid() const noexcept;

 private:
  std::uintptr_t socket_ = kInvalidSocketHandle;
  std::uint16_t port_ = 0;
};

}  // namespace backpressure
