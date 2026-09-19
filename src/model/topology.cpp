// Backpressure Fabric - topology construction and validation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/model/topology.hpp"

#include <algorithm>
#include <utility>

namespace backpressure {
namespace {

constexpr std::uint32_t kUnvisited = 0xFFFFFFFFu;

void record_issue(TopologyStats& stats, std::vector<TopologyIssue>& issues,
                  TopologyIssueKind kind, EdgeId edge, ResourceId resource,
                  std::uint64_t detail) {
  if (issues.size() >= kMaxTopologyIssues) {
    ++stats.issues_dropped;
    return;
  }
  TopologyIssue issue;
  issue.kind = kind;
  issue.edge = edge;
  issue.resource = resource;
  issue.detail = detail;
  issues.push_back(issue);
}

}  // namespace

Result<Topology> Topology::build(std::vector<Resource> resources,
                                 std::vector<DependencyEdge> edges,
                                 const TopologyLimits& limits, bool allow_cycles) {
  if (limits.max_out_degree == 0 || limits.max_in_degree == 0 ||
      limits.max_resources == 0 || limits.max_edges == 0) {
    return fail<Topology>(ErrorCode::InvalidArgument, "topology limits");
  }
  if (resources.size() > limits.max_resources) {
    return fail<Topology>(ErrorCode::LimitExceeded, "topology resources",
                          static_cast<std::uint64_t>(resources.size()));
  }
  if (edges.size() > limits.max_edges) {
    return fail<Topology>(ErrorCode::LimitExceeded, "topology edges",
                          static_cast<std::uint64_t>(edges.size()));
  }

  Topology t;

  for (const Resource& r : resources) {
    BPFAB_TRY(r.validate());
  }
  std::sort(resources.begin(), resources.end(),
            [](const Resource& a, const Resource& b) { return a.id < b.id; });
  for (std::size_t i = 1; i < resources.size(); ++i) {
    if (resources[i - 1].id == resources[i].id) {
      return fail<Topology>(ErrorCode::Duplicate, "duplicate resource", resources[i].id.value());
    }
  }

  t.resources_ = std::move(resources);
  t.resource_index_.reserve(t.resources_.size() * 2u + 1u);
  for (std::size_t i = 0; i < t.resources_.size(); ++i) {
    t.resource_index_.emplace(t.resources_[i].id, static_cast<std::uint32_t>(i));
  }

  for (const DependencyEdge& e : edges) {
    BPFAB_TRY(e.validate());
  }
  std::sort(edges.begin(), edges.end(),
            [](const DependencyEdge& a, const DependencyEdge& b) { return edge_precedes(a, b); });
  for (std::size_t i = 1; i < edges.size(); ++i) {
    if (edges[i - 1].id == edges[i].id) {
      return fail<Topology>(ErrorCode::Duplicate, "duplicate edge", edges[i].id.value());
    }
  }
  for (const DependencyEdge& e : edges) {
    if (t.resource_index_.find(e.from) == t.resource_index_.end()) {
      return fail<Topology>(ErrorCode::NotFound, "edge source", e.from.value());
    }
    if (t.resource_index_.find(e.to) == t.resource_index_.end()) {
      return fail<Topology>(ErrorCode::NotFound, "edge target", e.to.value());
    }
  }
  t.edges_ = std::move(edges);

  const std::size_t n = t.resources_.size();
  const std::size_t m = t.edges_.size();

  t.edge_index_.reserve(m * 2u + 1u);
  for (std::size_t i = 0; i < m; ++i) {
    t.edge_index_.emplace(t.edges_[i].id, static_cast<std::uint32_t>(i));
  }

  std::vector<std::uint32_t> out_degree(n, 0u);
  std::vector<std::uint32_t> in_degree(n, 0u);
  for (const DependencyEdge& e : t.edges_) {
    const std::uint32_t s = t.resource_index_.at(e.from);
    const std::uint32_t d = t.resource_index_.at(e.to);
    ++out_degree[s];
    ++in_degree[d];
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (out_degree[i] > limits.max_out_degree) {
      return fail<Topology>(ErrorCode::LimitExceeded, "out degree", out_degree[i]);
    }
    if (in_degree[i] > limits.max_in_degree) {
      return fail<Topology>(ErrorCode::LimitExceeded, "in degree", in_degree[i]);
    }
    if (out_degree[i] > t.stats_.max_out_degree) {
      t.stats_.max_out_degree = out_degree[i];
    }
    if (in_degree[i] > t.stats_.max_in_degree) {
      t.stats_.max_in_degree = in_degree[i];
    }
  }

  t.out_offsets_.assign(n + 1u, 0u);
  for (std::size_t i = 0; i < n; ++i) {
    t.out_offsets_[i + 1u] = t.out_offsets_[i] + out_degree[i];
  }
  t.out_edge_indices_.assign(m, 0u);
  std::vector<std::uint32_t> cursor(t.out_offsets_.begin(), t.out_offsets_.end() - 1);
  for (std::size_t e = 0; e < m; ++e) {
    const std::uint32_t s = t.resource_index_.at(t.edges_[e].from);
    t.out_edge_indices_[cursor[s]] = static_cast<std::uint32_t>(e);
    ++cursor[s];
  }

  // --- issues and statistics ------------------------------------------------
  std::vector<std::uint8_t> in_cycle(n, 0u);
  std::vector<std::uint8_t> has_self_loop(n, 0u);

  for (std::size_t i = 0; i < n; ++i) {
    const Resource& r = t.resources_[i];
    switch (r.protection) {
      case ProtectionClass::Barrier:
        ++t.stats_.barrier_resource_count;
        record_issue(t.stats_, t.issues_, TopologyIssueKind::BarrierResource, EdgeId{}, r.id, 0);
        break;
      case ProtectionClass::Sealed:
        record_issue(t.stats_, t.issues_, TopologyIssueKind::SealedResource, EdgeId{}, r.id, 0);
        break;
      case ProtectionClass::ObservedOnly:
        record_issue(t.stats_, t.issues_, TopologyIssueKind::ObservedOnlyResource, EdgeId{}, r.id,
                     0);
        break;
      case ProtectionClass::None:
      case ProtectionClass::Count:
        break;
    }
    if (out_degree[i] == 0u && in_degree[i] == 0u) {
      record_issue(t.stats_, t.issues_, TopologyIssueKind::IsolatedResource, EdgeId{}, r.id, 0);
    }
  }

  for (const DependencyEdge& e : t.edges_) {
    if (e.is_self_loop()) {
      ++t.stats_.self_loop_count;
      has_self_loop[t.resource_index_.at(e.from)] = 1u;
      record_issue(t.stats_, t.issues_, TopologyIssueKind::SelfLoop, e.id, e.from, 0);
    }
    if (e.is_amplifying()) {
      ++t.stats_.amplifying_edge_count;
      record_issue(t.stats_, t.issues_, TopologyIssueKind::AmplifyingEdge, e.id, e.to,
                   static_cast<std::uint64_t>(e.amplification.raw()));
    }
    if (e.attenuation.is_zero()) {
      ++t.stats_.zero_attenuation_edge_count;
      record_issue(t.stats_, t.issues_, TopologyIssueKind::ZeroAttenuationEdge, e.id, e.to, 0);
    }
    if (e.flags.has(EdgeFlagKind::RecoveryOnly)) {
      record_issue(t.stats_, t.issues_, TopologyIssueKind::RecoveryOnlyEdge, e.id, e.to, 0);
    }
  }

  // --- strongly connected components (iterative Tarjan) ---------------------
  {
    std::vector<std::uint32_t> index(n, kUnvisited);
    std::vector<std::uint32_t> low(n, 0u);
    std::vector<std::uint32_t> scc_stack;
    scc_stack.reserve(n);
    std::vector<std::uint8_t> on_stack(n, 0u);
    struct Frame {
      std::uint32_t node;
      std::uint32_t cursor;
    };
    std::vector<Frame> frames;
    frames.reserve(n);
    std::vector<std::uint32_t> component_scratch;
    component_scratch.reserve(64);
    std::uint32_t next_index = 0u;
    std::size_t scc_count = 0u;

    for (std::size_t root = 0; root < n; ++root) {
      if (index[root] != kUnvisited) {
        continue;
      }
      index[root] = next_index;
      low[root] = next_index;
      ++next_index;
      scc_stack.push_back(static_cast<std::uint32_t>(root));
      on_stack[root] = 1u;
      frames.push_back(Frame{static_cast<std::uint32_t>(root), t.out_offsets_[root]});

      while (!frames.empty()) {
        const std::uint32_t v = frames.back().node;
        if (frames.back().cursor < t.out_offsets_[v + 1u]) {
          const std::uint32_t e = t.out_edge_indices_[frames.back().cursor];
          ++frames.back().cursor;
          const std::uint32_t w = t.resource_index_.at(t.edges_[e].to);
          if (index[w] == kUnvisited) {
            index[w] = next_index;
            low[w] = next_index;
            ++next_index;
            scc_stack.push_back(w);
            on_stack[w] = 1u;
            frames.push_back(Frame{w, t.out_offsets_[w]});
          } else if (on_stack[w] != 0u && index[w] < low[v]) {
            low[v] = index[w];
          }
        } else {
          frames.pop_back();
          if (!frames.empty()) {
            const std::uint32_t parent = frames.back().node;
            if (low[v] < low[parent]) {
              low[parent] = low[v];
            }
          }
          if (low[v] == index[v]) {
            const std::size_t mark_begin = component_scratch.size();
            std::size_t size = 0u;
            std::uint32_t member = kUnvisited;
            do {
              member = scc_stack.back();
              scc_stack.pop_back();
              on_stack[member] = 0u;
              component_scratch.push_back(member);
              ++size;
            } while (member != v);

            const bool cyclic = size > 1u || has_self_loop[v] != 0u;
            if (cyclic) {
              ++scc_count;
              for (std::size_t k = mark_begin; k < component_scratch.size(); ++k) {
                in_cycle[component_scratch[k]] = 1u;
              }
              record_issue(t.stats_, t.issues_, TopologyIssueKind::CyclicComponent, EdgeId{},
                           t.resources_[v].id, size);
            }
            component_scratch.resize(mark_begin);
          }
        }
      }
    }

    t.stats_.strongly_connected_components = scc_count;
    for (std::size_t i = 0; i < n; ++i) {
      if (in_cycle[i] != 0u) {
        t.cyclic_resources_.push_back(t.resources_[i].id);
      }
    }
    t.stats_.cyclic_resource_count = t.cyclic_resources_.size();
  }

  if (!allow_cycles && !t.cyclic_resources_.empty()) {
    return fail<Topology>(ErrorCode::CycleDetected, "topology contains cycles",
                          t.cyclic_resources_.size());
  }

  t.stats_.resource_count = n;
  t.stats_.edge_count = m;

  DigestBuilder b;
  b.domain(0x60u);
  b.update_u32(static_cast<std::uint32_t>(n));
  for (const Resource& r : t.resources_) {
    r.digest_into(b);
  }
  b.update_u32(static_cast<std::uint32_t>(m));
  for (const DependencyEdge& e : t.edges_) {
    e.digest_into(b);
  }
  t.digest_ = b.finish();
  t.generation_ = Generation::from_raw(t.digest_.fold64());

  return Result<Topology>(std::move(t));
}

const Resource* Topology::find_resource(ResourceId id) const noexcept {
  const auto it = resource_index_.find(id);
  if (it == resource_index_.end()) {
    return nullptr;
  }
  return &resources_[it->second];
}

const DependencyEdge* Topology::find_edge(EdgeId id) const noexcept {
  const auto it = edge_index_.find(id);
  if (it == edge_index_.end()) {
    return nullptr;
  }
  return &edges_[it->second];
}

std::uint32_t Topology::index_of(ResourceId id) const noexcept {
  const auto it = resource_index_.find(id);
  return it == resource_index_.end() ? kInvalidIndex : it->second;
}

const Resource* Topology::resource_at(std::uint32_t index) const noexcept {
  return index < resources_.size() ? &resources_[index] : nullptr;
}

const DependencyEdge* Topology::edge_at(std::uint32_t index) const noexcept {
  return index < edges_.size() ? &edges_[index] : nullptr;
}

std::span<const std::uint32_t> Topology::out_edge_indices(ResourceId id) const noexcept {
  const std::uint32_t idx = index_of(id);
  if (idx == kInvalidIndex) {
    return {};
  }
  return out_edge_indices_at(idx);
}

std::span<const std::uint32_t> Topology::out_edge_indices_at(std::uint32_t index) const noexcept {
  if (index == kInvalidIndex || index + 1u >= out_offsets_.size()) {
    return {};
  }
  const std::uint32_t begin = out_offsets_[index];
  const std::uint32_t end = out_offsets_[index + 1u];
  return std::span<const std::uint32_t>(out_edge_indices_.data() + begin, end - begin);
}

std::size_t Topology::out_degree(ResourceId id) const noexcept {
  return out_edge_indices(id).size();
}

}  // namespace backpressure