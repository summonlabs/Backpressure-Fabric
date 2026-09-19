// Backpressure Fabric - runtime host implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/runtime/fabric.hpp"

#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace backpressure {
namespace {

[[nodiscard]] std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

}  // namespace

// ===========================================================================
// WorkerPool
// ===========================================================================

WorkerPool::~WorkerPool() {
  if (started_.load()) {
    (void)abort();
  }
}

Status WorkerPool::start(std::size_t threads, std::size_t queue_capacity) {
  if (started_.load()) {
    return Status::error(ErrorCode::Conflict, "worker pool already started");
  }
  if (threads == 0) {
    return Status::error(ErrorCode::InvalidArgument, "worker thread count");
  }
  if (queue_capacity == 0) {
    return Status::error(ErrorCode::InvalidArgument, "worker queue capacity");
  }
  {
    std::unique_lock lock(mutex_);
    capacity_ = queue_capacity;
    stopping_ = false;
    discard_ = false;
  }
  threads_.reserve(threads);
  for (std::size_t i = 0; i < threads; ++i) {
    threads_.emplace_back([this]() { worker_loop(); });
  }
  started_.store(true);
  return Status::success();
}

void WorkerPool::worker_loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      work_available_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          break;
        }
        continue;
      }
      if (discard_) {
        queue_.clear();
        discarded_.fetch_add(1);
        if (stopping_) {
          break;
        }
        continue;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
      ++active_;
    }
    // The pool mutex is deliberately not held while the job runs: jobs take the
    // fabric state lock, and holding both would risk inversion.
    job();
    {
      std::unique_lock lock(mutex_);
      if (active_ > 0) {
        --active_;
      }
      completed_.fetch_add(1);
      if (queue_.empty() && active_ == 0) {
        idle_.notify_all();
      }
    }
  }
  {
    std::unique_lock lock(mutex_);
    idle_.notify_all();
  }
}

Status WorkerPool::submit(std::function<void()> work) {
  if (!started_.load()) {
    return Status::error(ErrorCode::NotReady, "worker pool not started");
  }
  if (!work) {
    return Status::error(ErrorCode::InvalidArgument, "empty work item");
  }
  {
    std::unique_lock lock(mutex_);
    if (stopping_) {
      return Status::error(ErrorCode::ShuttingDown, "worker pool stopping");
    }
    if (queue_.size() >= capacity_) {
      return Status::error(ErrorCode::LimitExceeded, "worker queue", queue_.size());
    }
    queue_.push_back(std::move(work));
  }
  work_available_.notify_one();
  return Status::success();
}

Status WorkerPool::drain() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [this]() { return queue_.empty() && active_ == 0; });
  return Status::success();
}

Status WorkerPool::shutdown() {
  if (!started_.load()) {
    return Status::success();
  }
  {
    std::unique_lock lock(mutex_);
    stopping_ = true;
  }
  work_available_.notify_all();
  // Join without holding the pool mutex: workers need it to finish.
  for (std::thread& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
  started_.store(false);
  return Status::success();
}

Status WorkerPool::abort() {
  if (!started_.load()) {
    return Status::success();
  }
  {
    std::unique_lock lock(mutex_);
    stopping_ = true;
    discard_ = true;
    if (!queue_.empty()) {
      discarded_.fetch_add(queue_.size());
      queue_.clear();
    }
  }
  work_available_.notify_all();
  for (std::thread& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  threads_.clear();
  started_.store(false);
  return Status::success();
}

std::size_t WorkerPool::queued() const {
  std::unique_lock lock(mutex_);
  return queue_.size();
}

std::size_t WorkerPool::active() const {
  std::unique_lock lock(mutex_);
  return active_;
}

// ===========================================================================
// Fabric
// ===========================================================================

Fabric::~Fabric() {
  if (pool_.started()) {
    (void)pool_.abort();
  }
}

Result<std::unique_ptr<Fabric>> Fabric::open(const FabricConfig& config) {
  auto fabric = std::unique_ptr<Fabric>(new Fabric());
  fabric->config_ = config;
  fabric->ledger_ = PropagationLedger(config.ledger_lineage_capacity,
                                      config.ledger_application_capacity,
                                      config.ledger_damping_capacity);

  const std::uint32_t pid = current_process_id();
  const std::uint64_t nonce = steady_nanos();
  const BootId boot = BootId::from_seed(nonce, pid, 0x9E3779B97F4A7C15ull);
  fabric->incarnation_ = Incarnation::mint(boot, pid, 0);

  if (!config.state_directory.empty()) {
    DurableConfig durable_config;
    durable_config.directory = config.state_directory;
    durable_config.journal_limits = config.journal_limits;
    durable_config.max_lineages = config.ledger_lineage_capacity;
    durable_config.max_footprint_entries =
        config.max_revalidation_entries < config.ledger_application_capacity
            ? config.max_revalidation_entries
            : config.ledger_application_capacity;

    Result<DurableStore> opened = DurableStore::open(durable_config);
    if (!opened.ok()) {
      return fail<std::unique_ptr<Fabric>>(opened.status().code(), opened.status().context(),
                                           opened.status().detail());
    }
    fabric->store_ = std::make_unique<DurableStore>(std::move(opened).value());
    fabric->previous_epoch_ = fabric->store_->state().epoch;
    BPFAB_TRY(fabric->store_->establish_epoch(kNoTick));
    fabric->epoch_ = fabric->store_->state().epoch;
    BPFAB_TRY(fabric->store_->record_boot(fabric->incarnation_, kNoTick));

    const RecoveredState& recovered = fabric->store_->state();
    if (recovered.has_policy) {
      fabric->policy_ = std::make_shared<const PropagationPolicy>(recovered.policy);
    }
    for (const Fence& fence : recovered.fences) {
      Fence rebound = fence;
      // Explicit recovery step: a durable fence is re-bound to the new epoch,
      // never silently dropped and never silently trusted at its old epoch.
      rebound.epoch = fabric->epoch_;
      BPFAB_TRY(fabric->fences_.add(rebound));
    }
    for (const Digest128 lineage : recovered.lineages) {
      (void)fabric->ledger_.mark_lineage(lineage);
    }
    fabric->revalidation_ = recovered.revalidation_required;
  } else {
    fabric->previous_epoch_ = Epoch::none();
    fabric->epoch_ = Epoch(1);
  }

  if (config.worker_threads > 0) {
    BPFAB_TRY(fabric->pool_.start(config.worker_threads, config.worker_queue_capacity));
  }
  return Result<std::unique_ptr<Fabric>>(std::move(fabric));
}

Status Fabric::install_topology(std::vector<Resource> resources, std::vector<DependencyEdge> edges,
                                Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return Status::error(ErrorCode::ShuttingDown, "install topology");
  }
  Result<Topology> built = Topology::build(std::move(resources), std::move(edges),
                                           config_.topology_limits, true);
  if (!built.ok()) {
    return built.status();
  }
  auto topology = std::make_shared<const Topology>(std::move(built).value());
  if (store_ != nullptr) {
    BPFAB_TRY(store_->bind_topology(topology->digest(), topology->generation(), now));
  }
  topology_ = std::move(topology);
  ++topologies_installed_;
  return Status::success();
}

Status Fabric::install_policy(PropagationPolicy policy, Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return Status::error(ErrorCode::ShuttingDown, "install policy");
  }
  BPFAB_TRY(policy.validate());
  if (policy_ != nullptr && policy_->id == policy.id && policy.generation <= policy_->generation) {
    // An exact reinstall of the policy already in force is idempotent; a
    // different policy at or below the live generation is stale.
    if (policy_->digest() == policy.digest()) {
      return Status::success();
    }
    return Status::error(ErrorCode::StaleGeneration, "policy generation",
                         policy.generation.value());
  }
  if (store_ != nullptr) {
    BPFAB_TRY(store_->persist_policy(policy, now));
  }
  policy_ = std::make_shared<const PropagationPolicy>(policy);
  ++policies_installed_;
  return Status::success();
}

Status Fabric::register_publisher(SourceId source, const Incarnation& incarnation, Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return Status::error(ErrorCode::ShuttingDown, "register publisher");
  }
  if (!source.valid() || !incarnation.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "publisher identity");
  }
  const auto it = publishers_.find(source);
  if (it != publishers_.end()) {
    if (it->second.incarnation == incarnation) {
      it->second.state = PublisherState::Live;
      it->second.last_heartbeat = now;
      return Status::success();
    }
    if (same_boot(it->second.incarnation, incarnation) &&
        incarnation.seq <= it->second.incarnation.seq) {
      return Status::error(ErrorCode::StaleIncarnation, "publisher incarnation",
                           incarnation.seq);
    }
  } else if (publishers_.size() >= config_.max_publishers) {
    return Status::error(ErrorCode::LimitExceeded, "publisher registry", publishers_.size());
  }
  PublisherRecord record;
  record.source = source;
  record.incarnation = incarnation;
  record.state = PublisherState::Live;
  record.registered_at = now;
  record.last_heartbeat = now;
  record.restored_from_durable = false;
  publishers_[source] = record;
  return Status::success();
}

Status Fabric::heartbeat(SourceId source, const Incarnation& incarnation, Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return Status::error(ErrorCode::ShuttingDown, "heartbeat");
  }
  const auto it = publishers_.find(source);
  if (it == publishers_.end()) {
    return Status::error(ErrorCode::NotFound, "publisher", source.value());
  }
  if (it->second.incarnation != incarnation) {
    return Status::error(ErrorCode::StaleIncarnation, "publisher incarnation",
                         incarnation.seq);
  }
  if (it->second.state != PublisherState::Live) {
    return Status::error(ErrorCode::NotReady, "publisher not live");
  }
  if (now < it->second.last_heartbeat) {
    return Status::error(ErrorCode::InvalidArgument, "heartbeat regression", now);
  }
  it->second.last_heartbeat = now;
  return Status::success();
}

Status Fabric::retire_publisher(SourceId source, const Incarnation& incarnation, Tick now) {
  std::unique_lock lock(mutex_);
  const auto it = publishers_.find(source);
  if (it == publishers_.end()) {
    return Status::error(ErrorCode::NotFound, "publisher", source.value());
  }
  if (it->second.incarnation != incarnation) {
    return Status::error(ErrorCode::StaleIncarnation, "publisher incarnation", incarnation.seq);
  }
  it->second.state = PublisherState::Retired;
  it->second.last_heartbeat = now;
  return Status::success();
}

Status Fabric::put_fence(Fence fence, Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return Status::error(ErrorCode::ShuttingDown, "put fence");
  }
  if (!fence.epoch.known()) {
    fence.epoch = epoch_;
  }
  if (fence.epoch != epoch_) {
    return Status::error(ErrorCode::StaleEpoch, "fence epoch", fence.epoch.value());
  }
  if (store_ != nullptr) {
    BPFAB_TRY(store_->put_fence(fence, now));
  }
  return fences_.add(fence);
}

Status Fabric::clear_fence(FenceId id, Tick now) {
  std::unique_lock lock(mutex_);
  if (store_ != nullptr) {
    BPFAB_TRY(store_->remove_fence(id, now));
  }
  return fences_.remove(id);
}

std::vector<Fence> Fabric::fences() const {
  std::unique_lock lock(mutex_);
  const std::span<const Fence> view = fences_.fences();
  return std::vector<Fence>(view.begin(), view.end());
}

Status Fabric::mark_revalidation_required(ResourceId resource, Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return Status::error(ErrorCode::ShuttingDown, "mark revalidation");
  }
  if (store_ != nullptr) {
    BPFAB_TRY(store_->request_revalidation(resource, now));
  }
  if (std::find(revalidation_.begin(), revalidation_.end(), resource) == revalidation_.end()) {
    if (revalidation_.size() >= config_.max_revalidation_entries) {
      return Status::error(ErrorCode::LimitExceeded, "revalidation list", revalidation_.size());
    }
    revalidation_.push_back(resource);
  }
  return Status::success();
}

Status Fabric::clear_revalidation(ResourceId resource, Tick now) {
  std::unique_lock lock(mutex_);
  if (store_ != nullptr) {
    BPFAB_TRY(store_->clear_revalidation(resource, now));
  }
  const auto it = std::find(revalidation_.begin(), revalidation_.end(), resource);
  if (it != revalidation_.end()) {
    revalidation_.erase(it);
  }
  return Status::success();
}

std::vector<ResourceId> Fabric::revalidation_required() const {
  std::unique_lock lock(mutex_);
  return revalidation_;
}

Result<PropagationOutcome> Fabric::publish_locked(const PressureSignal& signal, Tick now,
                                                  bool durable_intent,
                                                  const std::atomic<bool>* cancel_flag) {
  if (shutting_down_.load()) {
    return fail<PropagationOutcome>(ErrorCode::ShuttingDown, "publish");
  }
  if (topology_ == nullptr || policy_ == nullptr) {
    return fail<PropagationOutcome>(ErrorCode::NotReady, "fabric not configured");
  }
  const auto it = publishers_.find(signal.source);
  if (it == publishers_.end()) {
    return fail<PropagationOutcome>(ErrorCode::NotFound, "publisher", signal.source.value());
  }
  if (it->second.incarnation != signal.publisher) {
    ++it->second.rejected;
    ++signals_rejected_;
    return fail<PropagationOutcome>(ErrorCode::StaleIncarnation, "publisher incarnation",
                                    signal.publisher.seq);
  }
  if (it->second.state != PublisherState::Live) {
    ++it->second.rejected;
    ++signals_rejected_;
    return fail<PropagationOutcome>(ErrorCode::NotReady, "publisher not live");
  }
  if (signal.epoch != epoch_) {
    ++it->second.rejected;
    ++signals_rejected_;
    return fail<PropagationOutcome>(ErrorCode::StaleEpoch, "signal epoch", signal.epoch.value());
  }
  if (signal.topology_digest != topology_->digest()) {
    ++it->second.rejected;
    ++signals_rejected_;
    return fail<PropagationOutcome>(ErrorCode::StaleGeneration, "signal topology");
  }

  EngineContext ctx;
  ctx.topology = topology_.get();
  ctx.policy = policy_.get();
  ctx.ledger = &ledger_;
  ctx.fences = &fences_;
  ctx.live_epoch = epoch_;

  const PropagationId propagation_id(attempts_.fetch_add(1));

  PropagationRequest request;
  request.id = propagation_id;
  request.policy_id = policy_->id;
  request.policy_generation = policy_->generation;
  request.topology_digest = topology_->digest();
  request.epoch = epoch_;
  request.now = now;
  request.signals.push_back(signal);

  // Phase 1: plan. Deterministic, side-effect free.
  request.dry_run = true;
  PropagationOutcome planned = PropagationEngine::propagate(ctx, request);
  if (!planned.status.ok()) {
    ++it->second.rejected;
    ++signals_rejected_;
    return Result<PropagationOutcome>(std::move(planned));
  }

  // Cancellation boundary: the decision may be abandoned here at no cost,
  // because nothing authoritative has been written yet.
  if (cancel_flag != nullptr && cancel_flag->load()) {
    PropagationOutcome cancelled = planned;
    cancelled.status = Status::error(ErrorCode::Cancelled, "publish cancelled");
    cancelled.complete = false;
    cancelled.finalize();
    ++signals_rejected_;
    return Result<PropagationOutcome>(std::move(cancelled));
  }

  // Phase 2: durable intent. Records the lineage before any authoritative
  // in-memory effect, so a crash leaves an accepted-but-ambiguous attempt
  // rather than an unrecorded effect.
  AttemptId attempt{};
  if (store_ != nullptr && durable_intent && planned.counters.signals_accepted > 0) {
    attempt = AttemptId(attempts_.fetch_add(1));
    Status st = store_->begin_attempt(attempt, now);
    if (!st.ok()) {
      ++signals_rejected_;
      return fail<PropagationOutcome>(st.code(), st.context(), st.detail());
    }
    st = store_->record_lineage(signal.lineage, now);
    if (!st.ok()) {
      (void)store_->abort_attempt(attempt, now);
      ++signals_rejected_;
      return fail<PropagationOutcome>(st.code(), st.context(), st.detail());
    }
  }

  // Phase 3: commit into the ledger.
  request.dry_run = false;
  PropagationOutcome outcome = PropagationEngine::propagate(ctx, request);
  if (!outcome.status.ok()) {
    if (attempt.valid() && store_ != nullptr) {
      (void)store_->abort_attempt(attempt, now);
    }
    ++it->second.rejected;
    ++signals_rejected_;
    return Result<PropagationOutcome>(std::move(outcome));
  }

  // Phase 4: verify. Plan and commit must agree exactly.
  if (outcome.fingerprint != planned.fingerprint) {
    if (attempt.valid() && store_ != nullptr) {
      (void)store_->abort_attempt(attempt, now);
    }
    ++signals_rejected_;
    return fail<PropagationOutcome>(ErrorCode::Internal, "plan/commit fingerprint mismatch");
  }

  if (attempt.valid() && store_ != nullptr) {
    const Status st = store_->commit_attempt(attempt, now);
    if (!st.ok()) {
      ++signals_rejected_;
      return fail<PropagationOutcome>(st.code(), st.context(), st.detail());
    }
  }

  ++signals_accepted_;
  ++it->second.accepted;
  (void)journal_compaction_locked(now);
  return Result<PropagationOutcome>(std::move(outcome));
}

Result<PropagationOutcome> Fabric::publish(const PressureSignal& signal, Tick now) {
  std::unique_lock lock(mutex_);
  return publish_locked(signal, now, true, nullptr);
}

Result<PropagationOutcome> Fabric::publish_batch(std::vector<PressureSignal> signals, Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return fail<PropagationOutcome>(ErrorCode::ShuttingDown, "publish batch");
  }
  if (topology_ == nullptr || policy_ == nullptr) {
    return fail<PropagationOutcome>(ErrorCode::NotReady, "fabric not configured");
  }
  if (signals.empty()) {
    return fail<PropagationOutcome>(ErrorCode::InvalidArgument, "empty signal batch");
  }
  for (const PressureSignal& signal : signals) {
    const auto it = publishers_.find(signal.source);
    if (it == publishers_.end()) {
      return fail<PropagationOutcome>(ErrorCode::NotFound, "publisher", signal.source.value());
    }
    if (it->second.incarnation != signal.publisher) {
      return fail<PropagationOutcome>(ErrorCode::StaleIncarnation, "publisher incarnation",
                                      signal.publisher.seq);
    }
    if (it->second.state != PublisherState::Live) {
      return fail<PropagationOutcome>(ErrorCode::NotReady, "publisher not live");
    }
  }

  EngineContext ctx;
  ctx.topology = topology_.get();
  ctx.policy = policy_.get();
  ctx.ledger = &ledger_;
  ctx.fences = &fences_;
  ctx.live_epoch = epoch_;

  const PropagationId propagation_id(attempts_.fetch_add(1));
  PropagationRequest request;
  request.id = propagation_id;
  request.policy_id = policy_->id;
  request.policy_generation = policy_->generation;
  request.topology_digest = topology_->digest();
  request.epoch = epoch_;
  request.now = now;
  request.signals = std::move(signals);

  request.dry_run = true;
  PropagationOutcome planned = PropagationEngine::propagate(ctx, request);
  if (!planned.status.ok()) {
    ++signals_rejected_;
    return Result<PropagationOutcome>(std::move(planned));
  }

  AttemptId attempt{};
  if (store_ != nullptr && planned.counters.signals_accepted > 0) {
    attempt = AttemptId(attempts_.fetch_add(1));
    Status st = store_->begin_attempt(attempt, now);
    if (!st.ok()) {
      return fail<PropagationOutcome>(st.code(), st.context(), st.detail());
    }
    for (const PressureSignal& signal : request.signals) {
      st = store_->record_lineage(signal.lineage, now);
      if (!st.ok()) {
        (void)store_->abort_attempt(attempt, now);
        return fail<PropagationOutcome>(st.code(), st.context(), st.detail());
      }
    }
  }

  request.dry_run = false;
  PropagationOutcome outcome = PropagationEngine::propagate(ctx, request);
  if (!outcome.status.ok()) {
    if (attempt.valid() && store_ != nullptr) {
      (void)store_->abort_attempt(attempt, now);
    }
    ++signals_rejected_;
    return Result<PropagationOutcome>(std::move(outcome));
  }
  if (outcome.fingerprint != planned.fingerprint) {
    if (attempt.valid() && store_ != nullptr) {
      (void)store_->abort_attempt(attempt, now);
    }
    return fail<PropagationOutcome>(ErrorCode::Internal, "plan/commit fingerprint mismatch");
  }
  if (attempt.valid() && store_ != nullptr) {
    const Status st = store_->commit_attempt(attempt, now);
    if (!st.ok()) {
      return fail<PropagationOutcome>(st.code(), st.context(), st.detail());
    }
  }
  signals_accepted_ += outcome.counters.signals_accepted;
  signals_rejected_ += outcome.counters.signals_rejected;
  (void)journal_compaction_locked(now);
  return Result<PropagationOutcome>(std::move(outcome));
}

Result<PropagationOutcome> Fabric::recover(SourceId source, ResourceId origin,
                                           Generation origin_generation, Magnitude residual,
                                           const AuthorityVector& authority, Tick now) {
  std::unique_lock lock(mutex_);
  if (shutting_down_.load()) {
    return fail<PropagationOutcome>(ErrorCode::ShuttingDown, "recover");
  }
  if (topology_ == nullptr || policy_ == nullptr) {
    return fail<PropagationOutcome>(ErrorCode::NotReady, "fabric not configured");
  }
  const auto it = publishers_.find(source);
  if (it == publishers_.end()) {
    return fail<PropagationOutcome>(ErrorCode::NotFound, "publisher", source.value());
  }
  if (it->second.state != PublisherState::Live) {
    return fail<PropagationOutcome>(ErrorCode::NotReady, "publisher not live");
  }

  EngineContext ctx;
  ctx.topology = topology_.get();
  ctx.policy = policy_.get();
  ctx.ledger = &ledger_;
  ctx.fences = &fences_;
  ctx.live_epoch = epoch_;

  RecoveryRequest request;
  request.id = PropagationId(attempts_.fetch_add(1));
  request.source = source;
  request.origin = origin;
  request.origin_generation = origin_generation;
  request.topology_digest = topology_->digest();
  request.epoch = epoch_;
  request.now = now;
  request.residual = residual;
  request.authority = authority;
  return Result<PropagationOutcome>(PropagationEngine::recover(ctx, request));
}

Status Fabric::journal_compaction_locked(Tick now) {
  (void)now;
  if (store_ == nullptr) {
    return Status::success();
  }
  (void)store_->compact_if_needed(config_.journal_compaction_threshold);
  return Status::success();
}

FabricStatus Fabric::status() const {
  std::unique_lock lock(mutex_);
  FabricStatus out;
  out.epoch = epoch_;
  out.previous_epoch = previous_epoch_;
  out.incarnation = incarnation_;
  out.durable = store_ != nullptr;
  out.has_topology = topology_ != nullptr;
  out.has_policy = policy_ != nullptr;
  if (topology_ != nullptr) {
    out.topology_digest = topology_->digest();
    out.topology_generation = topology_->generation();
  }
  if (policy_ != nullptr) {
    out.policy_id = policy_->id;
    out.policy_generation = policy_->generation;
  }
  out.publishers = publishers_.size();
  for (const auto& pair : publishers_) {
    if (pair.second.state == PublisherState::Live) {
      ++out.live_publishers;
    }
  }
  out.fences = fences_.size();
  out.revalidation_required = revalidation_.size();
  out.lineage_evictions = ledger_.lineage_evictions();
  if (store_ != nullptr) {
    out.recovered_lineages = store_->state().lineages.size();
    out.ambiguous_attempts = store_->state().attempts_ambiguous;
    out.corrupt_records = store_->state().corrupt_records;
  }
  out.signals_accepted = signals_accepted_;
  out.signals_rejected = signals_rejected_;
  out.topologies_installed = topologies_installed_;
  out.policies_installed = policies_installed_;
  out.shutting_down = shutting_down_.load();
  out.restored_liveness = false;
  return out;
}

std::vector<PublisherRecord> Fabric::publishers() const {
  std::unique_lock lock(mutex_);
  std::vector<PublisherRecord> out;
  out.reserve(publishers_.size());
  for (const auto& pair : publishers_) {
    out.push_back(pair.second);
  }
  std::sort(out.begin(), out.end(),
            [](const PublisherRecord& a, const PublisherRecord& b) { return a.source < b.source; });
  return out;
}

std::shared_ptr<const Topology> Fabric::topology() const {
  std::unique_lock lock(mutex_);
  return topology_;
}

std::shared_ptr<const PropagationPolicy> Fabric::policy() const {
  std::unique_lock lock(mutex_);
  return policy_;
}

Epoch Fabric::epoch() const {
  std::unique_lock lock(mutex_);
  return epoch_;
}

// --- asynchronous publishing ----------------------------------------------

void Fabric::mark_ticket_running(TicketId id) {
  std::unique_lock lock(ticket_mutex_);
  const auto it = tickets_.find(id);
  if (it != tickets_.end()) {
    it->second.state = TicketState::Running;
  }
}

bool Fabric::ticket_cancelled(TicketId id) const {
  std::unique_lock lock(ticket_mutex_);
  const auto it = tickets_.find(id);
  if (it == tickets_.end()) {
    return true;
  }
  return it->second.cancel_flag != nullptr && it->second.cancel_flag->load();
}

void Fabric::complete_ticket(TicketId id, PublishTicket ticket) {
  std::unique_lock lock(ticket_mutex_);
  ticket.id = id;
  tickets_[id] = std::move(ticket);
}

Result<TicketId> Fabric::publish_async(PressureSignal signal, Tick now) {
  if (shutting_down_.load()) {
    return fail<TicketId>(ErrorCode::ShuttingDown, "publish async");
  }
  if (!pool_.started()) {
    return fail<TicketId>(ErrorCode::NotReady, "worker pool not started");
  }
  const TicketId id = next_ticket_.fetch_add(1);
  {
    std::unique_lock lock(ticket_mutex_);
    if (tickets_.size() >= config_.max_tickets) {
      // Reclaim the oldest settled ticket before refusing.
      bool reclaimed = false;
      for (auto it = ticket_order_.begin(); it != ticket_order_.end(); ++it) {
        const auto found = tickets_.find(*it);
        if (found != tickets_.end() && found->second.state != TicketState::Pending &&
            found->second.state != TicketState::Running) {
          tickets_.erase(found);
          ticket_order_.erase(it);
          reclaimed = true;
          break;
        }
      }
      if (!reclaimed) {
        return fail<TicketId>(ErrorCode::LimitExceeded, "ticket store", tickets_.size());
      }
    }
    PublishTicket ticket;
    ticket.id = id;
    ticket.state = TicketState::Pending;
    ticket.cancel_flag = std::make_shared<std::atomic<bool>>(false);
    tickets_[id] = std::move(ticket);
    ticket_order_.push_back(id);
  }

  const Status submitted = pool_.submit([this, id, signal, now]() {
    mark_ticket_running(id);
    std::shared_ptr<std::atomic<bool>> flag;
    {
      std::unique_lock lock(ticket_mutex_);
      const auto it = tickets_.find(id);
      if (it != tickets_.end()) {
        flag = it->second.cancel_flag;
      }
    }
    if (flag == nullptr) {
      return;
    }
    if (flag->load()) {
      PublishTicket ticket;
      ticket.state = TicketState::Cancelled;
      ticket.status = Status::error(ErrorCode::Cancelled, "publish cancelled");
      ticket.cancelled = true;
      ticket.committed = false;
      ticket.cancel_flag = flag;
      complete_ticket(id, std::move(ticket));
      return;
    }
    PublishTicket ticket;
    ticket.cancel_flag = flag;
    Result<PropagationOutcome> result =
        fail<PropagationOutcome>(ErrorCode::Internal, "publish not attempted");
    {
      std::unique_lock lock(mutex_);
      result = publish_locked(signal, now, true, flag.get());
    }
    if (result.ok()) {
      ticket.outcome = std::move(result).value();
      ticket.status = ticket.outcome.status;
      if (ticket.status.code() == ErrorCode::Cancelled) {
        ticket.state = TicketState::Cancelled;
        ticket.cancelled = true;
        ticket.committed = false;
      } else if (ticket.status.ok()) {
        ticket.state = TicketState::Completed;
        ticket.committed = true;
      } else {
        ticket.state = TicketState::Failed;
        ticket.committed = ticket.outcome.counters.signals_accepted > 0;
      }
    } else {
      ticket.status = result.status();
      ticket.state = ticket.status.code() == ErrorCode::Cancelled ? TicketState::Cancelled
                                                                  : TicketState::Failed;
      ticket.cancelled = ticket.status.code() == ErrorCode::Cancelled;
    }
    complete_ticket(id, std::move(ticket));
  });

  if (!submitted.ok()) {
    std::unique_lock lock(ticket_mutex_);
    tickets_.erase(id);
    const auto it = std::find(ticket_order_.begin(), ticket_order_.end(), id);
    if (it != ticket_order_.end()) {
      ticket_order_.erase(it);
    }
    return fail<TicketId>(submitted.code(), submitted.context(), submitted.detail());
  }
  return Result<TicketId>(id);
}

Result<PublishTicket> Fabric::ticket(TicketId id) const {
  std::unique_lock lock(ticket_mutex_);
  const auto it = tickets_.find(id);
  if (it == tickets_.end()) {
    return fail<PublishTicket>(ErrorCode::NotFound, "ticket", id);
  }
  return Result<PublishTicket>(it->second);
}

Status Fabric::release_ticket(TicketId id) {
  std::unique_lock lock(ticket_mutex_);
  const auto it = tickets_.find(id);
  if (it == tickets_.end()) {
    return Status::error(ErrorCode::NotFound, "ticket", id);
  }
  if (it->second.state == TicketState::Pending || it->second.state == TicketState::Running) {
    return Status::error(ErrorCode::Conflict, "ticket still in flight", id);
  }
  tickets_.erase(it);
  const auto order = std::find(ticket_order_.begin(), ticket_order_.end(), id);
  if (order != ticket_order_.end()) {
    ticket_order_.erase(order);
  }
  return Status::success();
}

Status Fabric::cancel(TicketId id) {
  std::unique_lock lock(ticket_mutex_);
  const auto it = tickets_.find(id);
  if (it == tickets_.end()) {
    return Status::error(ErrorCode::NotFound, "ticket", id);
  }
  if (it->second.cancel_flag != nullptr) {
    it->second.cancel_flag->store(true);
  }
  if (it->second.state == TicketState::Pending) {
    it->second.state = TicketState::Cancelled;
    it->second.cancelled = true;
    it->second.status = Status::error(ErrorCode::Cancelled, "publish cancelled");
    it->second.committed = false;
  }
  return Status::success();
}

Status Fabric::drain() {
  if (!pool_.started()) {
    return Status::success();
  }
  return pool_.drain();
}

std::size_t Fabric::pending_tickets() const {
  std::unique_lock lock(ticket_mutex_);
  std::size_t pending = 0;
  for (const auto& pair : tickets_) {
    if (pair.second.state == TicketState::Pending || pair.second.state == TicketState::Running) {
      ++pending;
    }
  }
  return pending;
}

// --- lifecycle -------------------------------------------------------------

Status Fabric::flush_locked(Tick now) {
  if (store_ == nullptr) {
    return Status::success();
  }
  std::vector<std::pair<SourceId, ResourceId>> footprint;
  ledger_.for_each_application([&](const SourceResourceKey& key,
                                   const PropagationLedger::ApplicationRecord& record) {
    if (record.present && !record.magnitude.is_zero()) {
      footprint.emplace_back(key.source, key.resource);
    }
  });
  if (footprint.empty()) {
    return store_->sync();
  }
  const std::size_t per_record =
      (config_.journal_limits.max_record_bytes > 16u)
          ? (config_.journal_limits.max_record_bytes - 16u) / 16u
          : 1u;
  const std::size_t chunk = per_record == 0 ? 1u : per_record;
  for (std::size_t offset = 0; offset < footprint.size(); offset += chunk) {
    const std::size_t end = (offset + chunk < footprint.size()) ? offset + chunk : footprint.size();
    const std::span<const std::pair<SourceId, ResourceId>> slice(footprint.data() + offset,
                                                                 end - offset);
    const Status st = store_->record_pressure_footprint(slice, now);
    if (!st.ok()) {
      return st;
    }
  }
  return store_->sync();
}

Status Fabric::flush(Tick now) {
  std::unique_lock lock(mutex_);
  return flush_locked(now);
}

Status Fabric::shutdown(Tick now) {
  Status first_error = Status::success();
  if (shutting_down_.exchange(true)) {
    return Status::success();
  }

  // 1. Cancel every ticket that has not crossed its completion boundary.
  {
    std::unique_lock lock(ticket_mutex_);
    for (auto& pair : tickets_) {
      if (pair.second.cancel_flag != nullptr) {
        pair.second.cancel_flag->store(true);
      }
      if (pair.second.state == TicketState::Pending) {
        pair.second.state = TicketState::Cancelled;
        pair.second.cancelled = true;
        pair.second.committed = false;
        pair.second.status = Status::error(ErrorCode::Cancelled, "fabric shutdown");
      }
    }
  }

  // 2. Stop workers. Joining happens without holding any state lock.
  if (pool_.started()) {
    const Status st = pool_.abort();
    if (!st.ok() && first_error.ok()) {
      first_error = st;
    }
  }

  // 3. Persist the pressure footprint and retire every publisher.
  {
    std::unique_lock lock(mutex_);
    const Status st = flush_locked(now);
    if (!st.ok() && first_error.ok()) {
      first_error = st;
    }
    for (auto& pair : publishers_) {
      pair.second.state = PublisherState::Retired;
    }
    // Dynamic pressure is not durable: the in-memory ledger is returned to a
    // valid empty baseline, and the footprint above records what must be
    // re-observed rather than assumed.
    ledger_.clear();
  }
  return first_error;
}

}  // namespace backpressure