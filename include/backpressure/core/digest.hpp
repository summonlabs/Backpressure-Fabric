#pragma once

// Backpressure Fabric - deterministic digests and integrity checks.
//
// Digests are used for lineage (chain of custody of a pressure signal),
// explanation fingerprints and durable journal record chaining. They must be
// identical for identical logical content on every platform and build, so the
// construction is explicit and endian-normalised.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace backpressure {

/// 128-bit digest. Zero is reserved to mean "unset".
struct Digest128 {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0 && lo == 0; }
  [[nodiscard]] constexpr std::uint64_t fold64() const noexcept {
    return hi ^ (lo + 0x9E3779B97F4A7C15ull + (hi << 6) + (hi >> 2));
  }

  friend constexpr bool operator==(const Digest128& a, const Digest128& b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
  }
  friend constexpr bool operator!=(const Digest128& a, const Digest128& b) noexcept {
    return !(a == b);
  }
};

/// Lowercase 32 character hexadecimal rendering.
[[nodiscard]] std::string to_hex(Digest128 digest);

/// Incremental digest construction. Domain separators are mixed in between
/// fields so that concatenation ambiguities cannot collide.
class DigestBuilder {
 public:
  DigestBuilder() noexcept = default;

  void update(std::span<const std::byte> bytes) noexcept;
  void update(std::string_view text) noexcept;
  void update_u8(std::uint8_t v) noexcept;
  void update_u16(std::uint16_t v) noexcept;
  void update_u32(std::uint32_t v) noexcept;
  void update_u64(std::uint64_t v) noexcept;
  void update_bool(bool v) noexcept;
  void update_digest(Digest128 v) noexcept;
  /// Mixes an explicit domain separator so field boundaries are part of the hash.
  void domain(std::uint8_t tag) noexcept;

  [[nodiscard]] Digest128 finish() const noexcept;

 private:
  void mix_word(std::uint64_t w) noexcept;

  std::uint64_t h1_ = 0x243F6A8885A308D3ull;
  std::uint64_t h2_ = 0x13198A2E03707344ull;
  std::uint64_t len_ = 0;
};

[[nodiscard]] Digest128 digest_of(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] Digest128 digest_of(std::string_view text) noexcept;

/// Derive a 64-bit sub-digest for labelled sub-streams of one digest.
[[nodiscard]] inline Digest128 derive_digest(Digest128 parent, std::uint8_t tag) noexcept {
  DigestBuilder b;
  b.domain(0x02u);
  b.update_digest(parent);
  b.update_u8(tag);
  return b.finish();
}

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint32_t crc32_update(std::uint32_t seed,
                                         std::span<const std::byte> bytes) noexcept;

}  // namespace backpressure

namespace std {
template <>
struct hash<backpressure::Digest128> {
  [[nodiscard]] std::size_t operator()(const backpressure::Digest128& d) const noexcept {
    return static_cast<std::size_t>(d.fold64());
  }
};
}  // namespace std
