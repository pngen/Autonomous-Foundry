#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/policy.hpp"

// Selection.
//
// Selection is an authoritative state transition, not a query. It runs a fixed
// pipeline:
//
//   authority -> candidate lifecycle -> required evidence -> hard evaluation
//   constraints -> task compatibility -> policy feasibility -> ranking
//   -> stable tie-break -> decision
//
// A candidate that fails any stage before ranking never enters ranking, no
// matter how high its soft score is. The decision is prepared, then revalidated
// against the exact canonical state it was derived from, and only then
// committed.

namespace autonomous_foundry {

enum class SelectionStage : std::uint8_t {
  Authority = 0,
  CandidateLifecycle = 1,
  RequiredEvidence = 2,
  HardConstraints = 3,
  TaskCompatibility = 4,
  PolicyFeasibility = 5,
  Ranking = 6,
  TieBreak = 7,
  Decision = 8,
};

inline constexpr std::size_t kSelectionStageCount = 9;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view selection_stage_name(
    SelectionStage stage) noexcept;

enum class SelectionDecisionState : std::uint8_t {
  Prepared = 0,
  Committed = 1,
  Rejected = 2,
  Impossible = 3,
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view selection_decision_state_name(
    SelectionDecisionState state) noexcept;

enum class ExclusionReason : std::uint8_t {
  None = 0,
  UnknownCandidate = 1,
  LifecycleNotEligible = 2,
  MandatoryEvidenceMissing = 3,
  MandatoryEvaluationFailed = 4,
  MandatoryEvaluationUnknown = 5,
  MandatoryEvaluationUnsupported = 6,
  MandatoryEvaluationError = 7,
  MandatoryEvaluationCancelled = 8,
  MandatoryEvaluatorNotAuthoritative = 9,
  TaskIncompatible = 10,
  EvidenceGenerationMismatch = 11,
  Superseded = 12,
  Retired = 13,
  PolicyInfeasible = 14,
  BudgetExhausted = 15,
  LineageRetired = 16,
};

inline constexpr std::size_t kExclusionReasonCount = 17;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view exclusion_reason_name(
    ExclusionReason reason) noexcept;

struct RankingFactorValue {
  std::string key;
  RankingFactorKind kind{RankingFactorKind::QualityScore};
  RankingDirection direction{RankingDirection::HigherIsBetter};
  double weight{1.0};
  /// Raw measured value. NaN is never produced; a missing measurement uses
  /// available = false and contributes an explicit neutral value.
  double raw_value{0.0};
  bool available{false};
  /// Weighted contribution to the total ordering key.
  double contribution{0.0};

  friend bool operator==(const RankingFactorValue&, const RankingFactorValue&) noexcept = default;
};

struct RankingEntry {
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  std::uint32_t rank{0};
  double total_score{0.0};
  std::vector<RankingFactorValue> factors;
  std::uint64_t artifact_bytes{0};
  std::uint32_t lineage_depth{0};
  std::string diversity_key;

  friend bool operator==(const RankingEntry&, const RankingEntry&) noexcept = default;
};

struct ExclusionEntry {
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  SelectionStage stage{SelectionStage::CandidateLifecycle};
  ExclusionReason reason{ExclusionReason::None};
  std::string detail;

  friend bool operator==(const ExclusionEntry&, const ExclusionEntry&) noexcept = default;
};

struct SelectionDecision {
  SelectionGeneration generation;

  PopulationId population;
  PopulationGeneration population_generation;

  TaskId task;
  TaskGeneration task_generation;

  PolicyId policy;
  PolicyGeneration policy_generation;

  CoordinatorEpoch coordinator_epoch;

  SelectionDecisionState state{SelectionDecisionState::Prepared};

  CandidateId selected;
  CandidateGeneration selected_generation;

  std::vector<RankingEntry> ranking;
  std::vector<ExclusionEntry> excluded;

  /// SHA-256 over the canonical encoding of every input this decision used.
  std::string canonical_state_digest;

  std::string rationale;

  [[nodiscard]] bool has_winner() const noexcept { return selected.valid(); }

  friend bool operator==(const SelectionDecision&, const SelectionDecision&) noexcept = default;
};

}  // namespace autonomous_foundry
