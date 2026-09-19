#pragma once

// Backpressure Fabric - propagation authority.
//
// Authority is an explicit, bounded grant. It names the epoch under which it
// was issued, the policy generation it belongs to, the resources it covers, how
// many hops it is willing to travel, and whether it permits amplification. An
// authority vector that cannot be bound to a live epoch authorizes nothing.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/fixed.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/generation.hpp"

namespace backpressure {

enum class AuthorityLevel : std::uint8_t {
  /// No authority at all. Cannot authorize anything.
  None = 0,
  /// May be recorded and explained, never propagated.
  Observer = 1,
  /// May propagate within the local neighbourhood, bounded by max hops.
  Local = 2,
  /// May propagate only to explicitly enumerated resources.
  Scoped = 3,
  /// May propagate anywhere the topology permits.
  Global = 4,
  Count,
};

[[nodiscard]] constexpr const char* to_string(AuthorityLevel level) noexcept {
  switch (level) {
    case AuthorityLevel::None: return "None";
    case AuthorityLevel::Observer: return "Observer";
    case AuthorityLevel::Local: return "Local";
    case AuthorityLevel::Scoped: return "Scoped";
    case AuthorityLevel::Global: return "Global";
    case AuthorityLevel::Count: break;
  }
  return "Invalid";
}

/// Bounded set of resources an authority covers.
struct AuthorityScope {
  static constexpr std::size_t kMaxEntries = 64;

  std::array<ResourceId, kMaxEntries> items{};
  std::uint32_t count = 0;

  /// Adding the same resource twice is a no-op, not an error.
  [[nodiscard]] Status add(ResourceId id) {
    if (!id.valid()) {
      return Status::error(ErrorCode::InvalidArgument, "scope id");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      if (items[i] == id) {
        return Status::success();
      }
    }
    if (count >= kMaxEntries) {
      return Status::error(ErrorCode::LimitExceeded, "authority scope", count);
    }
    items[count] = id;
    ++count;
    return Status::success();
  }

  [[nodiscard]] bool contains(ResourceId id) const noexcept {
    for (std::uint32_t i = 0; i < count; ++i) {
      if (items[i] == id) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return count; }
};

/// The authority vector attached to a pressure signal.
struct AuthorityVector {
  AuthorityLevel level = AuthorityLevel::None;
  /// Epoch under which this authority was granted. Must equal the live epoch.
  Epoch epoch{};
  /// Policy generation this grant was made against.
  Generation policy_generation{};
  AuthorityScope scope{};
  /// Hop budget granted by this authority. Propagation also obeys policy hops;
  /// the effective bound is the minimum of the two.
  std::uint32_t max_hops_granted = 0;
  bool amplification_authorized = false;
  /// Upper bound on cumulative gain when amplification is authorized.
  Gain max_gain = Gain::unity();
  /// Logical tick after which the grant is void. kNoTick means no explicit expiry.
  Tick expires_at = kNoTick;

  [[nodiscard]] static AuthorityVector none() noexcept { return AuthorityVector{}; }

  [[nodiscard]] static AuthorityVector make_global(Epoch epoch, Generation policy,
                                                   std::uint32_t max_hops) noexcept {
    AuthorityVector a;
    a.level = AuthorityLevel::Global;
    a.epoch = epoch;
    a.policy_generation = policy;
    a.max_hops_granted = max_hops;
    return a;
  }

  [[nodiscard]] Status validate() const {
    if (level >= AuthorityLevel::Count) {
      return Status::error(ErrorCode::InvalidArgument, "authority level",
                           static_cast<std::uint64_t>(level));
    }
    if (level == AuthorityLevel::None) {
      return Status::error(ErrorCode::Unauthorized, "authority is None");
    }
    if (!epoch.known()) {
      return Status::error(ErrorCode::Unauthorized, "authority epoch unbound");
    }
    if (level == AuthorityLevel::Scoped && scope.empty()) {
      return Status::error(ErrorCode::Unauthorized, "scoped authority without scope");
    }
    if (level != AuthorityLevel::Global && max_hops_granted == 0u) {
      return Status::error(ErrorCode::Unauthorized, "authority without hop budget");
    }
    if (amplification_authorized && max_gain.is_unity()) {
      return Status::error(ErrorCode::InvalidArgument, "amplification authorized without gain");
    }
    return Status::success();
  }

  /// May this grant start or continue propagation at all?
  [[nodiscard]] bool permits_propagation() const noexcept {
    return level == AuthorityLevel::Local || level == AuthorityLevel::Scoped ||
           level == AuthorityLevel::Global;
  }

  /// Does this grant cover an effect at p target?
  [[nodiscard]] bool covers(ResourceId target) const noexcept {
    switch (level) {
      case AuthorityLevel::Global:
        return true;
      case AuthorityLevel::Scoped:
        return scope.contains(target);
      case AuthorityLevel::Local:
        // Local authority covers exactly what it was scoped to; an unscoped
        // Local grant covers nothing, which is the conservative reading.
        return scope.contains(target);
      case AuthorityLevel::Observer:
      case AuthorityLevel::None:
      case AuthorityLevel::Count:
        break;
    }
    return false;
  }

  [[nodiscard]] bool expired_at(Tick now) const noexcept {
    return expires_at != kNoTick && now > expires_at;
  }

  /// Full authority decision, including epoch and expiry binding.
  [[nodiscard]] Status check(Tick now, Epoch live_epoch, ResourceId target) const {
    BPFAB_TRY(validate());
    if (live_epoch != epoch) {
      return Status::error(ErrorCode::StaleEpoch, "authority epoch", epoch.value());
    }
    if (expired_at(now)) {
      return Status::error(ErrorCode::Expired, "authority expired", expires_at);
    }
    if (!covers(target)) {
      return Status::error(ErrorCode::Unauthorized, "authority scope", target.value());
    }
    return Status::success();
  }

  [[nodiscard]] Digest128 digest() const noexcept {
    DigestBuilder b;
    b.domain(0x20u);
    b.update_u8(static_cast<std::uint8_t>(level));
    digest_epoch(b, epoch);
    digest_generation(b, policy_generation);
    b.update_u32(count_of_scope());
    for (std::uint32_t i = 0; i < scope.count; ++i) {
      b.update_u64(scope.items[i].value());
    }
    b.update_u32(max_hops_granted);
    b.update_bool(amplification_authorized);
    b.update_u32(max_gain.raw());
    b.update_u64(expires_at);
    return b.finish();
  }

 private:
  [[nodiscard]] std::uint32_t count_of_scope() const noexcept {
    return static_cast<std::uint32_t>(scope.size());
  }
};

}  // namespace backpressure