// Backpressure Fabric - pressure signal implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/model/signal.hpp"

#include <limits>

namespace backpressure {
namespace {

void digest_hop(DigestBuilder& b, const ProvenanceHop& h) noexcept {
  b.domain(0x71u);
  digest_incarnation(b, h.publisher);
  digest_epoch(b, h.epoch);
  digest_generation(b, h.generation);
  b.update_u64(h.tick);
  b.update_u64(h.propagation.value());
  b.update_digest(h.parent);
}

}  // namespace

void Provenance::push(const ProvenanceHop& hop) noexcept {
  if (count < kMaxChain) {
    hops[count] = hop;
    ++count;
    return;
  }
  DigestBuilder b;
  b.domain(0x72u);
  b.update_digest(dropped_prefix);
  b.update_u64(evictions);
  digest_hop(b, hops[0]);
  dropped_prefix = b.finish();
  for (std::size_t i = 1; i < kMaxChain; ++i) {
    hops[i - 1u] = hops[i];
  }
  hops[kMaxChain - 1u] = hop;
  ++evictions;
}

const ProvenanceHop& Provenance::latest() const noexcept { return hops[count == 0u ? 0u : count - 1u]; }

Digest128 Provenance::chain_digest() const noexcept {
  DigestBuilder b;
  b.domain(0x73u);
  b.update_digest(dropped_prefix);
  b.update_u64(evictions);
  b.update_u32(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    digest_hop(b, hops[i]);
  }
  return b.finish();
}

Digest128 PressureSignal::compute_lineage() const noexcept {
  DigestBuilder b;
  b.domain(0x74u);
  b.update_u64(id.value());
  b.update_u64(source.value());
  b.update_u64(origin.value());
  b.update_u8(static_cast<std::uint8_t>(observation.severity()));
  b.update_u32(observation.magnitude().raw());
  b.update_bool(observation.has_evidence());
  b.update_bool(observation.severity_downgraded());
  digest_generation(b, origin_generation);
  b.update_digest(topology_digest);
  digest_epoch(b, epoch);
  b.update_u64(issued_at);
  b.update_u64(valid_until);
  b.update_u32(source_priority);
  b.update_u32(weight_q16);
  b.update_digest(authority.digest());
  b.update_digest(provenance.chain_digest());
  return b.finish();
}

Digest128 PressureSignal::bind_lineage() noexcept {
  lineage = compute_lineage();
  return lineage;
}

Tick PressureSignal::deadline(const PropagationPolicy& policy) const noexcept {
  if (valid_until != kNoTick) {
    return valid_until;
  }
  if (issued_at > std::numeric_limits<Tick>::max() - policy.lifetime_ticks) {
    return std::numeric_limits<Tick>::max();
  }
  return issued_at + policy.lifetime_ticks;
}

bool PressureSignal::is_expired(Tick now, const PropagationPolicy& policy) const noexcept {
  return now > deadline(policy);
}

Status PressureSignal::validate(const PropagationPolicy& policy, Tick now) const {
  BPFAB_TRY(policy.validate());
  if (!id.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "signal id");
  }
  if (!source.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "signal source");
  }
  if (!origin.valid()) {
    return Status::error(ErrorCode::InvalidArgument, "signal origin");
  }
  if (!origin_generation.known()) {
    return Status::error(ErrorCode::StaleGeneration, "signal origin generation unbound");
  }
  if (!epoch.known()) {
    return Status::error(ErrorCode::StaleEpoch, "signal epoch unbound");
  }
  if (topology_digest.is_zero()) {
    return Status::error(ErrorCode::StaleGeneration, "signal topology unbound");
  }
  if (!publisher.valid()) {
    return Status::error(ErrorCode::StaleIncarnation, "signal publisher unbound");
  }
  if (provenance.empty()) {
    return Status::error(ErrorCode::InvalidArgument, "signal provenance empty");
  }
  if (!observation.can_authorize_propagation()) {
    return Status::error(ErrorCode::UnknownPressure, "signal observation unknown");
  }
  BPFAB_TRY(authority.validate());
  if (authority.epoch != epoch) {
    return Status::error(ErrorCode::StaleEpoch, "authority signal epoch mismatch",
                         authority.epoch.value());
  }
  if (authority.policy_generation != policy.generation) {
    return Status::error(ErrorCode::StaleGeneration, "authority policy generation mismatch",
                         authority.policy_generation.value());
  }
  if (!authority.permits_propagation()) {
    return Status::error(ErrorCode::Unauthorized, "authority permits no propagation",
                         static_cast<std::uint64_t>(authority.level));
  }
  if (weight_q16 > 65536u) {
    return Status::error(ErrorCode::OutOfRange, "signal weight", weight_q16);
  }
  if (lineage.is_zero()) {
    return Status::error(ErrorCode::InvalidArgument, "signal lineage unbound");
  }
  if (is_expired(now, policy)) {
    return Status::error(ErrorCode::Expired, "signal expired", deadline(policy));
  }
  return Status::success();
}

}  // namespace backpressure
