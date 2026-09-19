#pragma once

// Backpressure Fabric - propagation fences.
//
// A fence is a hard, explicit refusal that outranks any authority, attenuation
// or hop budget. Fences come in two lifetimes: volatile fences belong to the
// epoch that declared them and die when the epoch advances, while durable
// fences survive restart and are explicitly re-bound to the new epoch during
// recovery so that a restart never silently removes a refusal.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/generation.hpp"

namespace backpressure {

enum class FenceKind : std::uint8_t {
  /// No pressure may be applied at this resource at all.
  RefuseIngress = 0,
  /// Pressure may be applied but must not travel further from this resource.
  RefuseEgress,
  /// Strongest refusal: equivalent to RefuseIngress plus RefuseEgress.
  Barrier,
  Count,
};

[[nodiscard]] constexpr const char* to_string(FenceKind k) noexcept {
  switch (k) {
    case FenceKind::RefuseIngress: return "RefuseIngress";
    case FenceKind::RefuseEgress: return "RefuseEgress";
    case FenceKind::Barrier: return "Barrier";
    case FenceKind::Count: break;
  }
  return "Invalid";
}

struct Fence {
  FenceId id{};
  ResourceId resource{};
  FenceKind kind = FenceKind::Barrier;
  /// Epoch under which this fence is currently held.
  Epoch epoch{};
  /// Optional binding to one dependency generation. Zero means "any".
  Generation dependency_generation{};
  /// Logical expiry; kNoTick means the fence does not expire.
  Tick expires_at = kNoTick;
  /// Survives restart and is re-bound on recovery.
  bool durable = false;
  Digest128 provenance{};

  [[nodiscard]] Status validate() const {
    if (!id.valid()) {
      return Status::error(ErrorCode::InvalidArgument, "fence id");
    }
    if (!resource.valid()) {
      return Status::error(ErrorCode::InvalidArgument, "fence resource");
    }
    if (kind >= FenceKind::Count) {
      return Status::error(ErrorCode::InvalidArgument, "fence kind",
                           static_cast<std::uint64_t>(kind));
    }
    if (!epoch.known()) {
      return Status::error(ErrorCode::StaleEpoch, "fence epoch unbound");
    }
    return Status::success();
  }

  [[nodiscard]] constexpr bool refuses_ingress() const noexcept {
    return kind == FenceKind::RefuseIngress || kind == FenceKind::Barrier;
  }
  [[nodiscard]] constexpr bool refuses_egress() const noexcept {
    return kind == FenceKind::RefuseEgress || kind == FenceKind::Barrier;
  }
  [[nodiscard]] constexpr bool expired_at(Tick now) const noexcept {
    return expires_at != kNoTick && now > expires_at;
  }

  /// Strength used to pick the governing fence at a resource.
  [[nodiscard]] constexpr int strength() const noexcept {
    return kind == FenceKind::Barrier ? 2 : 1;
  }
};

/// Bounded fence set. Not internally synchronized: the owner serialises access,
/// exactly as it does for the propagation ledger.
class FenceTable {
 public:
  static constexpr std::size_t kMaxFences = 4096;

  FenceTable() = default;

  [[nodiscard]] Status add(const Fence& fence) {
    BPFAB_TRY(fence.validate());
    for (Fence& existing : fences_) {
      if (existing.id == fence.id) {
        existing = fence;
        return Status::success();
      }
    }
    if (fences_.size() >= kMaxFences) {
      return Status::error(ErrorCode::LimitExceeded, "fence table", fences_.size());
    }
    fences_.push_back(fence);
    return Status::success();
  }

  [[nodiscard]] Status remove(FenceId id) {
    const auto it = std::find_if(fences_.begin(), fences_.end(),
                                 [id](const Fence& f) { return f.id == id; });
    if (it == fences_.end()) {
      return Status::error(ErrorCode::NotFound, "fence", id.value());
    }
    fences_.erase(it);
    return Status::success();
  }

  /// Strongest live fence governing \p resource, or nullptr.
  [[nodiscard]] const Fence* governing(ResourceId resource, Tick now) const noexcept {
    const Fence* best = nullptr;
    for (const Fence& f : fences_) {
      if (f.resource != resource || f.expired_at(now)) {
        continue;
      }
      if (best == nullptr || f.strength() > best->strength()) {
        best = &f;
      }
    }
    return best;
  }

  /// Drop volatile fences that belong to a superseded epoch.
  std::size_t purge_stale(Epoch live_epoch) {
    const std::size_t before = fences_.size();
    fences_.erase(std::remove_if(fences_.begin(), fences_.end(),
                                 [live_epoch](const Fence& f) {
                                   return !f.durable && f.epoch != live_epoch;
                                 }),
                  fences_.end());
    return before - fences_.size();
  }

  /// Re-bind durable fences to the live epoch. Returns how many were re-bound.
  /// This is an explicit, recorded recovery step: a restart never silently
  /// restores a fence, and never silently drops one either.
  std::size_t rebind_durable(Epoch live_epoch) {
    std::size_t rebound = 0;
    for (Fence& f : fences_) {
      if (f.durable && f.epoch != live_epoch) {
        f.epoch = live_epoch;
        ++rebound;
      }
    }
    return rebound;
  }

  /// Remove fences that expired at \p now. Returns how many were removed.
  std::size_t expire(Tick now) {
    const std::size_t before = fences_.size();
    fences_.erase(std::remove_if(fences_.begin(), fences_.end(),
                                 [now](const Fence& f) { return f.expired_at(now); }),
                  fences_.end());
    return before - fences_.size();
  }

  void clear() noexcept { fences_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return fences_.size(); }
  [[nodiscard]] bool empty() const noexcept { return fences_.empty(); }
  [[nodiscard]] std::span<const Fence> fences() const noexcept { return fences_; }

 private:
  std::vector<Fence> fences_;
};

}  // namespace backpressure
