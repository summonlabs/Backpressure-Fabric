#pragma once

// Backpressure Fabric - bounded binary encoding.
//
// Every encoder enforces a hard capacity; every decoder refuses truncated,
// oversized and trailing input. Wire and journal layouts are little-endian and
// length-delimited so a malformed producer cannot make the reader allocate
// unbounded memory.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "backpressure/core/digest.hpp"
#include "backpressure/core/status.hpp"

namespace backpressure {

/// Append-only bounded encoder.
class ByteWriter {
 public:
  explicit ByteWriter(std::size_t max_bytes);

  [[nodiscard]] Status u8(std::uint8_t v);
  [[nodiscard]] Status u16(std::uint16_t v);
  [[nodiscard]] Status u32(std::uint32_t v);
  [[nodiscard]] Status u64(std::uint64_t v);
  [[nodiscard]] Status i64(std::int64_t v);
  [[nodiscard]] Status boolean(bool v);
  [[nodiscard]] Status digest(Digest128 v);
  /// Unprefixed bytes; caller is responsible for knowing the length.
  [[nodiscard]] Status bytes(std::span<const std::byte> v);
  /// u32 length prefix followed by the bytes.
  [[nodiscard]] Status bytes_prefixed(std::span<const std::byte> v);
  /// u32 length prefix followed by UTF-8 text, bounded by p max_len.
  [[nodiscard]] Status text(std::string_view v, std::size_t max_len);

  [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return max_bytes_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return max_bytes_ - buffer_.size(); }
  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }

  [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buffer_); }

 private:
  [[nodiscard]] Status reserve(std::size_t n);
  std::vector<std::byte> buffer_;
  std::size_t max_bytes_;
};

/// Bounds-checked decoder over an immutable view.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<Digest128> digest();
  [[nodiscard]] Result<std::span<const std::byte>> bytes(std::size_t n);
  [[nodiscard]] Result<std::span<const std::byte>> bytes_prefixed(std::size_t max_len);
  [[nodiscard]] Result<std::string_view> text(std::size_t max_len);

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
  /// Fails with MalformedInput when bytes remain unread.
  [[nodiscard]] Status expect_end() const;

 private:
  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
};

[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

}  // namespace backpressure
