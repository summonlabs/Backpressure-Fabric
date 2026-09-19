#pragma once

// Backpressure Fabric - strong identities and name interning.
//
// Identities are distinct types, not interchangeable integers. Mixing a
// ResourceId with an EdgeId is a compile error, which is the whole point.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <compare>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

#include "backpressure/core/checked.hpp"
#include "backpressure/core/status.hpp"

namespace backpressure {

template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  static constexpr Rep kInvalid = std::numeric_limits<Rep>::max();

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId invalid() noexcept { return StrongId{}; }

  /// Build from an externally supplied integer (wire/persisted form).
  [[nodiscard]] static Result<StrongId> from_u64(std::uint64_t raw) {
    if (raw == static_cast<std::uint64_t>(kInvalid)) {
      return fail<StrongId>(ErrorCode::OutOfRange, "id sentinel", raw);
    }
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
      return fail<StrongId>(ErrorCode::OutOfRange, "id width", raw);
    }
    return Result<StrongId>(StrongId(static_cast<Rep>(raw)));
  }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != kInvalid; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(StrongId a, StrongId b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  Rep value_ = kInvalid;
};

struct ResourceTag;
struct EdgeTag;
struct SignalTag;
struct PropagationTag;
struct PolicyTag;
struct SourceTag;
struct FenceTag;
struct AttemptTag;
struct ObservationTag;

/// A network resource: a queue, port group, link, device or fabric endpoint
/// whose pressure the fabric is allowed to reason about.
using ResourceId = StrongId<ResourceTag>;
/// A directed dependency edge: pressure at c from propagates to c to.
using EdgeId = StrongId<EdgeTag>;
/// An authoritative pressure observation published by a source.
using SignalId = StrongId<SignalTag>;
/// One bounded propagation instance derived from exactly one signal.
using PropagationId = StrongId<PropagationTag>;
/// A named, versioned propagation policy.
using PolicyId = StrongId<PolicyTag>;
/// A pressure source (the authority that observed the pressure).
using SourceId = StrongId<SourceTag>;
/// A fence that forbids a specific effect at a specific resource.
using FenceId = StrongId<FenceTag>;
/// A single attempt to commit a durable mutation.
using AttemptId = StrongId<AttemptTag>;
/// A revalidation ticket for dynamic evidence after restart.
using ObservationId = StrongId<ObservationTag>;

/// Compact deterministic name interning. Symbols are stable for the lifetime of
/// the table and ordered by first interning, which keeps explanations
/// reproducible across runs that present names in the same order.
class SymbolTable {
 public:
  using Symbol = std::uint32_t;
  static constexpr Symbol kInvalidSymbol = 0xFFFFFFFFu;
  static constexpr std::size_t kDefaultCapacity = 1u << 20;

  SymbolTable() = default;
  SymbolTable(const SymbolTable&) = delete;
  SymbolTable& operator=(const SymbolTable&) = delete;
  SymbolTable(SymbolTable&&) = delete;
  SymbolTable& operator=(SymbolTable&&) = delete;
  ~SymbolTable() = default;

  [[nodiscard]] Result<Symbol> intern(std::string_view name,
                                      std::size_t capacity = kDefaultCapacity) {
    if (name.empty()) {
      return fail<Symbol>(ErrorCode::InvalidArgument, "empty symbol");
    }
    if (name.size() > kMaxNameBytes) {
      return fail<Symbol>(ErrorCode::OversizedInput, "symbol length", name.size());
    }
    auto it = index_.find(name);
    if (it != index_.end()) {
      return Result<Symbol>(it->second);
    }
    if (names_.size() >= capacity) {
      return fail<Symbol>(ErrorCode::LimitExceeded, "symbol capacity", names_.size());
    }
    const Symbol next = static_cast<Symbol>(names_.size());
    names_.emplace_back(name);
    // Key the index on the stable storage owned by the deque.
    index_.emplace(std::string_view(names_.back()), next);
    return Result<Symbol>(next);
  }

  [[nodiscard]] Result<Symbol> find(std::string_view name) const {
    auto it = index_.find(name);
    if (it == index_.end()) {
      return fail<Symbol>(ErrorCode::NotFound, "symbol");
    }
    return Result<Symbol>(it->second);
  }

  [[nodiscard]] std::string_view lookup(Symbol symbol) const noexcept {
    if (symbol >= names_.size()) {
      return {};
    }
    return std::string_view(names_[static_cast<std::size_t>(symbol)]);
  }

  [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }

  static constexpr std::size_t kMaxNameBytes = 255;

 private:
  std::deque<std::string> names_;
  std::unordered_map<std::string_view, Symbol> index_;
};

}  // namespace backpressure

namespace std {

template <class Tag, class Rep>
struct hash<backpressure::StrongId<Tag, Rep>> {
  [[nodiscard]] std::size_t operator()(const backpressure::StrongId<Tag, Rep>& id) const noexcept {
    return std::hash<Rep>{}(id.value());
  }
};

}  // namespace std
