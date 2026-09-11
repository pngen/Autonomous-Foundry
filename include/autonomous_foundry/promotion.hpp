#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/limits.hpp"

// Promotion eligibility.
//
// Autonomous Foundry does not promote artifacts. It determines that a selected
// candidate is eligible to be handed to an Artifact Promotion boundary, and it
// emits a request that an independent promotion system can validate on its own
// evidence. Requested is not promoted, and the runtime never reports otherwise.

namespace autonomous_foundry {

enum class PromotionEligibilityState : std::uint8_t {
  NotEvaluated = 0,
  Eligible = 1,
  NotEligible = 2,
  Withdrawn = 3,
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view promotion_eligibility_state_name(
    PromotionEligibilityState state) noexcept;

/// What the foundry knows about the handoff. There is deliberately no
/// "Promoted" value: only an external promotion system may claim that.
enum class PromotionHandoffState : std::uint8_t {
  NotRequested = 0,
  Requested = 1,
  AcceptedForProcessing = 2,
  DeclinedBySink = 3,
  SinkUnavailable = 4,
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view promotion_handoff_state_name(
    PromotionHandoffState state) noexcept;

/// Evidence summary shipped with a promotion request. The receiving system can
/// re-verify every digest independently.
struct PromotionEvidenceSummary {
  std::string evaluator_key;
  EvaluatorKind kind{EvaluatorKind::PortableReference};
  EvaluationOutcome outcome{EvaluationOutcome::Unknown};
  RequirementClass requirement_class{RequirementClass::Mandatory};
  bool complete{false};
  bool has_score{false};
  double score{0.0};
  EvaluationGeneration generation;
  std::string evidence_digest;

  friend bool operator==(const PromotionEvidenceSummary&,
                         const PromotionEvidenceSummary&) noexcept = default;
};

struct PromotionRequest {
  PromotionRequestId id;

  FoundryId foundry;
  FoundryRunId run;
  CoordinatorEpoch coordinator_epoch;

  PopulationId population;
  PopulationGeneration population_generation;

  CandidateId candidate;
  CandidateGeneration candidate_generation;

  TaskId task;
  TaskGeneration task_generation;

  LineageId lineage;
  SelectionGeneration selection_generation;
  PolicyId policy;
  PolicyGeneration policy_generation;
  EvidenceGeneration evidence_generation;

  PromotionEligibilityState eligibility{PromotionEligibilityState::NotEvaluated};
  /// Concrete reasons the candidate is not eligible. Empty when eligible.
  std::vector<std::string> outstanding_requirements;

  std::vector<ArtifactRef> artifacts;
  std::uint64_t total_artifact_bytes{0};
  std::string artifact_set_digest;

  std::vector<PromotionEvidenceSummary> mandatory_evidence;

  /// Root-first ancestry of the candidate, for provenance reconstruction.
  std::vector<CandidateId> ancestry;
  std::uint32_t lineage_depth{0};

  /// SHA-256 over the canonical encoding of the candidate plus its evidence as
  /// of the moment the request was produced.
  std::string canonical_state_digest;

  friend bool operator==(const PromotionRequest&, const PromotionRequest&) noexcept = default;
};

struct PromotionReceipt {
  PromotionHandoffState handoff{PromotionHandoffState::NotRequested};
  std::string sink_name;
  /// Opaque reference assigned by the receiving system, if any.
  std::string sink_reference;
  std::string detail;
  std::string request_digest;

  friend bool operator==(const PromotionReceipt&, const PromotionReceipt&) noexcept = default;
};

/// Interface implemented by anything that can receive a promotion request.
///
/// A receipt with handoff == AcceptedForProcessing means the receiver took
/// responsibility for its own promotion decision. It does not mean the
/// artifact was promoted.
class AUTONOMOUS_FOUNDRY_API PromotionSink {
 public:
  PromotionSink() = default;
  virtual ~PromotionSink();
  PromotionSink(const PromotionSink&) = delete;
  PromotionSink& operator=(const PromotionSink&) = delete;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual Result<PromotionReceipt> submit(const PromotionRequest& request) = 0;
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string encode_promotion_request_canonical(
    const PromotionRequest& request);

}  // namespace autonomous_foundry
