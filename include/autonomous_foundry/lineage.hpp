#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/limits.hpp"

// Lineage.
//
// Lineage is a first-class, durable, acyclic structure. It is preserved for
// retired and superseded candidates: losing selection is not a reason to
// destroy history. Re-parenting finalized history is refused, because a
// retroactive ancestry change would silently invalidate every selection
// decision derived from it.

namespace autonomous_foundry {

struct LineageNode {
  CandidateId candidate;
  CandidateGeneration candidate_generation;

  LineageId lineage;
  std::vector<CandidateId> parents;

  /// Root candidates have depth 0; a child is max(parent depth) + 1.
  std::uint32_t depth{0};

  CoordinatorEpoch created_epoch;

  bool retired{false};
  CoordinatorEpoch retired_epoch;
  std::string retirement_reason;

  friend bool operator==(const LineageNode&, const LineageNode&) noexcept = default;
};

/// Durable lineage DAG.
class AUTONOMOUS_FOUNDRY_API LineageGraph {
 public:
  LineageGraph() = default;

  /// Insert a node. Refuses self-parenting, unknown parents, re-parenting of
  /// an existing node, generation regression and depth overflow.
  Status insert(LineageNode node);

  /// Mark a node as having descended from a selected candidate: creates the
  /// child node with an explicit lineage identity.
  [[nodiscard]] Result<LineageNode> make_child(CandidateId child,
                                               CandidateGeneration child_generation,
                                               const std::vector<CandidateId>& parents,
                                               LineageId explicit_lineage,
                                               CoordinatorEpoch epoch) const;

  [[nodiscard]] bool contains(CandidateId candidate) const noexcept;
  [[nodiscard]] const LineageNode* find(CandidateId candidate) const noexcept;

  /// Deterministic ancestor traversal, nearest first, breadth first, ties
  /// resolved by identity order.
  [[nodiscard]] std::vector<CandidateId> ancestors_of(CandidateId candidate) const;

  /// Deterministic descendant traversal.
  [[nodiscard]] std::vector<CandidateId> descendants_of(CandidateId candidate) const;

  /// Root-to-node path for a candidate, root first.
  [[nodiscard]] std::vector<CandidateId> path_to_root(CandidateId candidate) const;

  Status retire(CandidateId candidate, std::string reason, CoordinatorEpoch epoch);
  [[nodiscard]] bool is_retired(CandidateId candidate) const noexcept;

  /// Full structural validation: acyclicity, depth consistency, reference
  /// integrity, no duplicate candidates.
  [[nodiscard]] Status validate() const;

  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  [[nodiscard]] const std::unordered_map<CandidateId, LineageNode>& nodes() const noexcept {
    return nodes_;
  }
  void restore(std::unordered_map<CandidateId, LineageNode> nodes) noexcept {
    nodes_ = std::move(nodes);
  }

 private:
  std::unordered_map<CandidateId, LineageNode> nodes_;
};

}  // namespace autonomous_foundry
