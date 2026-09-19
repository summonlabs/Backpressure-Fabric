#pragma once

// Backpressure Fabric - dependency edges.
//
// An edge states "pressure observed at \c from is a reason for pressure at
// \c to". The directed relationship, its attenuation, its hop override and its
// cooldown are all revisioned by a dependency generation: when the relationship
// changes, its generation advances and every propagation bound to the old
// generation becomes stale.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/fixed.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/model/resource.hpp"

namespace backpressure {

enum class EdgeFlagKind : std::uint16_t {
  None = 0,
  /// Propagation must not continue past this edge's target.
  Barrier = 1u << 0,
  /// This edge is permitted to amplify when the policy authorizes it.
  Amplifying = 1u << 1,
  /// This edge only carries recovery (decay) traffic, never pressure increases.
  RecoveryOnly = 1u << 2,
  /// Declaration is provisional: usable for explanation, not for authority.
  Provisional = 1u << 3,
};

/// Small bitset of edge flags with checked arithmetic-free semantics.
class EdgeFlags {
 public:
  constexpr EdgeFlags() noexcept = default;
  constexpr explicit EdgeFlags(std::uint16_t bits) noexcept : bits_(bits) {}

  [[nodiscard]] static constexpr EdgeFlags none() noexcept { return EdgeFlags{}; }
  [[nodiscard]] static constexpr EdgeFlags of(EdgeFlagKind k) noexcept {
    return EdgeFlags(static_cast<std::uint16_t>(k));
  }

  [[nodiscard]] constexpr bool has(EdgeFlagKind k) const noexcept {
    return (bits_ & static_cast<std::uint16_t>(k)) != 0u;
  }
  [[nodiscard]] constexpr std::uint16_t bits() const noexcept { return bits_; }

  constexpr EdgeFlags& operator|=(EdgeFlagKind k) noexcept {
    bits_ = static_cast<std::uint16_t>(bits_ | static_cast<std::uint16_t>(k));
    return *this;
  }

  friend constexpr EdgeFlags operator|(EdgeFlags a, EdgeFlagKind k) noexcept {
    return EdgeFlags(static_cast<std::uint16_t>(a.bits_ | static_cast<std::uint16_t>(k)));
  }
  friend constexpr bool operator==(EdgeFlags a, EdgeFlags b) noexcept {
    return a.bits_ == b.bits_;
  }
  friend constexpr bool operator!=(EdgeFlags a, EdgeFlags b) noexcept { return !(a == b); }

 private:
  std::uint16_t bits_ = 0;
};

struct DependencyEdge {
  EdgeId id{};
  ResourceId from{};
  ResourceId to{};
  /// Multiplier applied to pressure crossing this edge. Defaults to zero, which
  /// means "declared but not yet carrying pressure" - the safe reading.
  Attenuation attenuation = Attenuation::none();
  /// Amplification authorized for this edge, only honoured when the policy
  /// authorizes amplification and the Amplifying flag is set.
  Gain amplification = Gain::unity();
  /// Per-edge hop override; 0 means "use the policy default".
  std::uint32_t max_hops = 0;
  /// Revision of this dependency relationship.
  Generation dependency_generation{};
  /// Minimum ticks between two accepted propagations on this edge. 0 means
  /// "use the policy default"; the policy default of 0 disables damping.
  Tick cooldown_ticks = 0;
  /// Lower sorts first when expanding a node's edges.
  std::uint16_t priority = 0;
  EdgeFlags flags{};
  Digest128 provenance{};

  [[nodiscard]] constexpr bool is_self_loop() const noexcept { return from == to; }
  [[nodiscard]] constexpr bool is_amplifying() const noexcept {
    return flags.has(EdgeFlagKind::Amplifying) && !amplification.is_unity();
  }
  [[nodiscard]] constexpr bool is_barrier() const noexcept {
    return flags.has(EdgeFlagKind::Barrier);
  }

  [[nodiscard]] Status validate() const {
    if (!id.valid()) {
      return Status::error(ErrorCode::InvalidArgument, "edge id");
    }
    if (!from.valid() || !to.valid()) {
      return Status::error(ErrorCode::InvalidArgument, "edge endpoint");
    }
    if (!dependency_generation.known()) {
      return Status::error(ErrorCode::StaleGeneration, "edge generation unbound", id.value());
    }
    if (max_hops > kMaxEdgeHops) {
      return Status::error(ErrorCode::OutOfRange, "edge max hops", max_hops);
    }
    if (is_amplifying() && is_barrier()) {
      return Status::error(ErrorCode::Conflict, "amplifying barrier edge", id.value());
    }
    return Status::success();
  }

  /// Deterministic traversal order: priority, then target, then edge identity.
  friend bool edge_precedes(const DependencyEdge& a, const DependencyEdge& b) noexcept {
    if (a.priority != b.priority) {
      return a.priority < b.priority;
    }
    if (a.to != b.to) {
      return a.to < b.to;
    }
    return a.id < b.id;
  }

  void digest_into(DigestBuilder& b) const noexcept {
    b.domain(0x40u);
    b.update_u64(id.value());
    b.update_u64(from.value());
    b.update_u64(to.value());
    b.update_u32(attenuation.q16().raw());
    b.update_u32(amplification.raw());
    b.update_u32(max_hops);
    digest_generation(b, dependency_generation);
    b.update_u64(cooldown_ticks);
    b.update_u16(priority);
    b.update_u16(flags.bits());
    b.update_digest(provenance);
  }

  static constexpr std::uint32_t kMaxEdgeHops = 64;
};

}  // namespace backpressure