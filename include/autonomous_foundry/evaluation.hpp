#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/task.hpp"

// Evaluation evidence.
//
// An evaluation record is generation bound evidence about one candidate. It is
// not a boolean and not a score: it is a durable statement that a named
// evaluator ran against a specific candidate generation under a specific task
// generation and produced a specific typed outcome at a specific coordinator
// epoch. Evidence that is not complete, not authority bound, or produced by an
// evaluator kind that cannot satisfy a mandatory gate is never treated as a
// pass.

namespace autonomous_foundry {

enum class EvaluatorKind : std::uint8_t {
  /// Launches a real local process and observes its actual exit state.
  ProcessCommand = 0,
  /// Reads candidate artifacts and applies a deterministic static rule.
  SourcePolicy = 1,
  /// Deterministic in-process computation over declared candidate metadata.
  PortableReference = 2,
  /// The producing worker's own claim about its output. Recorded for lineage
  /// and diagnostics, but never able to satisfy a mandatory requirement.
  WorkerSelfReport = 3,
};

inline constexpr std::size_t kEvaluatorKindCount = 4;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view evaluator_kind_name(
    EvaluatorKind kind) noexcept;

enum class EvaluationOutcome : std::uint8_t {
  Pass = 0,
  Fail = 1,
  /// The evaluator could not determine an outcome. Never becomes a pass.
  Unknown = 2,
  /// The environment cannot provide this capability. Never becomes a pass.
  Unsupported = 3,
  /// The evaluator itself failed.
  Error = 4,
  Cancelled = 5,
};

inline constexpr std::size_t kEvaluationOutcomeCount = 6;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view evaluation_outcome_name(
    EvaluationOutcome outcome) noexcept;

/// Only a complete, authority bound PASS satisfies a mandatory requirement.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool outcome_satisfies_mandatory(
    EvaluationOutcome outcome) noexcept;

/// True when this evaluator kind is allowed to satisfy a mandatory gate.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool evaluator_kind_is_authoritative(
    EvaluatorKind kind) noexcept;

struct EvaluationRecord {
  EvaluationId id;
  EvaluationGeneration generation;

  CandidateId candidate;
  CandidateGeneration candidate_generation;

  TaskId task;
  TaskGeneration task_generation;

  PopulationId population;
  PopulationGeneration population_generation;

  EvaluatorId evaluator;
  std::string evaluator_key;
  EvaluatorKind kind{EvaluatorKind::PortableReference};
  RequirementClass requirement_class{RequirementClass::Mandatory};

  EvaluationOutcome outcome{EvaluationOutcome::Unknown};
  /// False while the evaluator is still running, and false forever when the
  /// evaluator never reported a terminal state.
  bool complete{false};
  bool has_score{false};
  double score{0.0};

  /// Coordinator epoch under which the record reached a terminal state.
  CoordinatorEpoch decided_epoch;

  std::string diagnostics;
  /// Digest of the evidence payload the evaluator produced.
  std::string evidence_digest;
  std::uint64_t duration_micros{0};

  /// True when this record may be counted as satisfying a mandatory gate.
  [[nodiscard]] bool authoritative_for_mandatory() const noexcept {
    return complete && evaluator_kind_is_authoritative(kind) &&
           outcome_satisfies_mandatory(outcome);
  }

  friend bool operator==(const EvaluationRecord&, const EvaluationRecord&) noexcept = default;
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_evaluation_record(
    const EvaluationRecord& record);

/// Aggregate evidence view for one candidate. Rebuilt from the durable record
/// map; never mutated in place by a late or stale record.
struct CandidateEvidence {
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  TaskGeneration task_generation;
  EvidenceGeneration generation;
  std::vector<EvaluationRecord> records;

  [[nodiscard]] const EvaluationRecord* find(std::string_view evaluator_key) const noexcept;
  [[nodiscard]] bool has_complete_authoritative_pass(std::string_view evaluator_key) const noexcept;
};

}  // namespace autonomous_foundry
