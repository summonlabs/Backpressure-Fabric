#pragma once

// Backpressure Fabric - version and build identification.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#if !defined(BPFAB_VERSION_MAJOR)
#define BPFAB_VERSION_MAJOR 1
#endif
#if !defined(BPFAB_VERSION_MINOR)
#define BPFAB_VERSION_MINOR 0
#endif
#if !defined(BPFAB_VERSION_PATCH)
#define BPFAB_VERSION_PATCH 0
#endif

#include <cstdint>
#include <string_view>

namespace backpressure {

inline constexpr int kVersionMajor = BPFAB_VERSION_MAJOR;
inline constexpr int kVersionMinor = BPFAB_VERSION_MINOR;
inline constexpr int kVersionPatch = BPFAB_VERSION_PATCH;

/// Semantic version of the persistent/on-the-wire state formats this build
/// understands. Bumped only when a durable or framed layout changes.
inline constexpr std::uint16_t kStateFormatVersion = 1;

inline constexpr std::string_view kVersionString = "1.0.0";

inline constexpr std::uint32_t version_packed() noexcept {
  return (static_cast<std::uint32_t>(kVersionMajor) << 16) |
         (static_cast<std::uint32_t>(kVersionMinor) << 8) |
         static_cast<std::uint32_t>(kVersionPatch);
}

}  // namespace backpressure
