// src/selection.cpp
//
// Selection vocabulary.
//
// Selection is an authoritative state transition, and every rejection is
// reported at the stage that produced it. These names are the observable
// contract: a persisted decision, a protocol frame and an operator report all
// spell a stage and a rejection reason exactly as they are spelled here, so a
// rejection can be attributed to a pipeline stage without parsing prose.

#include "autonomous_foundry/selection.hpp"

#include <cstdint>
#include <string_view>

namespace autonomous_foundry {

std::string_view selection_stage_name(SelectionStage stage) noexcept {
  switch (stage) {
    case SelectionStage::Authority:
      return "Authority";
    case SelectionStage::CandidateLifecycle:
      return "CandidateLifecycle";
    case SelectionStage::RequiredEvidence:
      return "RequiredEvidence";
    case SelectionStage::HardConstraints:
      return "HardConstraints";
    case SelectionStage::TaskCompatibility:
      return "TaskCompatibility";
    case SelectionStage::PolicyFeasibility:
      return "PolicyFeasibility";
    case SelectionStage::Ranking:
      return "Ranking";
    case SelectionStage::TieBreak:
      return "TieBreak";
    case SelectionStage::Decision:
      return "Decision";
  }
  return "Unknown";
}

std::string_view selection_decision_state_name(SelectionDecisionState state) noexcept {
  switch (state) {
    case SelectionDecisionState::Prepared:
      return "Prepared";
    case SelectionDecisionState::Committed:
      return "Committed";
    case SelectionDecisionState::Rejected:
      return "Rejected";
    case SelectionDecisionState::Impossible:
      return "Impossible";
  }
  return "Unknown";
}

std::string_view exclusion_reason_name(ExclusionReason reason) noexcept {
  switch (reason) {
    case ExclusionReason::None:
      return "None";
    case ExclusionReason::UnknownCandidate:
      return "UnknownCandidate";
    case ExclusionReason::LifecycleNotEligible:
      return "LifecycleNotEligible";
    case ExclusionReason::MandatoryEvidenceMissing:
      return "MandatoryEvidenceMissing";
    case ExclusionReason::MandatoryEvaluationFailed:
      return "MandatoryEvaluationFailed";
    case ExclusionReason::MandatoryEvaluationUnknown:
      return "MandatoryEvaluationUnknown";
    case ExclusionReason::MandatoryEvaluationUnsupported:
      return "MandatoryEvaluationUnsupported";
    case ExclusionReason::MandatoryEvaluationError:
      return "MandatoryEvaluationError";
    case ExclusionReason::MandatoryEvaluationCancelled:
      return "MandatoryEvaluationCancelled";
    case ExclusionReason::MandatoryEvaluatorNotAuthoritative:
      return "MandatoryEvaluatorNotAuthoritative";
    case ExclusionReason::TaskIncompatible:
      return "TaskIncompatible";
    case ExclusionReason::EvidenceGenerationMismatch:
      return "EvidenceGenerationMismatch";
    case ExclusionReason::Superseded:
      return "Superseded";
    case ExclusionReason::Retired:
      return "Retired";
    case ExclusionReason::PolicyInfeasible:
      return "PolicyInfeasible";
    case ExclusionReason::BudgetExhausted:
      return "BudgetExhausted";
    case ExclusionReason::LineageRetired:
      return "LineageRetired";
  }
  return "Unknown";
}

}  // namespace autonomous_foundry
