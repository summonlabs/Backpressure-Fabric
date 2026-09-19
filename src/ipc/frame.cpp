// Backpressure Fabric - framed transport implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/ipc/frame.hpp"

#include "backpressure/core/digest.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace backpressure {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kBadSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kBadSocket = -1;
#endif

[[nodiscard]] NativeSocket to_native(std::uintptr_t handle) noexcept {
  return static_cast<NativeSocket>(handle);
}

void close_native(std::uintptr_t handle) noexcept {
  if (handle == kInvalidSocketHandle) {
    return;
  }
  const NativeSocket s = to_native(handle);
  if (s == kBadSocket) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(s);
#else
  ::close(s);
#endif
}

[[nodiscard]] int last_socket_error() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

[[nodiscard]] bool is_would_block(int code) noexcept {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT;
#else
  return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
#endif
}

void put_u32(std::byte* p, std::uint32_t v) noexcept {
  for (unsigned i = 0; i < 4u; ++i) {
    p[i] = std::byte{static_cast<unsigned char>((v >> (8u * i)) & 0xFFu)};
  }
}

void put_u16(std::byte* p, std::uint16_t v) noexcept {
  p[0] = std::byte{static_cast<unsigned char>(v & 0xFFu)};
  p[1] = std::byte{static_cast<unsigned char>((v >> 8u) & 0xFFu)};
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* p) noexcept {
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4u; ++i) {
    v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8u * i);
  }
  return v;
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(
          std::to_integer<std::uint8_t>(p[1]))
                                 << 8u));
}

[[nodiscard]] Status send_all(NativeSocket s, const std::byte* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const int chunk = static_cast<int>(
        size - sent > 0x40000000u ? 0x40000000u : (size - sent));
#if defined(_WIN32)
    const int n = ::send(s, reinterpret_cast<const char*>(data + sent), chunk, 0);
#else
    const ssize_t n = ::send(s, data + sent, static_cast<std::size_t>(chunk), MSG_NOSIGNAL);
#endif
    if (n <= 0) {
      const int code = last_socket_error();
      if (is_would_block(code)) {
        continue;
      }
      return Status::error(ErrorCode::IoError, "socket send", static_cast<std::uint64_t>(code));
    }
    sent += static_cast<std::size_t>(n);
  }
  return Status::success();
}

[[nodiscard]] Status recv_all(NativeSocket s, std::byte* data, std::size_t size) {
  std::size_t received = 0;
  while (received < size) {
    const int chunk = static_cast<int>(
        size - received > 0x40000000u ? 0x40000000u : (size - received));
#if defined(_WIN32)
    const int n = ::recv(s, reinterpret_cast<char*>(data + received), chunk, 0);
#else
    const ssize_t n = ::recv(s, data + received, static_cast<std::size_t>(chunk), 0);
#endif
    if (n == 0) {
      return Status::error(ErrorCode::TruncatedInput, "peer closed", received);
    }
    if (n < 0) {
      const int code = last_socket_error();
      if (is_would_block(code)) {
        continue;
      }
      return Status::error(ErrorCode::IoError, "socket recv", static_cast<std::uint64_t>(code));
    }
    received += static_cast<std::size_t>(n);
  }
  return Status::success();
}

std::once_flag g_socket_once;
Status g_socket_status = Status::error(ErrorCode::NotReady, "socket runtime not initialised");

}  // namespace

Status SocketRuntime::ensure_initialised() {
  std::call_once(g_socket_once, []() {
#if defined(_WIN32)
    WSADATA data{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      g_socket_status = Status::error(ErrorCode::IoError, "WSAStartup",
                                      static_cast<std::uint64_t>(rc));
      return;
    }
#endif
    g_socket_status = Status::success();
  });
  return g_socket_status;
}

Status encode_frame(const Frame& frame, std::vector<std::byte>& out, std::size_t max_payload) {
  if (frame.type == MessageType::Count) {
    return Status::error(ErrorCode::InvalidArgument, "frame type");
  }
  if (frame.payload.size() > max_payload) {
    return Status::error(ErrorCode::OversizedInput, "frame payload", frame.payload.size());
  }
  out.resize(kFrameHeaderBytes + frame.payload.size());
  put_u32(out.data(), kFrameMagic);
  put_u16(out.data() + 4, kFrameVersion);
  put_u16(out.data() + 6, static_cast<std::uint16_t>(frame.type));
  put_u32(out.data() + 8, frame.flags);
  put_u32(out.data() + 12, static_cast<std::uint32_t>(frame.payload.size()));
  put_u32(out.data() + 16,
          crc32(std::span<const std::byte>(frame.payload.data(), frame.payload.size())));
  if (!frame.payload.empty()) {
    std::memcpy(out.data() + kFrameHeaderBytes, frame.payload.data(), frame.payload.size());
  }
  return Status::success();
}

Result<std::uint32_t> decode_frame_header(std::span<const std::byte> header, MessageType& type,
                                          std::uint32_t& flags, std::size_t max_payload) {
  if (header.size() != kFrameHeaderBytes) {
    return fail<std::uint32_t>(ErrorCode::TruncatedInput, "frame header", header.size());
  }
  if (get_u32(header.data()) != kFrameMagic) {
    return fail<std::uint32_t>(ErrorCode::MalformedInput, "frame magic");
  }
  const std::uint16_t version = get_u16(header.data() + 4);
  if (version != kFrameVersion) {
    return fail<std::uint32_t>(ErrorCode::VersionMismatch, "frame version", version);
  }
  const std::uint16_t raw_type = get_u16(header.data() + 6);
  if (raw_type == 0 || raw_type >= static_cast<std::uint16_t>(MessageType::Count)) {
    return fail<std::uint32_t>(ErrorCode::MalformedInput, "frame message type", raw_type);
  }
  type = static_cast<MessageType>(raw_type);
  flags = get_u32(header.data() + 8);
  const std::uint32_t length = get_u32(header.data() + 12);
  if (length > max_payload) {
    return fail<std::uint32_t>(ErrorCode::OversizedInput, "frame payload", length);
  }
  return Result<std::uint32_t>(length);
}

Result<Frame> decode_frame(std::span<const std::byte> bytes, std::size_t max_payload) {
  if (bytes.size() < kFrameHeaderBytes) {
    return fail<Frame>(ErrorCode::TruncatedInput, "frame", bytes.size());
  }
  MessageType type = MessageType::Hello;
  std::uint32_t flags = 0;
  BPFAB_TRY_DECL(const std::uint32_t, length,
                 decode_frame_header(bytes.first(kFrameHeaderBytes), type, flags, max_payload));
  if (bytes.size() != kFrameHeaderBytes + static_cast<std::size_t>(length)) {
    return fail<Frame>(ErrorCode::MalformedInput, "frame length mismatch", bytes.size());
  }
  const std::span<const std::byte> payload = bytes.subspan(kFrameHeaderBytes, length);
  const std::uint32_t expected = crc32(payload);
  const std::uint32_t stored = get_u32(bytes.data() + 16);
  if (expected != stored) {
    return fail<Frame>(ErrorCode::IntegrityMismatch, "frame crc", stored);
  }
  Frame frame;
  frame.type = type;
  frame.flags = flags;
  frame.payload.assign(payload.begin(), payload.end());
  return Result<Frame>(std::move(frame));
}

Connection::Connection(Connection&& other) noexcept : socket_(other.socket_) {
  other.socket_ = kInvalidSocketHandle;
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    (void)close();
    socket_ = other.socket_;
    other.socket_ = kInvalidSocketHandle;
  }
  return *this;
}

Connection::~Connection() { close_native(socket_); }

Connection Connection::adopt(std::uintptr_t socket) noexcept {
  Connection c;
  c.socket_ = socket;
  return c;
}

bool Connection::valid() const noexcept {
  return socket_ != kInvalidSocketHandle && to_native(socket_) != kBadSocket;
}

Status Connection::send(const Frame& frame, std::size_t max_payload) {
  if (!valid()) {
    return Status::error(ErrorCode::NotReady, "connection closed");
  }
  std::vector<std::byte> bytes;
  BPFAB_TRY(encode_frame(frame, bytes, max_payload));
  return send_all(to_native(socket_), bytes.data(), bytes.size());
}

Status Connection::send_raw(std::span<const std::byte> bytes) {
  if (!valid()) {
    return Status::error(ErrorCode::NotReady, "connection closed");
  }
  return send_all(to_native(socket_), bytes.data(), bytes.size());
}

Result<Connection> Connection::connect_loopback(std::uint16_t port) {
  BPFAB_TRY(SocketRuntime::ensure_initialised());
  const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBadSocket) {
    return fail<Connection>(ErrorCode::IoError, "socket create",
                            static_cast<std::uint64_t>(last_socket_error()));
  }
  int nodelay = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               static_cast<int>(sizeof(nodelay)));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(s, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    const int code = last_socket_error();
    close_native(static_cast<std::uintptr_t>(s));
    return fail<Connection>(ErrorCode::IoError, "connect loopback",
                            static_cast<std::uint64_t>(code));
  }
  return Result<Connection>(Connection::adopt(static_cast<std::uintptr_t>(s)));
}

Result<Frame> Connection::receive(std::size_t max_payload) {
  if (!valid()) {
    return fail<Frame>(ErrorCode::NotReady, "connection closed");
  }
  std::byte header[kFrameHeaderBytes] = {};
  BPFAB_TRY(recv_all(to_native(socket_), header, kFrameHeaderBytes));
  MessageType type = MessageType::Hello;
  std::uint32_t flags = 0;
  BPFAB_TRY_DECL(const std::uint32_t, length,
                 decode_frame_header(std::span<const std::byte>(header, kFrameHeaderBytes), type,
                                     flags, max_payload));
  std::vector<std::byte> payload(length);
  if (length > 0) {
    BPFAB_TRY(recv_all(to_native(socket_), payload.data(), length));
  }
  const std::uint32_t expected = crc32(std::span<const std::byte>(payload.data(), payload.size()));
  const std::uint32_t stored = get_u32(header + 16);
  if (expected != stored) {
    return fail<Frame>(ErrorCode::IntegrityMismatch, "frame crc", stored);
  }
  Frame frame;
  frame.type = type;
  frame.flags = flags;
  frame.payload = std::move(payload);
  return Result<Frame>(std::move(frame));
}

Result<bool> Connection::wait_readable(std::uint32_t milliseconds) {
  if (!valid()) {
    return fail<bool>(ErrorCode::NotReady, "connection closed");
  }
  const NativeSocket s = to_native(socket_);
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(s, &read_set);
  timeval tv{};
  tv.tv_sec = static_cast<long>(milliseconds / 1000u);
  tv.tv_usec = static_cast<long>((milliseconds % 1000u) * 1000u);
#if defined(_WIN32)
  const int rc = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
  const int rc = ::select(s + 1, &read_set, nullptr, nullptr, &tv);
#endif
  if (rc < 0) {
    return fail<bool>(ErrorCode::IoError, "select");
  }
  return Result<bool>(rc > 0);
}

Status Connection::close() noexcept {
  close_native(socket_);
  socket_ = kInvalidSocketHandle;
  return Status::success();
}

void Connection::shutdown_send() noexcept {
  if (!valid()) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(to_native(socket_), SD_SEND);
#else
  ::shutdown(to_native(socket_), SHUT_WR);
#endif
}

Result<std::string> Connection::peer_address() const {
  if (!valid()) {
    return fail<std::string>(ErrorCode::NotReady, "connection closed");
  }
  sockaddr_storage storage{};
  int length = static_cast<int>(sizeof(storage));
  if (::getpeername(to_native(socket_), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
    return fail<std::string>(ErrorCode::IoError, "getpeername");
  }
  if (storage.ss_family == AF_INET) {
    const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
    // Formatted directly from the network-order address rather than through
    // inet_ntop, whose annotated output contract the analyzer cannot verify.
    const std::uint32_t host = ntohl(addr->sin_addr.s_addr);
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", (host >> 24u) & 0xFFu,
                  (host >> 16u) & 0xFFu, (host >> 8u) & 0xFFu, host & 0xFFu);
    return Result<std::string>(std::string(buffer));
  }
  return Result<std::string>(std::string("non-ipv4"));
}

Listener::Listener(Listener&& other) noexcept : socket_(other.socket_), port_(other.port_) {
  other.socket_ = kInvalidSocketHandle;
  other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    (void)close();
    socket_ = other.socket_;
    port_ = other.port_;
    other.socket_ = kInvalidSocketHandle;
    other.port_ = 0;
  }
  return *this;
}

Listener::~Listener() { close_native(socket_); }

bool Listener::valid() const noexcept {
  return socket_ != kInvalidSocketHandle && to_native(socket_) != kBadSocket;
}

Result<Listener> Listener::bind_loopback(std::uint16_t port) {
  BPFAB_TRY(SocketRuntime::ensure_initialised());
  const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBadSocket) {
    return fail<Listener>(ErrorCode::IoError, "socket create",
                          static_cast<std::uint64_t>(last_socket_error()));
  }
  int reuse = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               static_cast<int>(sizeof(reuse)));
  int nodelay = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               static_cast<int>(sizeof(nodelay)));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(s, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
    const int code = last_socket_error();
    close_native(static_cast<std::uintptr_t>(s));
    return fail<Listener>(ErrorCode::IoError, "bind loopback", static_cast<std::uint64_t>(code));
  }
  if (::listen(s, 4) != 0) {
    const int code = last_socket_error();
    close_native(static_cast<std::uintptr_t>(s));
    return fail<Listener>(ErrorCode::IoError, "listen", static_cast<std::uint64_t>(code));
  }
  sockaddr_in bound{};
  int length = static_cast<int>(sizeof(bound));
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    close_native(static_cast<std::uintptr_t>(s));
    return fail<Listener>(ErrorCode::IoError, "getsockname");
  }
  Listener listener;
  listener.socket_ = static_cast<std::uintptr_t>(s);
  listener.port_ = ntohs(bound.sin_port);
  return Result<Listener>(std::move(listener));
}

Result<Connection> Listener::accept_one() {
  if (!valid()) {
    return fail<Connection>(ErrorCode::NotReady, "listener closed");
  }
  const NativeSocket s = ::accept(to_native(socket_), nullptr, nullptr);
  if (s == kBadSocket) {
    return fail<Connection>(ErrorCode::IoError, "accept",
                            static_cast<std::uint64_t>(last_socket_error()));
  }
  int nodelay = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               static_cast<int>(sizeof(nodelay)));
  return Result<Connection>(Connection::adopt(static_cast<std::uintptr_t>(s)));
}

Result<Connection> Listener::accept_within(std::uint32_t milliseconds) {
  if (!valid()) {
    return fail<Connection>(ErrorCode::NotReady, "listener closed");
  }
  const NativeSocket s = to_native(socket_);
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(s, &read_set);
  timeval tv{};
  tv.tv_sec = static_cast<long>(milliseconds / 1000u);
  tv.tv_usec = static_cast<long>((milliseconds % 1000u) * 1000u);
#if defined(_WIN32)
  const int rc = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
  const int rc = ::select(s + 1, &read_set, nullptr, nullptr, &tv);
#endif
  if (rc < 0) {
    return fail<Connection>(ErrorCode::IoError, "select accept");
  }
  if (rc == 0) {
    return fail<Connection>(ErrorCode::NotReady, "accept window elapsed");
  }
  return accept_one();
}

Status Listener::close() noexcept {
  close_native(socket_);
  socket_ = kInvalidSocketHandle;
  port_ = 0;
  return Status::success();
}

}  // namespace backpressure