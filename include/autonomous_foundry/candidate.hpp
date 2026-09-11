#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"

// Candidate records.
//
// A candidate is not "an answer an agent produced". It is a durable object with
// a lifecycle, a producing incarnation, a lineage position, an evidence set and
// a decision history. "Candidate exists" and "candidate is valid" are different
// states, and the runtime never lets one collapse into the other.

namespace autonomous_foundry {

enum class CandidateState : std::uint8_t {
  /// Registered and budgeted, but no production attempt has been authorized.
  Registered = 0,
  /// At least one attempt is authorized or in flight.
  Producing = 1,
  /// A worker published output and the runtime committed it transactionally.
  Published = 2,
  /// Mandatory or optional evaluations are in flight.
  Evaluating = 3,
  /// Every declared requirement has a complete record for this generation.
  Evaluated = 4,
  /// Something the candidate depended on changed. Evidence must be rebuilt
  /// before the candidate may participate in selection again.
  RevalidationRequired = 5,
  /// Won selection for its population generation.
  Selected = 6,
  /// Kept by the retention policy without winning selection.
  Retained = 7,
  /// Terminal: deliberately released. History is preserved.
  Retired = 8,
  /// Terminal: replaced by a newer candidate. History is preserved.
  Superseded = 9,
  /// Terminal: production failed for every attempt.
  ProductionFailed = 10,
  /// Terminal: production was cancelled.
  ProductionCancelled = 11,
  /// Terminal: failed a hard gate. Never enters ranking again in this
  /// generation.
  Disqualified = 12,
};

inline constexpr std::size_t kCandidateStateCount = 13;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view candidate_state_name(
    CandidateState state) noexcept;
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool candidate_state_is_terminal(
    CandidateState state) noexcept;

/// Whether state -> next is a legal candidate transition.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool candidate_transition_is_legal(
    CandidateState from, CandidateState to) noexcept;

struct CandidateRecord {
  CandidateId id;
  CandidateGeneration generation;

  PopulationId population;
  PopulationGeneration population_generation;

  TaskId task;
  TaskGeneration task_generation;

  LineageId lineage;
  std::vector<CandidateId> parents;
  std::uint32_t depth{0};

  WorkerId producer_worker;
  WorkerBootId producer_boot;
  AttemptId attempt;
  AttemptGeneration attempt_generation;
  AssignmentId assignment;

  CoordinatorEpoch created_epoch;
  CoordinatorEpoch state_changed_epoch;

  CandidateState state{CandidateState::Registered};

  std::vector<ArtifactRef> artifacts;
  std::string artifact_set_digest;

  /// Number of production attempts spent on this candidate slot.
  std::uint32_t attempt_count{0};

  /// Evidence generation of the candidate's evaluation set. Advances whenever
  /// evidence is added or invalidated.
  EvidenceGeneration evidence_generation;
  EvaluationGeneration evaluation_generation;

  /// Identity of the committed selection decision that made this candidate the
  /// winner, or the invalid generation when it is not a winner.
  SelectionGeneration selection_generation;

  bool selected{false};
  bool retained{false};
  bool promotion_eligible{false};
  bool promotion_requested{false};

  /// Reference diversity signal. This is a declared strategy label plus a
  /// deterministic structural bucket of the published source. It is labelled
  /// SYNTHETIC/REFERENCE and is never presented as semantic novelty.
  std::string diversity_key;

  std::string failure_reason;
  std::string superseded_by_reason;

  friend bool operator==(const CandidateRecord&, const CandidateRecord&) noexcept = default;
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_candidate_record(
    const CandidateRecord& record);

/// Deterministic, explicitly labelled reference diversity key.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string reference_diversity_key(
    std::string_view declared_strategy, const std::vector<ArtifactRef>& artifacts);

}  // namespace autonomous_foundry
