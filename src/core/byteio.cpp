// Backpressure Fabric - bounded binary encoding implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/core/byteio.hpp"

#include "backpressure/core/checked.hpp"

#include <cstring>
#include <limits>

namespace backpressure {
namespace {

[[nodiscard]] Status needs(std::size_t remaining, std::size_t required,
                           std::string_view what) {
  if (remaining < required) {
    return Status::error(ErrorCode::TruncatedInput, what, required);
  }
  return Status::success();
}

}  // namespace

ByteWriter::ByteWriter(std::size_t max_bytes) : max_bytes_(max_bytes) {
  buffer_.reserve(max_bytes < 4096u ? max_bytes : 4096u);
}

Status ByteWriter::reserve(std::size_t n) {
  if (n > remaining()) {
    return Status::error(ErrorCode::OversizedInput, "writer capacity", n);
  }
  return Status::success();
}

Status ByteWriter::u8(std::uint8_t v) {
  BPFAB_TRY(reserve(1));
  buffer_.push_back(std::byte{static_cast<unsigned char>(v)});
  return Status::success();
}

Status ByteWriter::u16(std::uint16_t v) {
  BPFAB_TRY(reserve(2));
  buffer_.push_back(std::byte{static_cast<unsigned char>(v & 0xFFu)});
  buffer_.push_back(std::byte{static_cast<unsigned char>((v >> 8u) & 0xFFu)});
  return Status::success();
}

Status ByteWriter::u32(std::uint32_t v) {
  BPFAB_TRY(reserve(4));
  for (unsigned i = 0; i < 4u; ++i) {
    buffer_.push_back(std::byte{static_cast<unsigned char>((v >> (8u * i)) & 0xFFu)});
  }
  return Status::success();
}

Status ByteWriter::u64(std::uint64_t v) {
  BPFAB_TRY(reserve(8));
  for (unsigned i = 0; i < 8u; ++i) {
    buffer_.push_back(std::byte{static_cast<unsigned char>((v >> (8u * i)) & 0xFFu)});
  }
  return Status::success();
}

Status ByteWriter::i64(std::int64_t v) {
  return u64(static_cast<std::uint64_t>(v));
}

Status ByteWriter::boolean(bool v) { return u8(v ? 1u : 0u); }

Status ByteWriter::digest(Digest128 v) {
  BPFAB_TRY(u64(v.hi));
  return u64(v.lo);
}

Status ByteWriter::bytes(std::span<const std::byte> v) {
  BPFAB_TRY(reserve(v.size()));
  buffer_.insert(buffer_.end(), v.begin(), v.end());
  return Status::success();
}

Status ByteWriter::bytes_prefixed(std::span<const std::byte> v) {
  BPFAB_TRY_DECL(std::uint32_t, len, checked::to_u32(v.size(), "bytes length"));
  BPFAB_TRY(u32(len));
  return bytes(v);
}

Status ByteWriter::text(std::string_view v, std::size_t max_len) {
  if (v.size() > max_len) {
    return Status::error(ErrorCode::OversizedInput, "text length", v.size());
  }
  return bytes_prefixed(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(v.data()), v.size()));
}

Result<std::uint8_t> ByteReader::u8() {
  if (remaining() < 1u) {
    return fail<std::uint8_t>(ErrorCode::TruncatedInput, "u8", offset_);
  }
  return Result<std::uint8_t>(std::to_integer<std::uint8_t>(data_[offset_++]));
}

Result<std::uint16_t> ByteReader::u16() {
  if (remaining() < 2u) {
    return fail<std::uint16_t>(ErrorCode::TruncatedInput, "u16", offset_);
  }
  std::uint16_t v = 0;
  for (unsigned i = 0; i < 2u; ++i) {
    v |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data_[offset_ + i]))
         << (8u * i);
  }
  offset_ += 2u;
  return Result<std::uint16_t>(v);
}

Result<std::uint32_t> ByteReader::u32() {
  if (remaining() < 4u) {
    return fail<std::uint32_t>(ErrorCode::TruncatedInput, "u32", offset_);
  }
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4u; ++i) {
    v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset_ + i]))
         << (8u * i);
  }
  offset_ += 4u;
  return Result<std::uint32_t>(v);
}

Result<std::uint64_t> ByteReader::u64() {
  if (remaining() < 8u) {
    return fail<std::uint64_t>(ErrorCode::TruncatedInput, "u64", offset_);
  }
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8u; ++i) {
    v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data_[offset_ + i]))
         << (8u * i);
  }
  offset_ += 8u;
  return Result<std::uint64_t>(v);
}

Result<std::int64_t> ByteReader::i64() {
  BPFAB_TRY_DECL(std::uint64_t, raw, u64());
  return Result<std::int64_t>(static_cast<std::int64_t>(raw));
}

Result<bool> ByteReader::boolean() {
  BPFAB_TRY_DECL(std::uint8_t, raw, u8());
  if (raw > 1u) {
    return fail<bool>(ErrorCode::MalformedInput, "boolean", raw);
  }
  return Result<bool>(raw == 1u);
}

Result<Digest128> ByteReader::digest() {
  BPFAB_TRY_DECL(std::uint64_t, hi, u64());
  BPFAB_TRY_DECL(std::uint64_t, lo, u64());
  return Result<Digest128>(Digest128{hi, lo});
}

Result<std::span<const std::byte>> ByteReader::bytes(std::size_t n) {
  BPFAB_TRY(needs(remaining(), n, "bytes"));
  const std::span<const std::byte> view(data_.data() + offset_, n);
  offset_ += n;
  return Result<std::span<const std::byte>>(view);
}

Result<std::span<const std::byte>> ByteReader::bytes_prefixed(std::size_t max_len) {
  BPFAB_TRY_DECL(std::uint32_t, len, u32());
  if (static_cast<std::size_t>(len) > max_len) {
    return fail<std::span<const std::byte>>(ErrorCode::OversizedInput, "prefixed length", len);
  }
  return bytes(static_cast<std::size_t>(len));
}

Result<std::string_view> ByteReader::text(std::size_t max_len) {
  BPFAB_TRY_DECL(std::span<const std::byte>, view, bytes_prefixed(max_len));
  return Result<std::string_view>(
      std::string_view(reinterpret_cast<const char*>(view.data()), view.size()));
}

Status ByteReader::expect_end() const {
  if (!at_end()) {
    return Status::error(ErrorCode::MalformedInput, "trailing bytes", remaining());
  }
  return Status::success();
}

}  // namespace backpressure
