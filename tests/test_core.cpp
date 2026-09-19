// Backpressure Fabric - core primitives tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>
#include <string>

#include "backpressure/backpressure.hpp"
#include "framework.hpp"

using namespace backpressure;

BPFAB_TEST(core, checked_arithmetic_refuses_overflow) {
  BPFAB_REQUIRE(checked::add_u64(1, 2, "add").ok());
  BPFAB_CHECK(checked::add_u64(1, 2, "add").value() == 3u);
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  BPFAB_REQUIRE_CODE(checked::add_u64(max, 1, "add"), ErrorCode::Overflow);
  BPFAB_REQUIRE_CODE(checked::mul_u64(max, 2, "mul"), ErrorCode::Overflow);
  BPFAB_CHECK(checked::mul_u64(max, 1, "mul").value() == max);
  BPFAB_REQUIRE_CODE(checked::sub_u64(1, 2, "sub"), ErrorCode::Overflow);
  BPFAB_REQUIRE_CODE(checked::div_u64(1, 0, "div"), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_CODE(checked::to_u8(256, "u8"), ErrorCode::OutOfRange);
  BPFAB_CHECK(checked::to_u8(255, "u8").value() == 255u);
  BPFAB_REQUIRE_CODE(checked::to_u16(70000, "u16"), ErrorCode::OutOfRange);
  BPFAB_CHECK(checked::add_sat_u64(max, max, 1000u) == 1000u);
  BPFAB_CHECK(checked::mul_sat_u64(max, max, 1000u) == 1000u);
  BPFAB_CHECK(checked::add_sat_u64(10u, 10u, 1000u) == 20u);
  BPFAB_REQUIRE_CODE(checked::mul_u32(70000u, 70000u, "mul32"), ErrorCode::OutOfRange);
}

BPFAB_TEST(core, fixed_point_range_is_enforced) {
  BPFAB_REQUIRE(Q16::from_raw(0u).ok());
  BPFAB_REQUIRE(Q16::from_raw(65536u).ok());
  BPFAB_REQUIRE_CODE(Q16::from_raw(65537u), ErrorCode::OutOfRange);
  BPFAB_REQUIRE(Q16::from_ratio(1u, 0u).status().code() == ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_CODE(Q16::from_ratio(3u, 2u), ErrorCode::OutOfRange);
  BPFAB_CHECK(Q16::from_ratio(1u, 2u).value().raw() == 32768u);
  BPFAB_CHECK(Q16::from_percent_milli(100000u).value().is_one());
  BPFAB_REQUIRE_CODE(Q16::from_percent_milli(100001u), ErrorCode::OutOfRange);
  BPFAB_CHECK(Q16::from_percent_milli(50000u).value().raw() == 32768u);
  BPFAB_CHECK(Q16::one().as_percent_milli() == 100000u);
}

BPFAB_TEST(core, attenuation_is_monotone_and_amplification_is_bounded) {
  const Potential start = Potential::from_magnitude(Magnitude::full());
  BPFAB_CHECK(start.raw() == Potential::kOne);
  Potential walking = start;
  for (int i = 0; i < 64; ++i) {
    const Potential next = walking.attenuated(Attenuation::from_percent_milli(50000u).value());
    BPFAB_REQUIRE(next <= walking);
    walking = next;
  }
  BPFAB_CHECK(walking.raw() < start.raw());

  const Magnitude full = Magnitude::full();
  BPFAB_CHECK(full.raw() == 65536u);
  BPFAB_CHECK(Magnitude::from_raw_q16_saturating(70000u) == full);

  const Gain gain = Gain::from_percent_milli(200000u).value();
  const Result<Potential> amplified = start.amplified(gain);
  BPFAB_REQUIRE(amplified.status().code() == ErrorCode::Overflow ||
                amplified.value().raw() <= Potential::kOne);
  BPFAB_REQUIRE_CODE(Gain::from_percent_milli(50000u), ErrorCode::OutOfRange);

  const Potential half = Potential::from_raw(Potential::kOne / 2u);
  const Result<Potential> doubled = half.amplified(Gain::from_percent_milli(200000u).value());
  BPFAB_REQUIRE(doubled.ok());
  BPFAB_CHECK(doubled.value().raw() == Potential::kOne);
}

BPFAB_TEST(core, strong_ids_reject_the_sentinel) {
  BPFAB_CHECK(!ResourceId{}.valid());
  BPFAB_CHECK(ResourceId(1u).valid());
  BPFAB_REQUIRE_CODE(ResourceId::from_u64(std::numeric_limits<std::uint64_t>::max()),
                     ErrorCode::OutOfRange);
  BPFAB_REQUIRE(ResourceId::from_u64(7u).ok());
  BPFAB_CHECK(ResourceId::from_u64(7u).value().value() == 7u);
  BPFAB_CHECK(ResourceId(1u) < ResourceId(2u));
  BPFAB_CHECK(ResourceId(1u) != ResourceId(2u));
  BPFAB_CHECK(EdgeId(1u) == EdgeId(1u));
  (void)std::hash<ResourceId>{}(ResourceId(3u));
}

BPFAB_TEST(core, generation_and_epoch_never_wrap) {
  Generation generation = Generation::initial();
  BPFAB_CHECK(generation.known());
  for (int i = 0; i < 4; ++i) {
    const auto next = generation.next();
    BPFAB_REQUIRE(next.ok());
    generation = next.value();
  }
  BPFAB_CHECK(generation.value() == 5u);
  const Generation saturated = Generation::from_raw(std::numeric_limits<std::uint64_t>::max());
  BPFAB_REQUIRE_CODE(saturated.next(), ErrorCode::Overflow);
  BPFAB_CHECK(!Generation::unknown().known());

  Epoch epoch(1);
  const auto next_epoch = epoch.next();
  BPFAB_REQUIRE(next_epoch.ok());
  BPFAB_CHECK(next_epoch.value().value() == 2u);
  const Epoch saturated_epoch(std::numeric_limits<std::uint64_t>::max());
  BPFAB_REQUIRE_CODE(saturated_epoch.next(), ErrorCode::Overflow);
}

BPFAB_TEST(core, symbol_table_is_stable_and_bounded) {
  SymbolTable table;
  const auto first = table.intern("port-0");
  BPFAB_REQUIRE(first.ok());
  const auto again = table.intern("port-0");
  BPFAB_REQUIRE(again.ok());
  BPFAB_CHECK(first.value() == again.value());
  const auto second = table.intern("port-1");
  BPFAB_REQUIRE(second.ok());
  BPFAB_CHECK(second.value() != first.value());
  BPFAB_CHECK(table.lookup(first.value()) == "port-0");
  BPFAB_CHECK(table.lookup(9999u).empty());
  BPFAB_REQUIRE_CODE(table.intern(""), ErrorCode::InvalidArgument);
  const std::string long_name(SymbolTable::kMaxNameBytes + 1u, 'x');
  BPFAB_REQUIRE_CODE(table.intern(long_name), ErrorCode::OversizedInput);
  BPFAB_REQUIRE_CODE(table.intern("third", 2u), ErrorCode::LimitExceeded);
  BPFAB_CHECK(table.find("missing").status().code() == ErrorCode::NotFound);
}

BPFAB_TEST(core, digests_are_deterministic_and_separated) {
  const Digest128 a = digest_of("abc");
  const Digest128 b = digest_of("abc");
  BPFAB_CHECK(a == b);
  BPFAB_CHECK(a != digest_of("abd"));
  BPFAB_CHECK(a != digest_of("ab"));
  BPFAB_CHECK(digest_of("") != digest_of(std::string_view("\0", 1)));
  BPFAB_CHECK(!a.is_zero());
  BPFAB_CHECK(to_hex(a).size() == 32u);

  DigestBuilder builder;
  builder.domain(0x11u);
  builder.update_u32(1u);
  builder.update_u64(2u);
  const Digest128 built = builder.finish();
  DigestBuilder other;
  other.domain(0x11u);
  other.update_u32(1u);
  other.update_u64(2u);
  BPFAB_CHECK(built == other.finish());
}

BPFAB_TEST(core, crc32_matches_the_standard_vector) {
  const std::string check = "123456789";
  const std::span<const std::byte> bytes(
      reinterpret_cast<const std::byte*>(check.data()), check.size());
  BPFAB_CHECK(crc32(bytes) == 0xCBF43926u);
  BPFAB_CHECK(crc32(std::span<const std::byte>()) == 0u);
}

BPFAB_TEST(core, byte_io_refuses_truncated_and_oversized_input) {
  ByteWriter writer(8);
  BPFAB_REQUIRE(writer.u32(0xDEADBEEFu).ok());
  BPFAB_REQUIRE(writer.u32(1u).ok());
  BPFAB_REQUIRE_CODE(writer.u8(1u), ErrorCode::OversizedInput);

  ByteWriter small(2);
  const std::string text = "hello";
  BPFAB_REQUIRE_CODE(
      small.text(text, 64u),
      ErrorCode::OversizedInput);

  ByteWriter nested(64);
  BPFAB_REQUIRE(nested.bytes_prefixed(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(text.data()), text.size()))
                    .ok());
  ByteReader reader(std::span<const std::byte>(nested.buffer().data(), nested.buffer().size()));
  BPFAB_REQUIRE_RESULT(view, reader.bytes_prefixed(64u));
  BPFAB_CHECK(view.size() == 5u);
  BPFAB_CHECK(std::string(reinterpret_cast<const char*>(view.data()), view.size()) == "hello");
  BPFAB_CHECK(reader.at_end());
  ByteReader bounded(std::span<const std::byte>(nested.buffer().data(), nested.buffer().size()));
  BPFAB_REQUIRE_CODE(bounded.bytes_prefixed(3u), ErrorCode::OversizedInput);

  ByteReader full(std::span<const std::byte>(nested.buffer().data(), nested.buffer().size()));
  BPFAB_REQUIRE_RESULT(whole, full.text(64u));
  BPFAB_CHECK(whole == text);
  BPFAB_REQUIRE_OK(full.expect_end());

  ByteReader trailing(std::span<const std::byte>(nested.buffer().data(), nested.buffer().size()));
  BPFAB_REQUIRE_RESULT(one, trailing.u8());
  (void)one;
  BPFAB_REQUIRE_CODE(trailing.expect_end(), ErrorCode::MalformedInput);

  const std::byte one_byte[1] = {std::byte{1}};
  ByteReader tiny(std::span<const std::byte>(one_byte, 1));
  BPFAB_CHECK(tiny.u32().status().code() == ErrorCode::TruncatedInput);

  const std::byte bad_bool[1] = {std::byte{7}};
  ByteReader bad(std::span<const std::byte>(bad_bool, 1));
  BPFAB_CHECK(bad.boolean().status().code() == ErrorCode::MalformedInput);
}

BPFAB_TEST(core, generators_are_reproducible) {
  SplitMix64 first(12345u);
  SplitMix64 second(12345u);
  for (int i = 0; i < 32; ++i) {
    BPFAB_CHECK(first.next_u64() == second.next_u64());
  }
  SplitMix64 bounded(9u);
  for (int i = 0; i < 64; ++i) {
    BPFAB_CHECK(bounded.next_bounded(10u) < 10u);
  }
  BPFAB_CHECK(bounded.next_bounded(0u) == 0u);
  Pcg32 pcg_first(77u);
  Pcg32 pcg_second(77u);
  for (int i = 0; i < 32; ++i) {
    BPFAB_CHECK(pcg_first.next_u32() == pcg_second.next_u32());
  }
  BPFAB_CHECK(derive_seed(1u, "a") != derive_seed(1u, "b"));
  BPFAB_CHECK(derive_seed(1u, "a") == derive_seed(1u, "a"));
}

BPFAB_TEST(core, logical_clock_never_moves_backwards) {
  LogicalClock clock(10);
  BPFAB_CHECK(clock.now() == 10u);
  const auto advanced = clock.advance(5);
  BPFAB_REQUIRE(advanced.ok());
  BPFAB_CHECK(clock.now() == 15u);
  BPFAB_REQUIRE_CODE(clock.set(14u), ErrorCode::InvalidArgument);
  BPFAB_REQUIRE_OK(clock.set(20u));
  BPFAB_CHECK(clock.now() == 20u);
  const auto overflow = clock.advance(std::numeric_limits<Tick>::max());
  BPFAB_REQUIRE_CODE(overflow.status(), ErrorCode::Overflow);
}

BPFAB_TEST(core, status_carries_bounded_context) {
  const Status status = Status::error(ErrorCode::Fenced, "a very long context string that is longer than the buffer");
  BPFAB_CHECK(status.code() == ErrorCode::Fenced);
  BPFAB_CHECK(status.context().size() <= kStatusContextBytes);
  BPFAB_CHECK(status.context().substr(0, 5) == "a ver");
  BPFAB_CHECK(Status::success().ok());
  BPFAB_CHECK(std::string(to_string(ErrorCode::CycleDetected)) == "CycleDetected");
  for (std::uint16_t i = 0; i < kErrorCodeCount; ++i) {
    BPFAB_CHECK(std::string(to_string(static_cast<ErrorCode>(i))) != "UnknownErrorCode");
  }
}