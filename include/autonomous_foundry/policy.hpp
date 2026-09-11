#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/limits.hpp"

// Policy: the explicit, digestible description of how candidates are ranked,
// how many survive, and how many resources the population may spend.
//
// The policy must state exactly which factors ranking uses. The runtime never
// invents a metric to fill a scorecard; a factor that is not listed here is not
// consulted, and a factor that is listed but unavailable is reported as such
// rather than defaulted to zero.

namespace autonomous_foundry {

enum class RankingFactorKind : std::uint8_t {
  /// Mean normalised score of complete optional evaluations.
  QualityScore = 0,
  /// Score of the optional evaluator keyed "performance", when present.
  EfficiencyScore = 1,
  /// Fraction of declared optional requirements with a complete record.
  EvaluationCompleteness = 2,
  /// Depth of the candidate in its lineage. Usually lower is better.
  LineageDepth = 3,
  /// Number of attempts spent to produce the candidate.
  AttemptCount = 4,
  /// Total published artifact bytes.
  ArtifactBytes = 5,
  /// Mandatory final tie-break on the identity value. Always lower is better
  /// and always evaluated last, so that ranking is a total order.
  CandidateIdTieBreak = 6,
};

inline constexpr std::size_t kRankingFactorKindCount = 7;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view ranking_factor_name(
    RankingFactorKind kind) noexcept;

enum class RankingDirection : std::uint8_t {
  HigherIsBetter = 0,
  LowerIsBetter = 1,
};

struct RankingFactor {
  RankingFactorKind kind{RankingFactorKind::QualityScore};
  RankingDirection direction{RankingDirection::HigherIsBetter};
  /// Relative weight. Must be finite and non-negative.
  double weight{1.0};
  /// Stable key used in explanations and in the policy digest.
  std::string key;

  friend bool operator==(const RankingFactor&, const RankingFactor&) noexcept = default;
};

struct SelectionPolicy {
  std::vector<RankingFactor> factors;
  /// When true, an incomplete mandatory requirement removes the candidate
  /// before ranking instead of being treated as a zero score.
  bool require_complete_mandatory{true};

  friend bool operator==(const SelectionPolicy&, const SelectionPolicy&) noexcept = default;
};

struct RetentionPolicy {
  /// Number of best-ranked eligible candidates retained, including the winner.
  std::uint32_t retain_top_k{3};
  /// Hard ceiling on retained candidates in the population.
  std::uint32_t max_retained{8};
  /// At most this many retained candidates may share one lineage.
  std::uint32_t max_per_lineage{2};
  /// The selected candidate is always retained when retention runs.
  bool retain_selected{true};

  friend bool operator==(const RetentionPolicy&, const RetentionPolicy&) noexcept = default;
};

struct FoundryPolicy {
  PolicyId id;
  PolicyGeneration generation;
  std::string name;

  SelectionPolicy selection;
  RetentionPolicy retention;
  BudgetLimits budgets;

  /// Maximum lineage depth a population may reach before advancing is refused.
  std::uint32_t max_generation_depth{16};

  /// When true, the selected candidate of generation N is carried into
  /// generation N+1 as an elite: it is re-registered as a new candidate whose
  /// parent is the selected candidate, and it must satisfy evaluation again.
  /// Carry-forward never inherits evaluation evidence.
  bool carry_forward_elite{false};

  std::string content_digest;

  friend bool operator==(const FoundryPolicy&, const FoundryPolicy&) noexcept = default;
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string encode_policy_canonical(
    const FoundryPolicy& policy);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> compute_policy_digest(
    const FoundryPolicy& policy);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_policy(const FoundryPolicy& policy);

/// The reference policy used by the CLI demo, the examples and the distributed
/// proof when no other policy is supplied.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API FoundryPolicy make_reference_policy(PolicyId id,
                                                                        PolicyGeneration generation);

}  // namespace autonomous_foundry
