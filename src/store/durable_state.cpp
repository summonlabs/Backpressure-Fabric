// Backpressure Fabric - durable state implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/store/durable_state.hpp"

#include <algorithm>
#include <filesystem>

#include "backpressure/core/byteio.hpp"

namespace backpressure {
namespace {

constexpr std::uint16_t kPolicyFormat = 1;
constexpr std::uint16_t kFenceFormat = 1;
constexpr std::size_t kMaxPolicyBytes = 512;
constexpr std::size_t kMaxFenceBytes = 256;
constexpr std::size_t kMaxJournalPayload = 1u << 12;

}  // namespace

Status serialize_policy(const PropagationPolicy& policy, std::vector<std::byte>& out,
                        std::size_t max_bytes) {
  const std::size_t bound = max_bytes < kMaxPolicyBytes ? max_bytes : kMaxPolicyBytes;
  ByteWriter writer(bound);
  BPFAB_TRY(writer.u16(kPolicyFormat));
  BPFAB_TRY(writer.u64(policy.id.value()));
  BPFAB_TRY(writer.u64(policy.generation.value()));
  BPFAB_TRY(writer.u32(policy.max_hops));
  BPFAB_TRY(writer.u32(policy.max_fanout));
  BPFAB_TRY(writer.u32(policy.max_expansions));
  BPFAB_TRY(writer.u32(policy.max_visited));
  BPFAB_TRY(writer.u32(policy.max_sources));
  BPFAB_TRY(writer.u32(policy.max_records));
  BPFAB_TRY(writer.u32(policy.max_path_records));
  BPFAB_TRY(writer.u32(policy.min_propagatable.raw()));
  BPFAB_TRY(writer.u64(policy.lifetime_ticks));
  BPFAB_TRY(writer.u8(static_cast<std::uint8_t>(policy.aggregation)));
  BPFAB_TRY(writer.u32(policy.aggregation_ceiling.raw()));
  BPFAB_TRY(writer.boolean(policy.allow_amplification));
  BPFAB_TRY(writer.u32(policy.max_cumulative_gain.raw()));
  BPFAB_TRY(writer.u64(policy.default_cooldown_ticks));
  BPFAB_TRY(writer.u32(policy.hysteresis_delta.raw()));
  BPFAB_TRY(writer.u32(policy.recovery_decay.per_step.raw()));
  BPFAB_TRY(writer.u64(policy.recovery_decay.step_ticks));
  BPFAB_TRY(writer.u32(policy.recovery_decay.floor.raw()));
  BPFAB_TRY(writer.u32(policy.recovery_decay.max_steps));
  BPFAB_TRY(writer.u32(policy.thresholds.elevated_milli));
  BPFAB_TRY(writer.u32(policy.thresholds.high_milli));
  BPFAB_TRY(writer.u32(policy.thresholds.critical_milli));
  BPFAB_TRY(writer.u32(policy.thresholds.exhausted_milli));
  BPFAB_TRY(writer.boolean(policy.require_known_pressure));
  out = writer.take();
  return Status::success();
}

Result<PropagationPolicy> deserialize_policy(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  BPFAB_TRY_DECL(const std::uint16_t, format, reader.u16());
  if (format != kPolicyFormat) {
    return fail<PropagationPolicy>(ErrorCode::VersionMismatch, "policy format", format);
  }
  PropagationPolicy policy;
  BPFAB_TRY_DECL(const std::uint64_t, raw_id, reader.u64());
  BPFAB_TRY_ASSIGN(policy.id, PolicyId::from_u64(raw_id));
  BPFAB_TRY_DECL(const std::uint64_t, raw_generation, reader.u64());
  BPFAB_TRY_ASSIGN(policy.generation, Generation::from_u64(raw_generation));
  BPFAB_TRY_ASSIGN(policy.max_hops, reader.u32());
  BPFAB_TRY_ASSIGN(policy.max_fanout, reader.u32());
  BPFAB_TRY_ASSIGN(policy.max_expansions, reader.u32());
  BPFAB_TRY_ASSIGN(policy.max_visited, reader.u32());
  BPFAB_TRY_ASSIGN(policy.max_sources, reader.u32());
  BPFAB_TRY_ASSIGN(policy.max_records, reader.u32());
  BPFAB_TRY_ASSIGN(policy.max_path_records, reader.u32());
  BPFAB_TRY_DECL(const std::uint32_t, floor_raw, reader.u32());
  policy.min_propagatable = Magnitude::from_raw_q16_saturating(floor_raw);
  if (floor_raw > Magnitude::full().raw()) {
    return fail<PropagationPolicy>(ErrorCode::OutOfRange, "policy floor", floor_raw);
  }
  BPFAB_TRY_ASSIGN(policy.lifetime_ticks, reader.u64());
  BPFAB_TRY_DECL(const std::uint8_t, rule, reader.u8());
  if (rule >= static_cast<std::uint8_t>(AggregationRule::Count)) {
    return fail<PropagationPolicy>(ErrorCode::MalformedInput, "policy aggregation", rule);
  }
  policy.aggregation = static_cast<AggregationRule>(rule);
  BPFAB_TRY_DECL(const std::uint32_t, ceiling_raw, reader.u32());
  if (ceiling_raw > Magnitude::full().raw()) {
    return fail<PropagationPolicy>(ErrorCode::OutOfRange, "policy ceiling", ceiling_raw);
  }
  policy.aggregation_ceiling = Magnitude::from_raw_q16_saturating(ceiling_raw);
  BPFAB_TRY_ASSIGN(policy.allow_amplification, reader.boolean());
  BPFAB_TRY_DECL(const std::uint32_t, gain_raw, reader.u32());
  BPFAB_TRY_ASSIGN(policy.max_cumulative_gain, Gain::from_raw(gain_raw));
  BPFAB_TRY_ASSIGN(policy.default_cooldown_ticks, reader.u64());
  BPFAB_TRY_DECL(const std::uint32_t, hysteresis_raw, reader.u32());
  if (hysteresis_raw > Magnitude::full().raw()) {
    return fail<PropagationPolicy>(ErrorCode::OutOfRange, "policy hysteresis", hysteresis_raw);
  }
  policy.hysteresis_delta = Magnitude::from_raw_q16_saturating(hysteresis_raw);
  BPFAB_TRY_DECL(const std::uint32_t, decay_raw, reader.u32());
  policy.recovery_decay.per_step = Q16::from_raw_saturating(decay_raw);
  if (decay_raw > Q16::kOne) {
    return fail<PropagationPolicy>(ErrorCode::OutOfRange, "policy decay", decay_raw);
  }
  BPFAB_TRY_ASSIGN(policy.recovery_decay.step_ticks, reader.u64());
  BPFAB_TRY_DECL(const std::uint32_t, decay_floor_raw, reader.u32());
  if (decay_floor_raw > Magnitude::full().raw()) {
    return fail<PropagationPolicy>(ErrorCode::OutOfRange, "policy decay floor", decay_floor_raw);
  }
  policy.recovery_decay.floor = Magnitude::from_raw_q16_saturating(decay_floor_raw);
  BPFAB_TRY_ASSIGN(policy.recovery_decay.max_steps, reader.u32());
  BPFAB_TRY_ASSIGN(policy.thresholds.elevated_milli, reader.u32());
  BPFAB_TRY_ASSIGN(policy.thresholds.high_milli, reader.u32());
  BPFAB_TRY_ASSIGN(policy.thresholds.critical_milli, reader.u32());
  BPFAB_TRY_ASSIGN(policy.thresholds.exhausted_milli, reader.u32());
  BPFAB_TRY_ASSIGN(policy.require_known_pressure, reader.boolean());
  BPFAB_TRY(reader.expect_end());
  const Status valid = policy.validate();
  if (!valid.ok()) {
    return fail<PropagationPolicy>(valid.code(), valid.context(), valid.detail());
  }
  return Result<PropagationPolicy>(policy);
}

Status serialize_fence(const Fence& fence, std::vector<std::byte>& out) {
  ByteWriter writer(kMaxFenceBytes);
  BPFAB_TRY(writer.u16(kFenceFormat));
  BPFAB_TRY(writer.u64(fence.id.value()));
  BPFAB_TRY(writer.u64(fence.resource.value()));
  BPFAB_TRY(writer.u8(static_cast<std::uint8_t>(fence.kind)));
  BPFAB_TRY(writer.u64(fence.epoch.value()));
  BPFAB_TRY(writer.u64(fence.dependency_generation.value()));
  BPFAB_TRY(writer.u64(fence.expires_at));
  BPFAB_TRY(writer.boolean(fence.durable));
  BPFAB_TRY(writer.digest(fence.provenance));
  out = writer.take();
  return Status::success();
}

Result<Fence> deserialize_fence(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  BPFAB_TRY_DECL(const std::uint16_t, format, reader.u16());
  if (format != kFenceFormat) {
    return fail<Fence>(ErrorCode::VersionMismatch, "fence format", format);
  }
  Fence fence;
  BPFAB_TRY_DECL(const std::uint64_t, raw_id, reader.u64());
  BPFAB_TRY_ASSIGN(fence.id, FenceId::from_u64(raw_id));
  BPFAB_TRY_DECL(const std::uint64_t, raw_resource, reader.u64());
  BPFAB_TRY_ASSIGN(fence.resource, ResourceId::from_u64(raw_resource));
  BPFAB_TRY_DECL(const std::uint8_t, kind, reader.u8());
  if (kind >= static_cast<std::uint8_t>(FenceKind::Count)) {
    return fail<Fence>(ErrorCode::MalformedInput, "fence kind", kind);
  }
  fence.kind = static_cast<FenceKind>(kind);
  BPFAB_TRY_DECL(const std::uint64_t, raw_epoch, reader.u64());
  BPFAB_TRY_ASSIGN(fence.epoch, Epoch::from_u64(raw_epoch));
  BPFAB_TRY_DECL(const std::uint64_t, raw_generation, reader.u64());
  BPFAB_TRY_ASSIGN(fence.dependency_generation, Generation::from_u64(raw_generation));
  BPFAB_TRY_ASSIGN(fence.expires_at, reader.u64());
  BPFAB_TRY_ASSIGN(fence.durable, reader.boolean());
  BPFAB_TRY_ASSIGN(fence.provenance, reader.digest());
  BPFAB_TRY(reader.expect_end());
  BPFAB_TRY(fence.validate());
  return Result<Fence>(fence);
}

Result<DurableStore> DurableStore::open(const DurableConfig& config) {
  if (config.directory.empty()) {
    return fail<DurableStore>(ErrorCode::InvalidArgument, "durable directory");
  }
  if (config.journal_limits.max_record_bytes < kMaxJournalPayload) {
    return fail<DurableStore>(ErrorCode::InvalidArgument, "journal record bound below payload bound");
  }
  BPFAB_TRY(fsutil::ensure_directory(config.directory));

  DurableStore store;
  store.config_ = config;
  store.state_.stale_temp_files_removed = fsutil::remove_stale_temp_files(config.directory);

  // One writer per state directory, enforced by the operating system.
  const std::filesystem::path lock_path =
      std::filesystem::path(config.directory) / std::filesystem::path("fabric.lock");
  Result<fsutil::ExclusiveLock> acquired =
      fsutil::ExclusiveLock::acquire(lock_path.string());
  if (!acquired.ok()) {
    return fail<DurableStore>(acquired.status().code(), acquired.status().context());
  }
  store.lock_ = std::move(acquired).value();

  const std::filesystem::path path =
      std::filesystem::path(config.directory) / std::filesystem::path(config.journal_name);
  Result<Journal> opened = Journal::open(path.string(), config.journal_limits, true);
  if (!opened.ok()) {
    return fail<DurableStore>(opened.status().code(), opened.status().context());
  }
  store.journal_ = std::move(opened).value();
  BPFAB_TRY(store.replay());
  return Result<DurableStore>(std::move(store));
}

Status DurableStore::replay() {
  for (const JournalRecord& record : journal_.records()) {
    BPFAB_TRY(apply_record(record));
    ++state_.records_replayed;
  }
  state_.corrupt_records = journal_.stats().truncated_tail_records;
  state_.journal_repaired = journal_.stats().repaired;
  state_.attempts_open = open_attempts_.size();
  state_.attempts_ambiguous = open_attempts_.size();
  return Status::success();
}

Status DurableStore::remember_lineage(Digest128 lineage) {
  for (const Digest128 existing : state_.lineages) {
    if (existing == lineage) {
      return Status::success();
    }
  }
  if (state_.lineages.size() >= config_.max_lineages) {
    state_.lineages.pop_front();
    ++state_.lineage_evictions;
  }
  state_.lineages.push_back(lineage);
  return Status::success();
}

Status DurableStore::apply_record(const JournalRecord& record) {
  ByteReader reader(record.payload);
  switch (record.type) {
    case RecordType::Epoch: {
      state_.previous_epoch = state_.epoch;
      state_.epoch = record.epoch;
      return Status::success();
    }
    case RecordType::Boot: {
      BPFAB_TRY_DECL(const std::uint64_t, hi, reader.u64());
      BPFAB_TRY_DECL(const std::uint64_t, lo, reader.u64());
      BPFAB_TRY_DECL(const std::uint32_t, pid, reader.u32());
      BPFAB_TRY_DECL(const std::uint32_t, index, reader.u32());
      BPFAB_TRY_DECL(const std::uint64_t, seq, reader.u64());
      state_.boot = BootId::from_raw(hi, lo);
      state_.incarnation = Incarnation{state_.boot, pid, index, seq};
      state_.boot_recorded = true;
      return reader.expect_end();
    }
    case RecordType::Policy: {
      BPFAB_TRY_DECL(PropagationPolicy, policy, deserialize_policy(record.payload));
      state_.policy = policy;
      state_.has_policy = true;
      return Status::success();
    }
    case RecordType::TopologyBinding: {
      BPFAB_TRY_ASSIGN(state_.topology_digest, reader.digest());
      BPFAB_TRY_DECL(const std::uint64_t, generation, reader.u64());
      BPFAB_TRY_ASSIGN(state_.topology_generation, Generation::from_u64(generation));
      state_.has_topology_binding = true;
      return reader.expect_end();
    }
    case RecordType::FencePut: {
      BPFAB_TRY_DECL(Fence, fence, deserialize_fence(record.payload));
      bool replaced = false;
      for (Fence& existing : state_.fences) {
        if (existing.id == fence.id) {
          existing = fence;
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        if (state_.fences.size() >= FenceTable::kMaxFences) {
          return Status::error(ErrorCode::LimitExceeded, "recovered fences",
                               state_.fences.size());
        }
        state_.fences.push_back(fence);
      }
      return Status::success();
    }
    case RecordType::FenceRemove: {
      BPFAB_TRY_DECL(const std::uint64_t, raw_id, reader.u64());
      BPFAB_TRY_DECL(const FenceId, id, FenceId::from_u64(raw_id));
      const auto it = std::find_if(state_.fences.begin(), state_.fences.end(),
                                   [id](const Fence& f) { return f.id == id; });
      if (it == state_.fences.end()) {
        return Status::error(ErrorCode::NotFound, "fence removal", raw_id);
      }
      state_.fences.erase(it);
      return reader.expect_end();
    }
    case RecordType::Lineage: {
      BPFAB_TRY_DECL(Digest128, lineage, reader.digest());
      return remember_lineage(lineage);
    }
    case RecordType::Revalidation: {
      BPFAB_TRY_DECL(const std::uint8_t, op, reader.u8());
      if (op == kRevalidationRequest || op == kRevalidationClear) {
        BPFAB_TRY_DECL(const std::uint64_t, raw_resource, reader.u64());
        BPFAB_TRY_DECL(const ResourceId, resource, ResourceId::from_u64(raw_resource));
        const auto it = std::find(state_.revalidation_required.begin(),
                                  state_.revalidation_required.end(), resource);
        if (op == kRevalidationRequest) {
          if (it == state_.revalidation_required.end()) {
            if (state_.revalidation_required.size() >= config_.max_footprint_entries) {
              ++state_.footprint_overflow;
              return Status::success();
            }
            state_.revalidation_required.push_back(resource);
          }
        } else if (it != state_.revalidation_required.end()) {
          state_.revalidation_required.erase(it);
        }
        return reader.expect_end();
      }
      if (op == kRevalidationFootprint) {
        BPFAB_TRY_DECL(const std::uint32_t, count, reader.u32());
        if (static_cast<std::size_t>(count) > config_.max_footprint_entries) {
          return Status::error(ErrorCode::OversizedInput, "footprint entries", count);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          BPFAB_TRY_DECL(const std::uint64_t, raw_source, reader.u64());
          BPFAB_TRY_DECL(const std::uint64_t, raw_resource, reader.u64());
          BPFAB_TRY_DECL(const SourceId, source, SourceId::from_u64(raw_source));
          (void)source;
          BPFAB_TRY_DECL(const ResourceId, resource, ResourceId::from_u64(raw_resource));
          if (state_.revalidation_required.size() >= config_.max_footprint_entries) {
            ++state_.footprint_overflow;
            continue;
          }
          if (std::find(state_.revalidation_required.begin(),
                        state_.revalidation_required.end(),
                        resource) == state_.revalidation_required.end()) {
            state_.revalidation_required.push_back(resource);
          }
        }
        state_.footprint_entries = state_.revalidation_required.size();
        return reader.expect_end();
      }
      return Status::error(ErrorCode::MalformedInput, "revalidation op", op);
    }
    case RecordType::AttemptBegin: {
      BPFAB_TRY_DECL(const std::uint64_t, raw_id, reader.u64());
      BPFAB_TRY_DECL(const AttemptId, id, AttemptId::from_u64(raw_id));
      if (std::find(open_attempts_.begin(), open_attempts_.end(), id) != open_attempts_.end()) {
        return Status::error(ErrorCode::Duplicate, "attempt begin", raw_id);
      }
      open_attempts_.push_back(id);
      return reader.expect_end();
    }
    case RecordType::AttemptCommit: {
      BPFAB_TRY_DECL(const std::uint64_t, raw_id, reader.u64());
      BPFAB_TRY_DECL(const AttemptId, id, AttemptId::from_u64(raw_id));
      const auto it = std::find(open_attempts_.begin(), open_attempts_.end(), id);
      if (it == open_attempts_.end()) {
        return Status::error(ErrorCode::NotFound, "attempt commit", raw_id);
      }
      open_attempts_.erase(it);
      ++state_.attempts_committed;
      ++attempts_committed_seen_;
      return reader.expect_end();
    }
    case RecordType::AttemptAbort: {
      BPFAB_TRY_DECL(const std::uint64_t, raw_id, reader.u64());
      BPFAB_TRY_DECL(const AttemptId, id, AttemptId::from_u64(raw_id));
      const auto it = std::find(open_attempts_.begin(), open_attempts_.end(), id);
      if (it == open_attempts_.end()) {
        return Status::error(ErrorCode::NotFound, "attempt abort", raw_id);
      }
      open_attempts_.erase(it);
      ++state_.attempts_aborted;
      return reader.expect_end();
    }
    case RecordType::Compaction: {
      BPFAB_TRY_DECL(const std::uint64_t, previous_epoch, reader.u64());
      state_.previous_epoch = Epoch::from_raw(previous_epoch);
      return reader.expect_end();
    }
    case RecordType::Count:
      break;
  }
  return Status::error(ErrorCode::MalformedInput, "record type",
                       static_cast<std::uint64_t>(record.type));
}

Status DurableStore::establish_epoch(Tick now) {
  Epoch next = Epoch::none();
  if (!state_.epoch.known()) {
    next = Epoch(1);
  } else {
    BPFAB_TRY_ASSIGN(next, state_.epoch.next());
  }
  std::vector<std::byte> payload;
  ByteWriter writer(32);
  BPFAB_TRY(writer.u64(next.value()));
  payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::Epoch, now, next,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  state_.previous_epoch = state_.epoch;
  state_.epoch = next;
  return Status::success();
}

Status DurableStore::record_boot(const Incarnation& incarnation, Tick now) {
  if (!incarnation.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "boot incarnation");
  }
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  ByteWriter writer(64);
  BPFAB_TRY(writer.u64(incarnation.boot.hi));
  BPFAB_TRY(writer.u64(incarnation.boot.lo));
  BPFAB_TRY(writer.u32(incarnation.pid));
  BPFAB_TRY(writer.u32(incarnation.index));
  BPFAB_TRY(writer.u64(incarnation.seq));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::Boot, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  state_.boot = incarnation.boot;
  state_.incarnation = incarnation;
  state_.boot_recorded = true;
  return Status::success();
}

Status DurableStore::persist_policy(const PropagationPolicy& policy, Tick now) {
  BPFAB_TRY(policy.validate());
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  if (state_.has_policy && state_.policy.id == policy.id &&
      policy.generation <= state_.policy.generation) {
    if (state_.policy.digest() == policy.digest()) {
      return Status::success();
    }
    return Status::error(ErrorCode::StaleGeneration, "policy generation",
                         policy.generation.value());
  }
  std::vector<std::byte> payload;
  BPFAB_TRY(serialize_policy(policy, payload, config_.journal_limits.max_record_bytes));
  BPFAB_TRY(journal_.append(RecordType::Policy, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  state_.policy = policy;
  state_.has_policy = true;
  return Status::success();
}

Status DurableStore::bind_topology(Digest128 digest, Generation generation, Tick now) {
  if (digest.is_zero() || !generation.known()) {
    return Status::error(ErrorCode::InvalidArgument, "topology binding");
  }
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  ByteWriter writer(32);
  BPFAB_TRY(writer.digest(digest));
  BPFAB_TRY(writer.u64(generation.value()));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::TopologyBinding, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  state_.topology_digest = digest;
  state_.topology_generation = generation;
  state_.has_topology_binding = true;
  return Status::success();
}

Status DurableStore::put_fence(const Fence& fence, Tick now) {
  BPFAB_TRY(fence.validate());
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  std::vector<std::byte> payload;
  BPFAB_TRY(serialize_fence(fence, payload));
  if (payload.size() > config_.journal_limits.max_record_bytes) {
    return Status::error(ErrorCode::OversizedInput, "fence record", payload.size());
  }
  BPFAB_TRY(journal_.append(RecordType::FencePut, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  bool replaced = false;
  for (Fence& existing : state_.fences) {
    if (existing.id == fence.id) {
      existing = fence;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    if (state_.fences.size() >= FenceTable::kMaxFences) {
      return Status::error(ErrorCode::LimitExceeded, "fence capacity", state_.fences.size());
    }
    state_.fences.push_back(fence);
  }
  return Status::success();
}

Status DurableStore::remove_fence(FenceId id, Tick now) {
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  const auto it = std::find_if(state_.fences.begin(), state_.fences.end(),
                               [id](const Fence& f) { return f.id == id; });
  if (it == state_.fences.end()) {
    return Status::error(ErrorCode::NotFound, "fence remove", id.value());
  }
  ByteWriter writer(16);
  BPFAB_TRY(writer.u64(id.value()));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::FenceRemove, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  const auto live = std::find_if(state_.fences.begin(), state_.fences.end(),
                                 [id](const Fence& f) { return f.id == id; });
  if (live != state_.fences.end()) {
    state_.fences.erase(live);
  }
  return Status::success();
}

Status DurableStore::record_lineage(Digest128 lineage, Tick now) {
  if (lineage.is_zero()) {
    return Status::error(ErrorCode::InvalidArgument, "lineage digest");
  }
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  ByteWriter writer(24);
  BPFAB_TRY(writer.digest(lineage));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::Lineage, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  return remember_lineage(lineage);
}

Status DurableStore::request_revalidation(ResourceId resource, Tick now) {
  if (!resource.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "revalidation resource");
  }
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  ByteWriter writer(16);
  BPFAB_TRY(writer.u8(kRevalidationRequest));
  BPFAB_TRY(writer.u64(resource.value()));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::Revalidation, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  if (std::find(state_.revalidation_required.begin(), state_.revalidation_required.end(),
                resource) == state_.revalidation_required.end()) {
    if (state_.revalidation_required.size() >= config_.max_footprint_entries) {
      ++state_.footprint_overflow;
      return Status::error(ErrorCode::LimitExceeded, "revalidation footprint",
                           state_.revalidation_required.size());
    }
    state_.revalidation_required.push_back(resource);
  }
  return Status::success();
}

Status DurableStore::clear_revalidation(ResourceId resource, Tick now) {
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  ByteWriter writer(16);
  BPFAB_TRY(writer.u8(kRevalidationClear));
  BPFAB_TRY(writer.u64(resource.value()));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::Revalidation, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  const auto it = std::find(state_.revalidation_required.begin(),
                            state_.revalidation_required.end(), resource);
  if (it != state_.revalidation_required.end()) {
    state_.revalidation_required.erase(it);
  }
  return Status::success();
}

Status DurableStore::record_pressure_footprint(
    std::span<const std::pair<SourceId, ResourceId>> entries, Tick now) {
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  if (entries.size() > config_.max_footprint_entries) {
    return Status::error(ErrorCode::OversizedInput, "footprint entries", entries.size());
  }
  const std::size_t needed = 5u + entries.size() * 16u;
  if (needed > config_.journal_limits.max_record_bytes) {
    return Status::error(ErrorCode::OversizedInput, "footprint record", needed);
  }
  ByteWriter writer(config_.journal_limits.max_record_bytes);
  BPFAB_TRY(writer.u8(kRevalidationFootprint));
  BPFAB_TRY(writer.u32(static_cast<std::uint32_t>(entries.size())));
  for (const auto& entry : entries) {
    BPFAB_TRY(writer.u64(entry.first.value()));
    BPFAB_TRY(writer.u64(entry.second.value()));
  }
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::Revalidation, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  for (const auto& entry : entries) {
    if (std::find(state_.revalidation_required.begin(), state_.revalidation_required.end(),
                  entry.second) == state_.revalidation_required.end()) {
      if (state_.revalidation_required.size() >= config_.max_footprint_entries) {
        ++state_.footprint_overflow;
        break;
      }
      state_.revalidation_required.push_back(entry.second);
    }
  }
  state_.footprint_entries = state_.revalidation_required.size();
  return Status::success();
}

Status DurableStore::begin_attempt(AttemptId id, Tick now) {
  if (!id.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "attempt id");
  }
  if (!state_.epoch.known()) {
    return Status::error(ErrorCode::NotReady, "epoch not established");
  }
  if (std::find(open_attempts_.begin(), open_attempts_.end(), id) != open_attempts_.end()) {
    return Status::error(ErrorCode::Duplicate, "attempt already open", id.value());
  }
  ByteWriter writer(16);
  BPFAB_TRY(writer.u64(id.value()));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::AttemptBegin, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  open_attempts_.push_back(id);
  state_.attempts_open = open_attempts_.size();
  return Status::success();
}

Status DurableStore::commit_attempt(AttemptId id, Tick now) {
  const auto it = std::find(open_attempts_.begin(), open_attempts_.end(), id);
  if (it == open_attempts_.end()) {
    return Status::error(ErrorCode::NotFound, "attempt commit", id.value());
  }
  ByteWriter writer(16);
  BPFAB_TRY(writer.u64(id.value()));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::AttemptCommit, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  const auto live = std::find(open_attempts_.begin(), open_attempts_.end(), id);
  if (live != open_attempts_.end()) {
    open_attempts_.erase(live);
  }
  ++state_.attempts_committed;
  state_.attempts_open = open_attempts_.size();
  state_.attempts_ambiguous = open_attempts_.size();
  return Status::success();
}

Status DurableStore::abort_attempt(AttemptId id, Tick now) {
  const auto it = std::find(open_attempts_.begin(), open_attempts_.end(), id);
  if (it == open_attempts_.end()) {
    return Status::error(ErrorCode::NotFound, "attempt abort", id.value());
  }
  ByteWriter writer(16);
  BPFAB_TRY(writer.u64(id.value()));
  const std::vector<std::byte> payload = writer.take();
  BPFAB_TRY(journal_.append(RecordType::AttemptAbort, now, state_.epoch,
                            std::span<const std::byte>(payload.data(), payload.size())));
  BPFAB_TRY(journal_.sync());
  const auto live = std::find(open_attempts_.begin(), open_attempts_.end(), id);
  if (live != open_attempts_.end()) {
    open_attempts_.erase(live);
  }
  ++state_.attempts_aborted;
  state_.attempts_open = open_attempts_.size();
  state_.attempts_ambiguous = open_attempts_.size();
  return Status::success();
}

Status DurableStore::sync() { return journal_.sync(); }

Status DurableStore::compact() {
  std::vector<JournalRecord> compacted;
  const Tick now = kNoTick;

  {
    // The marker is written last so that replay sees it after the epoch record
    // and can restore the epoch that preceded this compaction.
    JournalRecord marker;
    JournalRecord epoch_record;
    epoch_record.type = RecordType::Epoch;
    epoch_record.tick = now;
    epoch_record.epoch = state_.epoch;
    ByteWriter writer(16);
    BPFAB_TRY(writer.u64(state_.epoch.value()));
    epoch_record.payload = writer.take();
    compacted.push_back(std::move(epoch_record));
  }
  if (state_.boot_recorded) {
    JournalRecord boot_record;
    boot_record.type = RecordType::Boot;
    boot_record.tick = now;
    boot_record.epoch = state_.epoch;
    ByteWriter writer(64);
    BPFAB_TRY(writer.u64(state_.incarnation.boot.hi));
    BPFAB_TRY(writer.u64(state_.incarnation.boot.lo));
    BPFAB_TRY(writer.u32(state_.incarnation.pid));
    BPFAB_TRY(writer.u32(state_.incarnation.index));
    BPFAB_TRY(writer.u64(state_.incarnation.seq));
    boot_record.payload = writer.take();
    compacted.push_back(std::move(boot_record));
  }
  if (state_.has_policy) {
    JournalRecord policy_record;
    policy_record.type = RecordType::Policy;
    policy_record.tick = now;
    policy_record.epoch = state_.epoch;
    BPFAB_TRY(serialize_policy(state_.policy, policy_record.payload,
                               config_.journal_limits.max_record_bytes));
    compacted.push_back(std::move(policy_record));
  }
  if (state_.has_topology_binding) {
    JournalRecord topology_record;
    topology_record.type = RecordType::TopologyBinding;
    topology_record.tick = now;
    topology_record.epoch = state_.epoch;
    ByteWriter writer(32);
    BPFAB_TRY(writer.digest(state_.topology_digest));
    BPFAB_TRY(writer.u64(state_.topology_generation.value()));
    topology_record.payload = writer.take();
    compacted.push_back(std::move(topology_record));
  }
  for (const Fence& fence : state_.fences) {
    JournalRecord record;
    record.type = RecordType::FencePut;
    record.tick = now;
    record.epoch = state_.epoch;
    BPFAB_TRY(serialize_fence(fence, record.payload));
    compacted.push_back(std::move(record));
  }
  for (const ResourceId resource : state_.revalidation_required) {
    JournalRecord record;
    record.type = RecordType::Revalidation;
    record.tick = now;
    record.epoch = state_.epoch;
    ByteWriter writer(16);
    BPFAB_TRY(writer.u8(kRevalidationRequest));
    BPFAB_TRY(writer.u64(resource.value()));
    record.payload = writer.take();
    compacted.push_back(std::move(record));
  }
  for (const Digest128 lineage : state_.lineages) {
    JournalRecord record;
    record.type = RecordType::Lineage;
    record.tick = now;
    record.epoch = state_.epoch;
    ByteWriter writer(24);
    BPFAB_TRY(writer.digest(lineage));
    record.payload = writer.take();
    compacted.push_back(std::move(record));
  }
  for (const AttemptId id : open_attempts_) {
    JournalRecord record;
    record.type = RecordType::AttemptBegin;
    record.tick = now;
    record.epoch = state_.epoch;
    ByteWriter writer(16);
    BPFAB_TRY(writer.u64(id.value()));
    record.payload = writer.take();
    compacted.push_back(std::move(record));
  }
  {
    JournalRecord marker;
    marker.type = RecordType::Compaction;
    marker.tick = now;
    marker.epoch = state_.epoch;
    ByteWriter writer(16);
    BPFAB_TRY(writer.u64(state_.epoch.value()));
    marker.payload = writer.take();
    compacted.push_back(std::move(marker));
  }

  BPFAB_TRY(journal_.rewrite(compacted));
  state_.records_replayed = journal_.records().size();
  return Status::success();
}

std::size_t DurableStore::compact_if_needed(std::size_t record_threshold) {
  if (record_threshold == 0 || journal_.records().size() < record_threshold) {
    return 0;
  }
  return compact().ok() ? 1u : 0u;
}

}  // namespace backpressure