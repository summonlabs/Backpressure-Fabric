#pragma once

// Backpressure Fabric - resources and protection classes.
//
// A resource is a place pressure can be observed at or applied to. It owns no
// queue, performs no admission and enforces no rate: it is the identity that
// propagation authority is expressed against.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/generation.hpp"

namespace backpressure {

enum class ResourceClass : std::uint8_t {
  Unknown = 0,
  Port,
  QueueGroup,
  Link,
  Device,
  FabricEndpoint,
  VirtualChannel,
  Count,
};

[[nodiscard]] constexpr const char* to_string(ResourceClass c) noexcept {
  switch (c) {
    case ResourceClass::Unknown: return "Unknown";
    case ResourceClass::Port: return "Port";
    case ResourceClass::QueueGroup: return "QueueGroup";
    case ResourceClass::Link: return "Link";
    case ResourceClass::Device: return "Device";
    case ResourceClass::FabricEndpoint: return "FabricEndpoint";
    case ResourceClass::VirtualChannel: return "VirtualChannel";
    case ResourceClass::Count: break;
  }
  return "Invalid";
}

/// Protection classes are hard boundaries. They are evaluated before damping and
/// before hop budgets, because a boundary is a scope statement, not a budget.
enum class ProtectionClass : std::uint8_t {
  /// Ordinary resource.
  None = 0,
  /// Pressure may be observed and reported here but never applied.
  ObservedOnly,
  /// Pressure may be applied here but must never travel further from here.
  Barrier,
  /// Pressure may neither be applied here nor travel through here.
  Sealed,
  Count,
};

[[nodiscard]] constexpr const char* to_string(ProtectionClass c) noexcept {
  switch (c) {
    case ProtectionClass::None: return "None";
    case ProtectionClass::ObservedOnly: return "ObservedOnly";
    case ProtectionClass::Barrier: return "Barrier";
    case ProtectionClass::Sealed: return "Sealed";
    case ProtectionClass::Count: break;
  }
  return "Invalid";
}

struct Resource {
  ResourceId id{};
  /// Interned name symbol; may be invalid when the resource was declared unnamed.
  SymbolTable::Symbol name = SymbolTable::kInvalidSymbol;
  ResourceClass klass = ResourceClass::Unknown;
  ProtectionClass protection = ProtectionClass::None;
  Generation definition_generation{};
  /// Operator opt-out: a resource may declare that it accepts no propagation.
  bool accepts_propagation = true;
  /// Digest of the declaration that produced this resource (provenance).
  Digest128 provenance{};

  /// May propagated pressure be applied here?
  [[nodiscard]] constexpr bool can_receive() const noexcept {
    return accepts_propagation && protection != ProtectionClass::Sealed &&
           protection != ProtectionClass::ObservedOnly;
  }

  /// May pressure that arrived here continue to this resource's dependencies?
  [[nodiscard]] constexpr bool can_forward() const noexcept {
    return can_receive() && protection != ProtectionClass::Barrier;
  }

  [[nodiscard]] Status validate() const {
    if (!id.valid()) {
      return Status::error(ErrorCode::InvalidArgument, "resource id");
    }
    if (klass >= ResourceClass::Count) {
      return Status::error(ErrorCode::InvalidArgument, "resource class",
                           static_cast<std::uint64_t>(klass));
    }
    if (protection >= ProtectionClass::Count) {
      return Status::error(ErrorCode::InvalidArgument, "protection class",
                           static_cast<std::uint64_t>(protection));
    }
    if (!definition_generation.known()) {
      return Status::error(ErrorCode::StaleGeneration, "resource definition generation unbound",
                           id.value());
    }
    return Status::success();
  }

  void digest_into(DigestBuilder& b) const noexcept {
    b.domain(0x30u);
    b.update_u64(id.value());
    b.update_u32(name);
    b.update_u8(static_cast<std::uint8_t>(klass));
    b.update_u8(static_cast<std::uint8_t>(protection));
    digest_generation(b, definition_generation);
    b.update_bool(accepts_propagation);
    b.update_digest(provenance);
  }
};

}  // namespace backpressure
