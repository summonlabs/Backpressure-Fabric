// Backpressure Fabric - digest and integrity implementations.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/core/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace backpressure {
namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t z) noexcept {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct Crc32Table {
  static constexpr std::size_t kEntries = 256;
  std::array<std::uint32_t, kEntries> values{};
  constexpr Crc32Table() noexcept {
    std::uint32_t* slots = values.data();
    for (std::size_t i = 0; i < kEntries; ++i) {
      std::uint32_t c = static_cast<std::uint32_t>(i);
      for (int k = 0; k < 8; ++k) {
        c = ((c & 1u) != 0u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
      }
      slots[i] = c;
    }
  }
};

constexpr Crc32Table kCrc32Table{};

constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

void DigestBuilder::mix_word(std::uint64_t w) noexcept {
  h1_ = mix64(h1_ ^ (w + 0x9E3779B97F4A7C15ull));
  h2_ = mix64(h2_ + w + 0x165667B19E3779F9ull);
  const std::uint64_t t = h1_;
  h1_ ^= (h2_ >> 29);
  h2_ ^= (t << 17);
}

void DigestBuilder::update(std::span<const std::byte> bytes) noexcept {
  std::size_t i = 0;
  while (i + 8 <= bytes.size()) {
    std::uint64_t w = 0;
    for (std::size_t k = 0; k < 8; ++k) {
      w |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i + k])) << (8u * k);
    }
    mix_word(w);
    i += 8;
  }
  if (i < bytes.size()) {
    const std::size_t tail = bytes.size() - i;
    std::uint64_t w = 0;
    for (std::size_t k = 0; k < tail; ++k) {
      w |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i + k])) << (8u * k);
    }
    mix_word(w);
    // Domain-separate the tail so "ab" and "ab\0\0..." cannot collide.
    mix_word(0xFF00000000000000ull | static_cast<std::uint64_t>(tail));
  }
  len_ += static_cast<std::uint64_t>(bytes.size());
}

void DigestBuilder::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void DigestBuilder::update_u8(std::uint8_t v) noexcept {
  const std::byte b{static_cast<unsigned char>(v)};
  update(std::span<const std::byte>(&b, 1));
}

void DigestBuilder::update_u16(std::uint16_t v) noexcept {
  std::byte buf[2];
  buf[0] = std::byte{static_cast<unsigned char>(v & 0xFFu)};
  buf[1] = std::byte{static_cast<unsigned char>((v >> 8u) & 0xFFu)};
  update(std::span<const std::byte>(buf, 2));
}

void DigestBuilder::update_u32(std::uint32_t v) noexcept {
  std::byte buf[4];
  for (std::size_t i = 0; i < 4; ++i) {
    buf[i] = std::byte{static_cast<unsigned char>((v >> (8u * i)) & 0xFFu)};
  }
  update(std::span<const std::byte>(buf, 4));
}

void DigestBuilder::update_u64(std::uint64_t v) noexcept {
  std::byte buf[8];
  for (std::size_t i = 0; i < 8; ++i) {
    buf[i] = std::byte{static_cast<unsigned char>((v >> (8u * i)) & 0xFFu)};
  }
  update(std::span<const std::byte>(buf, 8));
}

void DigestBuilder::update_bool(bool v) noexcept { update_u8(v ? 1u : 0u); }

void DigestBuilder::update_digest(Digest128 v) noexcept {
  update_u64(v.hi);
  update_u64(v.lo);
}

void DigestBuilder::domain(std::uint8_t tag) noexcept {
  mix_word(0x00FF000000000000ull | static_cast<std::uint64_t>(tag));
}

Digest128 DigestBuilder::finish() const noexcept {
  DigestBuilder copy = *this;
  copy.update_u64(len_);
  copy.domain(0x5Au);
  Digest128 d;
  d.hi = mix64(copy.h1_ ^ (copy.h2_ + 0x9E3779B97F4A7C15ull));
  d.lo = mix64(copy.h2_ ^ (copy.h1_ + 0x165667B19E3779F9ull));
  if (d.is_zero()) {
    d.lo = 0x1u;
  }
  return d;
}

Digest128 digest_of(std::span<const std::byte> bytes) noexcept {
  DigestBuilder b;
  b.domain(0x01u);
  b.update(bytes);
  return b.finish();
}

Digest128 digest_of(std::string_view text) noexcept {
  DigestBuilder b;
  b.domain(0x01u);
  b.update(text);
  return b.finish();
}

std::uint32_t crc32_update(std::uint32_t seed, std::span<const std::byte> bytes) noexcept {
  std::uint32_t crc = ~seed;
  for (const std::byte b : bytes) {
    const std::uint8_t idx = static_cast<std::uint8_t>(
        (crc ^ static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b))) & 0xFFu);
    crc = kCrc32Table.values[idx] ^ (crc >> 8u);
  }
  return ~crc;
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
  return crc32_update(0u, bytes);
}

std::string to_hex(Digest128 digest) {
  std::string out;
  out.resize(32);
  const std::uint64_t parts[2] = {digest.hi, digest.lo};
  std::size_t o = 0;
  for (const std::uint64_t p : parts) {
    for (int nib = 15; nib >= 0; --nib) {
      out[o++] = kHexDigits[(p >> (4u * static_cast<unsigned>(nib))) & 0xFu];
    }
  }
  return out;
}

}  // namespace backpressure