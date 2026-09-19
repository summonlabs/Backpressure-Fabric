#pragma once

// Backpressure Fabric - durable fabric state.
//
// The store is the only component that writes to disk. It persists
// configuration, fences, epoch and boot binding, propagation lineage and
// attempt outcomes, and it reconstructs exactly that on recovery. It never
// restores liveness: a recovered process has no publishers, no leases, no live
// pressure and no authority until those are re-established and re-bound to the
// new epoch by explicit calls.
//
// Durable mutation protocol:
//   validate -> bind authority -> plan -> reserve -> journal -> work -> verify
//   -> commit -> retire
//
// An attempt that was begun but neither committed nor aborted is reported as
// ambiguous. Ambiguity is never resolved by guessing.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/fence.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/model/policy.hpp"
#include "backpressure/store/journal.hpp"

namespace backpressure {

struct DurableConfig {
  /// Directory holding the journal and sidecar files. Required.
  std::string directory;
  std::string journal_name = "fabric.bpfj";
  JournalLimits journal_limits{};
  std::size_t max_lineages = 1u << 16;
  std::size_t max_footprint_entries = 1u << 16;
};

struct RecoveredState {
  /// Epoch in force before this open, and the epoch established by this open.
  Epoch previous_epoch{};
  Epoch epoch{};
  BootId boot{};
  Incarnation incarnation{};
  bool boot_recorded = false;

  bool has_policy = false;
  PropagationPolicy policy{};

  bool has_topology_binding = false;
  Digest128 topology_digest{};
  Generation topology_generation{};

  std::vector<Fence> fences{};
  std::deque<Digest128> lineages{};
  std::uint64_t lineage_evictions = 0;
  std::vector<ResourceId> revalidation_required{};
  std::size_t footprint_entries = 0;
  std::uint64_t footprint_overflow = 0;

  std::uint64_t attempts_open = 0;
  std::uint64_t attempts_ambiguous = 0;
  std::uint64_t attempts_committed = 0;
  std::uint64_t attempts_aborted = 0;

  std::uint64_t records_replayed = 0;
  std::uint64_t corrupt_records = 0;
  bool journal_repaired = false;
  std::uint64_t stale_temp_files_removed = 0;

  /// Always false. Present so that a caller cannot overlook the fact that no
  /// liveness, freshness, lease or authority is restored by recovery.
  bool restored_liveness = false;
};

[[nodiscard]] Status serialize_policy(const PropagationPolicy& policy,
                                      std::vector<std::byte>& out, std::size_t max_bytes);
[[nodiscard]] Result<PropagationPolicy> deserialize_policy(std::span<const std::byte> bytes);

[[nodiscard]] Status serialize_fence(const Fence& fence, std::vector<std::byte>& out);
[[nodiscard]] Result<Fence> deserialize_fence(std::span<const std::byte> bytes);

class DurableStore {
 public:
  static constexpr std::uint8_t kRevalidationRequest = 1;
  static constexpr std::uint8_t kRevalidationClear = 2;
  static constexpr std::uint8_t kRevalidationFootprint = 3;

  DurableStore() = default;
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  DurableStore(DurableStore&&) = default;
  DurableStore& operator=(DurableStore&&) = default;

  [[nodiscard]] static Result<DurableStore> open(const DurableConfig& config);

  [[nodiscard]] const RecoveredState& state() const noexcept { return state_; }
  [[nodiscard]] const Journal& journal() const noexcept { return journal_; }
  [[nodiscard]] const std::string& directory() const noexcept { return config_.directory; }
  [[nodiscard]] std::uint64_t ambiguous_attempts() const noexcept {
    return state_.attempts_ambiguous;
  }

  /// Advance the epoch and durably record it. Must be called once per process
  /// before any authority is accepted.
  [[nodiscard]] Status establish_epoch(Tick now);

  /// Record this process incarnation. Always establishes a fresh incarnation;
  /// no previous incarnation is ever revived.
  [[nodiscard]] Status record_boot(const Incarnation& incarnation, Tick now);

  [[nodiscard]] Status persist_policy(const PropagationPolicy& policy, Tick now);
  [[nodiscard]] Status bind_topology(Digest128 digest, Generation generation, Tick now);
  [[nodiscard]] Status put_fence(const Fence& fence, Tick now);
  [[nodiscard]] Status remove_fence(FenceId id, Tick now);
  [[nodiscard]] Status record_lineage(Digest128 lineage, Tick now);
  [[nodiscard]] Status request_revalidation(ResourceId resource, Tick now);
  [[nodiscard]] Status clear_revalidation(ResourceId resource, Tick now);
  /// Record which (source, resource) pairs currently carry pressure so that a
  /// restart knows exactly what must be re-observed rather than assumed.
  [[nodiscard]] Status record_pressure_footprint(
      std::span<const std::pair<SourceId, ResourceId>> entries, Tick now);

  [[nodiscard]] Status begin_attempt(AttemptId id, Tick now);
  [[nodiscard]] Status commit_attempt(AttemptId id, Tick now);
  [[nodiscard]] Status abort_attempt(AttemptId id, Tick now);

  [[nodiscard]] Status sync();
  /// Rewrite the journal from recovered state, dropping superseded records.
  [[nodiscard]] Status compact();
  [[nodiscard]] std::size_t compact_if_needed(std::size_t record_threshold);

  [[nodiscard]] std::size_t fence_count() const noexcept { return state_.fences.size(); }
  [[nodiscard]] bool locked() const noexcept { return lock_.held(); }

 private:
  [[nodiscard]] Status replay();
  [[nodiscard]] Status apply_record(const JournalRecord& record);
  [[nodiscard]] Status remember_lineage(Digest128 lineage);

  DurableConfig config_{};
  Journal journal_{};
  fsutil::ExclusiveLock lock_{};
  RecoveredState state_{};
  std::vector<AttemptId> open_attempts_{};
  std::uint64_t attempts_committed_seen_ = 0;
};

}  // namespace backpressure