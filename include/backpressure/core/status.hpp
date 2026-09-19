#pragma once

// Backpressure Fabric - status codes and result plumbing.
//
// Every fallible operation returns a Status or a Result<T>. Status carries no
// allocation: a code, a bounded human readable context string and a bounded
// numeric detail. Result<T> never silently manufactures a value: the value is
// only reachable when the status is Ok.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace backpressure {

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  /// Caller supplied structurally invalid arguments.
  InvalidArgument,
  /// Input could not be parsed as the expected structure.
  MalformedInput,
  /// Input ended before the structure was complete.
  TruncatedInput,
  /// Input exceeded a configured hard bound.
  OversizedInput,
  /// Integrity check (CRC / digest / chain) failed.
  IntegrityMismatch,
  /// Checked arithmetic overflowed.
  Overflow,
  /// Value was representable but outside the accepted domain.
  OutOfRange,
  /// Referenced entity does not exist.
  NotFound,
  /// Entity already exists.
  Duplicate,
  /// Two authoritative statements disagree.
  Conflict,
  /// A topology contains a cycle where one is not permitted.
  CycleDetected,
  /// Evidence is bound to a superseded dependency generation.
  StaleGeneration,
  /// Evidence is bound to a superseded authority epoch.
  StaleEpoch,
  /// Evidence is bound to a superseded publisher incarnation.
  StaleIncarnation,
  /// Evidence lifetime elapsed.
  Expired,
  /// A fence forbids this operation.
  Fenced,
  /// Authority does not cover the requested effect.
  Unauthorized,
  /// Source pressure is UNKNOWN; UNKNOWN never authorizes propagation.
  UnknownPressure,
  /// A traversal/expansion budget was exhausted.
  BudgetExhausted,
  /// A configured structural limit was reached.
  LimitExceeded,
  /// Component is not in a state that permits the operation.
  NotReady,
  /// Operation was cancelled; no authoritative effect was committed.
  Cancelled,
  /// Component is shutting down and no longer accepts work.
  ShuttingDown,
  /// Operating system or filesystem failure.
  IoError,
  /// Persisted/on-wire format version is not supported by this build.
  VersionMismatch,
  /// Outcome is ambiguous and requires revalidation.
  Ambiguous,
  /// Durable evidence must be revalidated before it can authorize anything.
  RevalidationRequired,
  /// Feature is not supported by this build/platform.
  Unsupported,
  /// Invariant violation inside the runtime.
  Internal,
  Count,
};

inline constexpr std::uint16_t kErrorCodeCount = static_cast<std::uint16_t>(ErrorCode::Count);

[[nodiscard]] constexpr const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::MalformedInput: return "MalformedInput";
    case ErrorCode::TruncatedInput: return "TruncatedInput";
    case ErrorCode::OversizedInput: return "OversizedInput";
    case ErrorCode::IntegrityMismatch: return "IntegrityMismatch";
    case ErrorCode::Overflow: return "Overflow";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::Duplicate: return "Duplicate";
    case ErrorCode::Conflict: return "Conflict";
    case ErrorCode::CycleDetected: return "CycleDetected";
    case ErrorCode::StaleGeneration: return "StaleGeneration";
    case ErrorCode::StaleEpoch: return "StaleEpoch";
    case ErrorCode::StaleIncarnation: return "StaleIncarnation";
    case ErrorCode::Expired: return "Expired";
    case ErrorCode::Fenced: return "Fenced";
    case ErrorCode::Unauthorized: return "Unauthorized";
    case ErrorCode::UnknownPressure: return "UnknownPressure";
    case ErrorCode::BudgetExhausted: return "BudgetExhausted";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    case ErrorCode::NotReady: return "NotReady";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::ShuttingDown: return "ShuttingDown";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::VersionMismatch: return "VersionMismatch";
    case ErrorCode::Ambiguous: return "Ambiguous";
    case ErrorCode::RevalidationRequired: return "RevalidationRequired";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::Count: break;
  }
  return "UnknownErrorCode";
}

/// True when the code represents a rejection caused by stale or superseded
/// evidence rather than a structural fault.
[[nodiscard]] constexpr bool is_staleness(ErrorCode code) noexcept {
  return code == ErrorCode::StaleGeneration || code == ErrorCode::StaleEpoch ||
         code == ErrorCode::StaleIncarnation || code == ErrorCode::Expired;
}

inline constexpr std::size_t kStatusContextBytes = 96;

/// Small, allocation-free status object.
class Status {
 public:
  constexpr Status() noexcept = default;
  constexpr Status(ErrorCode code) noexcept : code_(code) {}  // NOLINT(google-explicit-constructor)

  /// Successful status. Named \c success because \c ok is the predicate.
  [[nodiscard]] static constexpr Status success() noexcept { return Status{}; }

  [[nodiscard]] static Status error(ErrorCode code, std::string_view context = {},
                                    std::uint64_t detail = 0) noexcept {
    Status s(code);
    s.detail_ = detail;
    const std::size_t n = context.size() < kStatusContextBytes ? context.size() : kStatusContextBytes;
    for (std::size_t i = 0; i < n; ++i) {
      s.context_[i] = context[i];
    }
    s.context_len_ = static_cast<std::uint16_t>(n);
    return s;
  }

  [[nodiscard]] constexpr ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] constexpr bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] std::string_view context() const noexcept {
    return std::string_view(context_, context_len_);
  }
  [[nodiscard]] constexpr std::uint64_t detail() const noexcept { return detail_; }

  /// Same code with additional context; original context is preserved when the
  /// new context is empty.
  [[nodiscard]] Status with_context(std::string_view extra) const noexcept {
    if (extra.empty()) {
      return *this;
    }
    return Status::error(code_, extra, detail_);
  }

  friend constexpr bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_;
  }
  friend constexpr bool operator!=(const Status& a, const Status& b) noexcept {
    return !(a == b);
  }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::uint64_t detail_ = 0;
  std::uint16_t context_len_ = 0;
  char context_[kStatusContextBytes] = {};
};

/// Value-or-status carrier. Result<T> is only constructible with a value when
/// the accompanying status is Ok.
template <class T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<T, Status>, "use Status directly for void-like results");
  static_assert(!std::is_reference_v<T>, "Result<T> stores by value");

 public:
  using value_type = T;

  Result(T value)  // NOLINT(google-explicit-constructor)
      : status_(ErrorCode::Ok), value_(std::in_place, std::move(value)) {}

  Result(Status status)  // NOLINT(google-explicit-constructor)
      : status_(status) {}

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  [[nodiscard]] T& value() & noexcept { return *value_; }
  [[nodiscard]] const T& value() const& noexcept { return *value_; }
  [[nodiscard]] T&& value() && noexcept { return std::move(*value_); }

  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

  [[nodiscard]] T* operator->() noexcept { return &*value_; }
  [[nodiscard]] const T* operator->() const noexcept { return &*value_; }
  [[nodiscard]] T& operator*() noexcept { return *value_; }
  [[nodiscard]] const T& operator*() const noexcept { return *value_; }

 private:
  Status status_;
  std::optional<T> value_;
};

/// Convenience factories that keep call sites terse.
template <class T>
[[nodiscard]] Result<T> ok_result(T value) {
  return Result<T>(std::move(value));
}

template <class T>
[[nodiscard]] Result<T> fail(ErrorCode code, std::string_view context = {},
                             std::uint64_t detail = 0) {
  return Result<T>(Status::error(code, context, detail));
}

[[nodiscard]] inline Status fail_status(ErrorCode code, std::string_view context = {},
                                        std::uint64_t detail = 0) {
  return Status::error(code, context, detail);
}

}  // namespace backpressure

/// Stop the enclosing function, returning a failing Status unchanged.
#define BPFAB_TRY(expr)                        \
  do {                                         \
    ::backpressure::Status bpfab_try_status_ = (expr);\
    if (!bpfab_try_status_.ok()) {             \
      return bpfab_try_status_;                \
    }                                          \
  } while (false)

/// Unwrap a Result<T> into an already declared lvalue, or return the failing
/// Status from the enclosing function.
#define BPFAB_TRY_ASSIGN(lhs, expr)            \
  do {                                         \
    auto bpfab_try_result_ = (expr);           \
    if (!bpfab_try_result_.ok()) {             \
      return bpfab_try_result_.status();       \
    }                                          \
    (lhs) = std::move(bpfab_try_result_).value();\
  } while (false)

/// Declare a named value from a Result<T>, or return the failing Status from
/// the enclosing function. Must be used as a standalone statement.
#define BPFAB_TRY_DECL(type, name, expr)       \
  auto bpfab_try_tmp_##name = (expr);          \
  if (!bpfab_try_tmp_##name.ok()) {            \
    return bpfab_try_tmp_##name.status();      \
  }                                            \
  type name = std::move(bpfab_try_tmp_##name).value()
