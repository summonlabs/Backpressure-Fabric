#pragma once

// Backpressure Fabric - runtime host.
//
// The fabric owns the live, authoritative state of one process: the installed
// topology generation, the active policy generation, the authority epoch, the
// publisher registry with exact incarnations, the fence set, the propagation
// ledger and the optional durable store. It is the only component that decides
// whether a statement may take effect, and it never restores liveness from
// disk.
//
// Locking contract
// ----------------
//   * state_mutex_ guards topology_, policy_, publishers_, fences_, ledger_,
//     epoch_ and revalidation_. Lock order is state_mutex_ first; the worker
//     pool mutex is never acquired while state_mutex_ is held, and state_mutex_
//     is never acquired while the pool mutex is held.
//   * The engine is pure. It is invoked with state_mutex_ held and never calls
//     back into the fabric, so it cannot re-enter the lock.
//   * No callback is invoked and no event is emitted while any lock is held.
//   * shutdown() releases state_mutex_ before joining workers.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/engine/engine.hpp"
#include "backpressure/model/fence.hpp"
#include "backpressure/model/policy.hpp"
#include "backpressure/model/topology.hpp"
#include "backpressure/store/durable_state.hpp"

namespace backpressure {

struct FabricConfig {
  /// Empty means in-memory only: no durability, and recovery is not claimed.
  std::string state_directory{};
  JournalLimits journal_limits{};
  TopologyLimits topology_limits{};
  std::size_t max_publishers = 64;
  std::size_t ledger_lineage_capacity = PropagationLedger::kDefaultLineageCapacity;
  std::size_t ledger_application_capacity = PropagationLedger::kDefaultApplicationCapacity;
  std::size_t ledger_damping_capacity = PropagationLedger::kDefaultDampingCapacity;
  std::size_t journal_compaction_threshold = 8192;
  /// Hard bound on the revalidation footprint.
  std::size_t max_revalidation_entries = 1u << 16;
  /// Worker threads for asynchronous publish. 0 keeps publishing synchronous.
  std::size_t worker_threads = 0;
  std::size_t worker_queue_capacity = 256;
  std::size_t max_tickets = 4096;
};

enum class PublisherState : std::uint8_t {
  Unknown = 0,
  Live,
  Retired,
  Count,
};

[[nodiscard]] constexpr const char* to_string(PublisherState s) noexcept {
  switch (s) {
    case PublisherState::Unknown: return "Unknown";
    case PublisherState::Live: return "Live";
    case PublisherState::Retired: return "Retired";
    case PublisherState::Count: break;
  }
  return "Invalid";
}

struct PublisherRecord {
  SourceId source{};
  Incarnation incarnation{};
  PublisherState state = PublisherState::Unknown;
  Tick registered_at = kNoTick;
  Tick last_heartbeat = kNoTick;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  /// Always false immediately after open: liveness is never restored.
  bool restored_from_durable = false;
};

struct FabricStatus {
  Epoch epoch{};
  Epoch previous_epoch{};
  Incarnation incarnation{};
  bool durable = false;
  bool has_topology = false;
  bool has_policy = false;
  Digest128 topology_digest{};
  Generation topology_generation{};
  PolicyId policy_id{};
  Generation policy_generation{};
  std::size_t publishers = 0;
  std::size_t live_publishers = 0;
  std::size_t fences = 0;
  std::size_t revalidation_required = 0;
  std::size_t recovered_lineages = 0;
  std::uint64_t lineage_evictions = 0;
  std::uint64_t ambiguous_attempts = 0;
  std::uint64_t corrupt_records = 0;
  std::uint64_t signals_accepted = 0;
  std::uint64_t signals_rejected = 0;
  std::uint64_t topologies_installed = 0;
  std::uint64_t policies_installed = 0;
  bool shutting_down = false;
  bool restored_liveness = false;
};

using TicketId = std::uint64_t;
inline constexpr TicketId kInvalidTicket = 0;

enum class TicketState : std::uint8_t {
  Pending = 0,
  Running,
  Completed,
  Failed,
  Cancelled,
  Count,
};

[[nodiscard]] constexpr const char* to_string(TicketState s) noexcept {
  switch (s) {
    case TicketState::Pending: return "Pending";
    case TicketState::Running: return "Running";
    case TicketState::Completed: return "Completed";
    case TicketState::Failed: return "Failed";
    case TicketState::Cancelled: return "Cancelled";
    case TicketState::Count: break;
  }
  return "Invalid";
}

struct PublishTicket {
  TicketId id = kInvalidTicket;
  TicketState state = TicketState::Pending;
  Status status{};
  PropagationOutcome outcome{};
  /// True only when the publish crossed its authoritative completion boundary.
  bool committed = false;
  /// True when a cancellation request was observed before that boundary.
  bool cancelled = false;
  /// Shared cancellation request. Checked under the state lock immediately
  /// before the authoritative commit, so a cancelled publish never mutates
  /// authoritative state and never later reports success.
  std::shared_ptr<std::atomic<bool>> cancel_flag{};
};

/// Bounded worker pool. The pool mutex is never held while running a job, and
/// workers are joined without holding it.
class WorkerPool {
 public:
  WorkerPool() = default;
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  ~WorkerPool();

  [[nodiscard]] Status start(std::size_t threads, std::size_t queue_capacity);
  [[nodiscard]] Status submit(std::function<void()> work);
  /// Block until every queued job has run and no job is executing.
  [[nodiscard]] Status drain();
  /// Stop accepting work, run queued work, then join.
  [[nodiscard]] Status shutdown();
  /// Stop accepting work, discard queued work, then join. Running jobs finish.
  [[nodiscard]] Status abort();

  [[nodiscard]] bool started() const noexcept { return started_.load(); }
  [[nodiscard]] std::size_t queued() const;
  [[nodiscard]] std::size_t active() const;
  [[nodiscard]] std::uint64_t completed() const noexcept { return completed_.load(); }
  [[nodiscard]] std::uint64_t discarded() const noexcept { return discarded_.load(); }

 private:
  void worker_loop();

  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> threads_;
  std::size_t capacity_ = 0;
  std::size_t active_ = 0;
  bool stopping_ = false;
  bool discard_ = false;
  std::atomic<bool> started_{false};
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> discarded_{0};
};

class Fabric {
 public:
  Fabric() = default;
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;
  Fabric(Fabric&&) = delete;
  Fabric& operator=(Fabric&&) = delete;
  ~Fabric();

  /// Open the fabric. When config.state_directory is set the durable store is
  /// opened, the epoch is advanced, this process's incarnation is recorded, and
  /// durable configuration, fences and lineage are replayed. No liveness, lease
  /// or pressure is restored.
  [[nodiscard]] static Result<std::unique_ptr<Fabric>> open(const FabricConfig& config);

  // --- configuration -------------------------------------------------------
  [[nodiscard]] Status install_topology(std::vector<Resource> resources,
                                        std::vector<DependencyEdge> edges, Tick now);
  [[nodiscard]] Status install_policy(PropagationPolicy policy, Tick now);

  // --- publishers ----------------------------------------------------------
  [[nodiscard]] Status register_publisher(SourceId source, const Incarnation& incarnation,
                                          Tick now);
  [[nodiscard]] Status heartbeat(SourceId source, const Incarnation& incarnation, Tick now);
  [[nodiscard]] Status retire_publisher(SourceId source, const Incarnation& incarnation, Tick now);

  // --- fences --------------------------------------------------------------
  [[nodiscard]] Status put_fence(Fence fence, Tick now);
  [[nodiscard]] Status clear_fence(FenceId id, Tick now);

  // --- propagation ---------------------------------------------------------
  [[nodiscard]] Result<PropagationOutcome> publish(const PressureSignal& signal, Tick now);
  [[nodiscard]] Result<PropagationOutcome> publish_batch(std::vector<PressureSignal> signals,
                                                         Tick now);
  [[nodiscard]] Result<PropagationOutcome> recover(SourceId source, ResourceId origin,
                                                   Generation origin_generation, Magnitude residual,
                                                   const AuthorityVector& authority, Tick now);

  // --- revalidation --------------------------------------------------------
  [[nodiscard]] Status mark_revalidation_required(ResourceId resource, Tick now);
  [[nodiscard]] Status clear_revalidation(ResourceId resource, Tick now);
  [[nodiscard]] std::vector<ResourceId> revalidation_required() const;

  // --- asynchronous publishing --------------------------------------------
  [[nodiscard]] Result<TicketId> publish_async(PressureSignal signal, Tick now);
  [[nodiscard]] Result<PublishTicket> ticket(TicketId id) const;
  [[nodiscard]] Status release_ticket(TicketId id);
  [[nodiscard]] Status cancel(TicketId id);
  [[nodiscard]] Status drain();
  [[nodiscard]] std::size_t pending_tickets() const;

  // --- introspection -------------------------------------------------------
  [[nodiscard]] FabricStatus status() const;
  [[nodiscard]] std::vector<PublisherRecord> publishers() const;
  [[nodiscard]] std::vector<Fence> fences() const;
  [[nodiscard]] std::shared_ptr<const Topology> topology() const;
  [[nodiscard]] std::shared_ptr<const PropagationPolicy> policy() const;
  [[nodiscard]] Epoch epoch() const;
  [[nodiscard]] const Incarnation& incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] bool shutting_down() const noexcept { return shutting_down_.load(); }
  [[nodiscard]] bool durable() const noexcept { return store_ != nullptr; }

  // --- lifecycle -----------------------------------------------------------
  /// Persist the current pressure footprint so a restart knows what must be
  /// re-observed rather than assumed.
  [[nodiscard]] Status flush(Tick now);
  [[nodiscard]] Status shutdown(Tick now);

 private:
  [[nodiscard]] Result<PropagationOutcome> publish_locked(const PressureSignal& signal, Tick now,
                                                          bool durable_intent,
                                                          const std::atomic<bool>* cancel_flag);
  [[nodiscard]] Status flush_locked(Tick now);
  [[nodiscard]] Status journal_compaction_locked(Tick now);
  void complete_ticket(TicketId id, PublishTicket ticket);
  void mark_ticket_running(TicketId id);
  [[nodiscard]] bool ticket_cancelled(TicketId id) const;

  FabricConfig config_{};
  mutable std::mutex mutex_;
  std::unique_ptr<DurableStore> store_{};
  std::shared_ptr<const Topology> topology_{};
  std::shared_ptr<const PropagationPolicy> policy_{};
  PropagationLedger ledger_{};
  FenceTable fences_{};
  std::unordered_map<SourceId, PublisherRecord> publishers_{};
  std::vector<ResourceId> revalidation_{};
  Incarnation incarnation_{};
  Epoch epoch_{};
  Epoch previous_epoch_{};
  std::uint64_t topologies_installed_ = 0;
  std::uint64_t policies_installed_ = 0;
  std::uint64_t signals_accepted_ = 0;
  std::uint64_t signals_rejected_ = 0;
  std::atomic<bool> shutting_down_{false};

  WorkerPool pool_{};
  mutable std::mutex ticket_mutex_;
  std::unordered_map<TicketId, PublishTicket> tickets_{};
  std::deque<TicketId> ticket_order_{};
  std::atomic<TicketId> next_ticket_{1};
  std::atomic<std::uint64_t> attempts_{1};
};

}  // namespace backpressure