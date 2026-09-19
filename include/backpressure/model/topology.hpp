#pragma once

// Backpressure Fabric - dependency topology.
//
// A topology is an immutable, canonically ordered view of resources and
// dependency edges. Immutability is what makes concurrent propagation safe
// without locks: readers see one generation forever. Rebuilding a topology
// produces a new digest and therefore a new topology generation, which is what
// invalidates in-flight propagation bound to the previous revision.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "backpressure/core/digest.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/status.hpp"
#include "backpressure/model/dependency.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/model/resource.hpp"

namespace backpressure {

struct TopologyLimits {
  std::size_t max_resources = 1u << 16;
  std::size_t max_edges = 1u << 18;
  std::size_t max_out_degree = 1024;
  std::size_t max_in_degree = 4096;
};

enum class TopologyIssueKind : std::uint8_t {
  SelfLoop = 0,
  CyclicComponent,
  AmplifyingEdge,
  BarrierResource,
  SealedResource,
  ObservedOnlyResource,
  IsolatedResource,
  ZeroAttenuationEdge,
  RecoveryOnlyEdge,
  Count,
};

[[nodiscard]] constexpr const char* to_string(TopologyIssueKind k) noexcept {
  switch (k) {
    case TopologyIssueKind::SelfLoop: return "SelfLoop";
    case TopologyIssueKind::CyclicComponent: return "CyclicComponent";
    case TopologyIssueKind::AmplifyingEdge: return "AmplifyingEdge";
    case TopologyIssueKind::BarrierResource: return "BarrierResource";
    case TopologyIssueKind::SealedResource: return "SealedResource";
    case TopologyIssueKind::ObservedOnlyResource: return "ObservedOnlyResource";
    case TopologyIssueKind::IsolatedResource: return "IsolatedResource";
    case TopologyIssueKind::ZeroAttenuationEdge: return "ZeroAttenuationEdge";
    case TopologyIssueKind::RecoveryOnlyEdge: return "RecoveryOnlyEdge";
    case TopologyIssueKind::Count: break;
  }
  return "Invalid";
}

struct TopologyIssue {
  TopologyIssueKind kind = TopologyIssueKind::SelfLoop;
  EdgeId edge{};
  ResourceId resource{};
  std::uint64_t detail = 0;
};

struct TopologyStats {
  std::size_t resource_count = 0;
  std::size_t edge_count = 0;
  std::size_t max_out_degree = 0;
  std::size_t max_in_degree = 0;
  std::size_t self_loop_count = 0;
  std::size_t cyclic_resource_count = 0;
  std::size_t strongly_connected_components = 0;
  std::size_t amplifying_edge_count = 0;
  std::size_t barrier_resource_count = 0;
  std::size_t zero_attenuation_edge_count = 0;
  /// Issues that were counted but not retained because the issue list is bounded.
  std::size_t issues_dropped = 0;
};

/// Hard bound on the retained topology issue list.
inline constexpr std::size_t kMaxTopologyIssues = 4096;

class Topology {
 public:
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

  Topology() = default;

  /// Canonicalises and validates the supplied declarations. Ordering of the
  /// inputs does not affect the result: resources are ordered by identity and
  /// edges by (priority, target, identity).
  ///
  /// \p allow_cycles permits a cyclic topology to be built. Cycles are then
  /// reported through stats()/issues() rather than rejected; propagation over a
  /// cyclic topology remains bounded because traversal settles each resource at
  /// most once.
  [[nodiscard]] static Result<Topology> build(std::vector<Resource> resources,
                                              std::vector<DependencyEdge> edges,
                                              const TopologyLimits& limits,
                                              bool allow_cycles);

  [[nodiscard]] const std::vector<Resource>& resources() const noexcept { return resources_; }
  [[nodiscard]] const std::vector<DependencyEdge>& edges() const noexcept { return edges_; }
  [[nodiscard]] const std::vector<TopologyIssue>& issues() const noexcept { return issues_; }
  [[nodiscard]] const TopologyStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::vector<ResourceId>& cyclic_resources() const noexcept {
    return cyclic_resources_;
  }

  [[nodiscard]] const Resource* find_resource(ResourceId id) const noexcept;
  [[nodiscard]] const DependencyEdge* find_edge(EdgeId id) const noexcept;
  [[nodiscard]] std::uint32_t index_of(ResourceId id) const noexcept;
  [[nodiscard]] const Resource* resource_at(std::uint32_t index) const noexcept;
  [[nodiscard]] const DependencyEdge* edge_at(std::uint32_t index) const noexcept;

  /// Edge indices leaving \p id, in canonical traversal order.
  [[nodiscard]] std::span<const std::uint32_t> out_edge_indices(ResourceId id) const noexcept;
  [[nodiscard]] std::span<const std::uint32_t> out_edge_indices_at(
      std::uint32_t index) const noexcept;
  [[nodiscard]] std::size_t out_degree(ResourceId id) const noexcept;

  [[nodiscard]] bool has_cycles() const noexcept { return stats_.cyclic_resource_count != 0; }
  [[nodiscard]] bool empty() const noexcept { return resources_.empty(); }

  /// Canonical digest of the whole topology (resources then edges).
  [[nodiscard]] Digest128 digest() const noexcept { return digest_; }
  /// Generation derived from the digest. Non-zero by construction.
  [[nodiscard]] Generation generation() const noexcept { return generation_; }

 private:
  std::vector<Resource> resources_;
  std::vector<DependencyEdge> edges_;
  std::vector<std::uint32_t> out_offsets_;
  std::vector<std::uint32_t> out_edge_indices_;
  std::unordered_map<ResourceId, std::uint32_t> resource_index_;
  std::unordered_map<EdgeId, std::uint32_t> edge_index_;
  std::vector<TopologyIssue> issues_;
  std::vector<ResourceId> cyclic_resources_;
  TopologyStats stats_{};
  Digest128 digest_{};
  Generation generation_{};
};

}  // namespace backpressure
