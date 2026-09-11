// src/lineage.cpp
//
// Durable lineage DAG.
//
// Lineage is history, so it is only ever appended to. This file refuses every
// operation that would rewrite already-observed ancestry: re-parenting an
// existing candidate, making a node shallower than its parents imply, or
// inserting a node whose lineage identity disagrees with its primary parent.
// A retroactive ancestry change would silently invalidate every selection
// decision derived from the old shape, so it is a hard failure, not a repair.

#include "autonomous_foundry/lineage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "autonomous_foundry/limits.hpp"

namespace autonomous_foundry {

namespace {

/// Maximum number of parents a lineage node may declare. Mirrors the bound used
/// by candidate validation; repeated here so this file needs no private header.
inline constexpr std::size_t kMaxCandidateParents = 8;

/// The depth a node must declare: 0 for a root, one more than its deepest
/// parent otherwise.
[[nodiscard]] std::uint32_t computed_depth(
    const std::unordered_map<CandidateId, LineageNode>& nodes,
    const std::vector<CandidateId>& parents) {
  std::uint32_t deepest = 0;
  for (const CandidateId parent : parents) {
    const auto found = nodes.find(parent);
    if (found != nodes.end() && found->second.depth > deepest) {
      deepest = found->second.depth;
    }
  }
  return parents.empty() ? 0u : deepest + 1u;
}

/// Iterative depth-first cycle detection over every node. No recursion: a
/// lineage may legally reach kMaxLineageDepth, and a recursive walk would make
/// the stack depth a function of untrusted input.
[[nodiscard]] bool find_cycle(const std::unordered_map<CandidateId, LineageNode>& nodes,
                              const std::vector<CandidateId>& ordered,
                              CandidateId& repeated) {
  // 0 = unvisited, 1 = on the current path, 2 = fully explored. The colour is
  // a plain int rather than a narrow byte so that no implicit narrowing
  // conversion happens inside the standard container.
  std::unordered_map<CandidateId, int> colour;
  colour.reserve(nodes.size() * 2 + 1);
  for (const auto& entry : nodes) {
    colour.emplace(entry.first, 0);
  }

  std::vector<std::pair<CandidateId, std::size_t>> stack;
  for (const CandidateId root : ordered) {
    const auto root_colour = colour.find(root);
    if (root_colour == colour.end() || root_colour->second != 0) {
      continue;
    }
    stack.clear();
    stack.emplace_back(root, 0);
    colour[root] = 1;
    while (!stack.empty()) {
      std::pair<CandidateId, std::size_t>& frame = stack.back();
      const LineageNode& node = nodes.at(frame.first);
      if (frame.second < node.parents.size()) {
        const CandidateId parent = node.parents[frame.second];
        ++frame.second;
        const auto parent_colour = colour.find(parent);
        if (parent_colour == colour.end()) {
          continue;  // missing parent: reported separately by validate()
        }
        if (parent_colour->second == 1) {
          repeated = parent;
          return true;
        }
        if (parent_colour->second == 0) {
          parent_colour->second = 1;
          stack.emplace_back(parent, 0);
        }
        continue;
      }
      colour[frame.first] = 2;
      stack.pop_back();
    }
  }
  return false;
}

}  // namespace

Status LineageGraph::insert(LineageNode node) {
  if (!node.candidate.valid()) {
    return Status(ErrorCode::NullIdentity, std::string_view{"lineage node names the null candidate identity"});
  }
  if (!node.candidate_generation.valid()) {
    return Status(ErrorCode::InvalidArgument,
                  "lineage node " + node.candidate.to_string() +
                      " carries generation 0, which is never a valid generation");
  }
  if (!node.lineage.valid()) {
    return Status(ErrorCode::NullIdentity,
                  "lineage node " + node.candidate.to_string() +
                      " names the null lineage identity");
  }
  if (node.parents.size() > kMaxCandidateParents) {
    return Status(ErrorCode::InvalidArgument,
                  "lineage node " + node.candidate.to_string() + " declares " +
                      std::to_string(node.parents.size()) +
                      " parents, which exceeds the maximum of " +
                      std::to_string(kMaxCandidateParents));
  }
  for (std::size_t index = 0; index < node.parents.size(); ++index) {
    const CandidateId parent = node.parents[index];
    if (!parent.valid()) {
      return Status(ErrorCode::NullIdentity,
                    "lineage node " + node.candidate.to_string() + " parent[" +
                        std::to_string(index) + "] is the null identity");
    }
    if (parent == node.candidate) {
      return Status(ErrorCode::LineageSelfParent,
                    "lineage node " + node.candidate.to_string() + " declares itself as parent[" +
                        std::to_string(index) + "]");
    }
  }

  const auto existing = nodes_.find(node.candidate);
  if (existing != nodes_.end()) {
    if (existing->second == node) {
      // An identical re-insert is how a replay of already-durable history is
      // absorbed. It changes nothing, so it is not an error.
      return Status();
    }
    return Status(ErrorCode::AlreadyExists,
                  "lineage node " + node.candidate.to_string() +
                      " already exists with different ancestry, retirement state or epoch; "
                      "finalized lineage history is never rewritten");
  }

  for (const CandidateId parent : node.parents) {
    if (nodes_.find(parent) == nodes_.end()) {
      return Status(ErrorCode::LineageOrphan,
                    "lineage node " + node.candidate.to_string() + " names parent " +
                        parent.to_string() + ", which is not present in the lineage graph");
    }
  }

  const std::uint32_t expected = computed_depth(nodes_, node.parents);
  if (node.depth != expected) {
    return Status(ErrorCode::GenerationRegression,
                  "lineage node " + node.candidate.to_string() + " declares depth " +
                      std::to_string(node.depth) + " but its parents place it at depth " +
                      std::to_string(expected));
  }
  if (node.depth > kMaxLineageDepth) {
    return Status(ErrorCode::InvalidArgument,
                  "lineage node " + node.candidate.to_string() + " declares depth " +
                      std::to_string(node.depth) + ", which exceeds the maximum of " +
                      std::to_string(kMaxLineageDepth));
  }
  if (!node.parents.empty()) {
    const LineageNode& primary = nodes_.at(node.parents.front());
    if (node.lineage != primary.lineage) {
      return Status(
          ErrorCode::LineageReparentForbidden,
          "lineage node " + node.candidate.to_string() + " declares lineage " +
              node.lineage.to_string() + " but its primary parent " + primary.candidate.to_string() +
              " belongs to lineage " + primary.lineage.to_string() +
              "; a candidate descends from its primary parent's lineage and lineage identity is "
              "never reassigned retroactively");
    }
  }

  nodes_.emplace(node.candidate, std::move(node));
  return Status();
}

Result<LineageNode> LineageGraph::make_child(CandidateId child,
                                             CandidateGeneration child_generation,
                                             const std::vector<CandidateId>& parents,
                                             LineageId explicit_lineage,
                                             CoordinatorEpoch epoch) const {
  if (!child.valid()) {
    return Status(ErrorCode::NullIdentity, std::string_view{"child candidate identity is the null identity"});
  }
  if (!child_generation.valid()) {
    return Status(ErrorCode::InvalidArgument,
                  "child candidate " + child.to_string() +
                      " carries generation 0, which is never a valid generation");
  }
  if (parents.size() > kMaxCandidateParents) {
    return Status(ErrorCode::InvalidArgument,
                  "child candidate " + child.to_string() + " declares " +
                      std::to_string(parents.size()) + " parents, which exceeds the maximum of " +
                      std::to_string(kMaxCandidateParents));
  }
  for (std::size_t index = 0; index < parents.size(); ++index) {
    const CandidateId parent = parents[index];
    if (!parent.valid()) {
      return Status(ErrorCode::NullIdentity,
                    "child candidate " + child.to_string() + " parent[" + std::to_string(index) +
                        "] is the null identity");
    }
    if (parent == child) {
      return Status(ErrorCode::LineageSelfParent,
                    "candidate " + child.to_string() + " cannot be its own parent[" +
                        std::to_string(index) + "]");
    }
  }

  std::uint32_t deepest = 0;
  for (const CandidateId parent : parents) {
    const auto found = nodes_.find(parent);
    if (found == nodes_.end()) {
      return Status(ErrorCode::LineageOrphan,
                    "child candidate " + child.to_string() + " names parent " +
                        parent.to_string() + ", which is not present in the lineage graph");
    }
    if (found->second.depth > deepest) {
      deepest = found->second.depth;
    }
  }

  LineageNode node;
  node.candidate = child;
  node.candidate_generation = child_generation;
  node.parents = parents;
  if (parents.empty()) {
    node.lineage = explicit_lineage;
    node.depth = 0;
  } else {
    // A child belongs to its primary parent's lineage: lineage identity is
    // inherited from the first declared parent, and the explicit identity is
    // ignored rather than silently swapped in.
    node.lineage = nodes_.at(parents.front()).lineage;
    node.depth = deepest + 1u;
  }
  if (node.depth > kMaxLineageDepth) {
    return Status(ErrorCode::InvalidArgument,
                  "child candidate " + child.to_string() + " would sit at depth " +
                      std::to_string(node.depth) + ", which exceeds the maximum of " +
                      std::to_string(kMaxLineageDepth));
  }
  node.created_epoch = epoch;
  node.retired = false;
  return node;
}

bool LineageGraph::contains(CandidateId candidate) const noexcept {
  return nodes_.find(candidate) != nodes_.end();
}

const LineageNode* LineageGraph::find(CandidateId candidate) const noexcept {
  const auto found = nodes_.find(candidate);
  return found == nodes_.end() ? nullptr : &found->second;
}

std::vector<CandidateId> LineageGraph::ancestors_of(CandidateId candidate) const {
  std::vector<CandidateId> result;
  const auto start = nodes_.find(candidate);
  if (start == nodes_.end()) {
    return result;
  }

  std::vector<CandidateId> visited;
  std::vector<CandidateId> frontier = start->second.parents;
  while (!frontier.empty()) {
    // Breadth first, nearest first. The order inside one level is the order in
    // which its members were discovered while expanding the previous level,
    // that is the declared parent order with the primary parent first. Sorting
    // each level by identity would destroy that order, and sorting cannot be
    // the contract anyway: the result must be identical for a graph built in a
    // different insertion order, which the declared parent lists guarantee and
    // a re-sort does not. Duplicates are dropped by the visited check below, so
    // no sort is needed to make the level unique.
    frontier.erase(std::unique(frontier.begin(), frontier.end()), frontier.end());

    std::vector<CandidateId> next;
    for (const CandidateId current : frontier) {
      if (current == candidate) {
        // A candidate is never its own ancestor, even if a cycle would
        // otherwise claim it is.
        continue;
      }
      if (std::find(visited.begin(), visited.end(), current) != visited.end()) {
        continue;
      }
      visited.push_back(current);
      result.push_back(current);
      const auto found = nodes_.find(current);
      if (found == nodes_.end()) {
        continue;
      }
      for (const CandidateId parent : found->second.parents) {
        next.push_back(parent);
      }
    }
    frontier.swap(next);
  }
  return result;
}

std::vector<CandidateId> LineageGraph::descendants_of(CandidateId candidate) const {
  std::vector<CandidateId> result;
  if (nodes_.find(candidate) == nodes_.end()) {
    return result;
  }

  std::vector<std::pair<std::uint32_t, CandidateId>> ordered;
  for (const auto& entry : nodes_) {
    if (entry.first == candidate) {
      continue;
    }
    // Walk the primary-parent chain upward. "Descended from" means the
    // candidate appears on this chain, not merely somewhere in a hand-declared
    // parent list of a node whose primary ancestry never reaches the candidate.
    std::vector<CandidateId> walk;
    CandidateId current =
        entry.second.parents.empty() ? CandidateId() : entry.second.parents.front();
    bool descended = false;
    while (current.valid()) {
      if (current == candidate) {
        descended = true;
        break;
      }
      if (std::find(walk.begin(), walk.end(), current) != walk.end()) {
        break;  // defensive: a cycle would otherwise loop forever
      }
      walk.push_back(current);
      const auto found = nodes_.find(current);
      if (found == nodes_.end() || found->second.parents.empty()) {
        break;
      }
      current = found->second.parents.front();
    }
    if (descended) {
      ordered.emplace_back(entry.second.depth, entry.first);
    }
  }
  std::sort(ordered.begin(), ordered.end());
  result.reserve(ordered.size());
  for (const auto& entry : ordered) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<CandidateId> LineageGraph::path_to_root(CandidateId candidate) const {
  std::vector<CandidateId> reversed;
  const auto start = nodes_.find(candidate);
  if (start == nodes_.end()) {
    return reversed;
  }

  std::vector<CandidateId> visited;
  CandidateId current = candidate;
  while (current.valid()) {
    if (std::find(visited.begin(), visited.end(), current) != visited.end()) {
      // A cycle means there is no root, so there is no root-first path. An
      // arbitrary truncation would be a fabricated provenance claim.
      return std::vector<CandidateId>();
    }
    if (visited.size() >= kMaxLineageDepth + 1) {
      return std::vector<CandidateId>();
    }
    visited.push_back(current);
    const auto found = nodes_.find(current);
    if (found == nodes_.end()) {
      break;
    }
    reversed.push_back(current);
    if (found->second.parents.empty()) {
      break;
    }
    current = found->second.parents.front();
  }

  std::vector<CandidateId> path(reversed.rbegin(), reversed.rend());
  return path;
}

Status LineageGraph::retire(CandidateId candidate, std::string reason, CoordinatorEpoch epoch) {
  const auto found = nodes_.find(candidate);
  if (found == nodes_.end()) {
    return Status(ErrorCode::UnknownIdentity,
                  "lineage node " + candidate.to_string() +
                      " is not present in the lineage graph and cannot be retired");
  }
  LineageNode& node = found->second;
  if (node.retired) {
    // Retiring twice is idempotent: the first retirement is the durable fact,
    // and a later duplicate must not overwrite its reason or epoch.
    return Status();
  }
  node.retired = true;
  node.retired_epoch = epoch;
  node.retirement_reason = std::move(reason);
  return Status();
}

bool LineageGraph::is_retired(CandidateId candidate) const noexcept {
  const auto found = nodes_.find(candidate);
  return found != nodes_.end() && found->second.retired;
}

Status LineageGraph::validate() const {
  if (nodes_.empty()) {
    return Status();
  }

  std::vector<CandidateId> ordered;
  ordered.reserve(nodes_.size());
  for (const auto& entry : nodes_) {
    ordered.push_back(entry.first);
  }
  std::sort(ordered.begin(), ordered.end());
  ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

  // Reference integrity.
  for (const CandidateId candidate : ordered) {
    const LineageNode& node = nodes_.at(candidate);
    if (!node.candidate_generation.valid()) {
      return Status(ErrorCode::InvalidArgument,
                    "lineage node " + candidate.to_string() +
                        " carries generation 0, which is never a valid generation");
    }
    if (node.parents.size() > kMaxCandidateParents) {
      return Status(ErrorCode::InvalidArgument,
                    "lineage node " + candidate.to_string() + " declares " +
                        std::to_string(node.parents.size()) +
                        " parents, which exceeds the maximum of " +
                        std::to_string(kMaxCandidateParents));
    }
    for (std::size_t index = 0; index < node.parents.size(); ++index) {
      const CandidateId parent = node.parents[index];
      if (parent == candidate) {
        return Status(ErrorCode::LineageSelfParent,
                      "lineage node " + candidate.to_string() + " is its own parent[" +
                          std::to_string(index) + "]");
      }
      if (nodes_.find(parent) == nodes_.end()) {
        return Status(ErrorCode::LineageOrphan,
                      "lineage node " + candidate.to_string() + " names parent " +
                          parent.to_string() + ", which is referenced but missing");
      }
    }
  }

  // Acyclicity.
  CandidateId repeated;
  if (find_cycle(nodes_, ordered, repeated)) {
    return Status(ErrorCode::LineageCycle,
                  "lineage graph is cyclic: " + repeated.to_string() +
                      " is reachable from itself through the declared parent chain");
  }

  // Depth consistency.
  for (const CandidateId candidate : ordered) {
    const LineageNode& node = nodes_.at(candidate);
    const std::uint32_t expected = computed_depth(nodes_, node.parents);
    if (node.depth != expected) {
      return Status(ErrorCode::GenerationRegression,
                    "lineage node " + candidate.to_string() + " declares depth " +
                        std::to_string(node.depth) + " but its parents place it at depth " +
                        std::to_string(expected));
    }
    if (node.depth > kMaxLineageDepth) {
      return Status(ErrorCode::InvalidArgument,
                    "lineage node " + candidate.to_string() + " declares depth " +
                        std::to_string(node.depth) + ", which exceeds the maximum of " +
                        std::to_string(kMaxLineageDepth));
    }
  }
  return Status();
}

}  // namespace autonomous_foundry