#include "autonomous_foundry/error.hpp"

#include <string>
#include <string_view>

namespace autonomous_foundry {

std::string_view error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "Ok";

    // identity
    case ErrorCode::NullIdentity:
      return "NullIdentity";
    case ErrorCode::CrossDomainIdentity:
      return "CrossDomainIdentity";
    case ErrorCode::DuplicateIdentity:
      return "DuplicateIdentity";
    case ErrorCode::UnknownIdentity:
      return "UnknownIdentity";
    case ErrorCode::IdentityExhausted:
      return "IdentityExhausted";

    // arguments and encoding
    case ErrorCode::InvalidArgument:
      return "InvalidArgument";
    case ErrorCode::MalformedEncoding:
      return "MalformedEncoding";
    case ErrorCode::LengthOutOfRange:
      return "LengthOutOfRange";
    case ErrorCode::IntegerOverflow:
      return "IntegerOverflow";
    case ErrorCode::InvalidUnicode:
      return "InvalidUnicode";

    // lifecycle
    case ErrorCode::IllegalStateTransition:
      return "IllegalStateTransition";
    case ErrorCode::LifecycleClosed:
      return "LifecycleClosed";
    case ErrorCode::NotReady:
      return "NotReady";
    case ErrorCode::AlreadyExists:
      return "AlreadyExists";

    // authority and generations
    case ErrorCode::StaleCoordinatorEpoch:
      return "StaleCoordinatorEpoch";
    case ErrorCode::StaleWorkerBoot:
      return "StaleWorkerBoot";
    case ErrorCode::StaleSession:
      return "StaleSession";
    case ErrorCode::StaleAuthority:
      return "StaleAuthority";
    case ErrorCode::StalePopulationGeneration:
      return "StalePopulationGeneration";
    case ErrorCode::StaleTaskGeneration:
      return "StaleTaskGeneration";
    case ErrorCode::StaleCandidateGeneration:
      return "StaleCandidateGeneration";
    case ErrorCode::StaleEvaluationGeneration:
      return "StaleEvaluationGeneration";
    case ErrorCode::StalePolicyGeneration:
      return "StalePolicyGeneration";
    case ErrorCode::StaleAttempt:
      return "StaleAttempt";
    case ErrorCode::StaleAssignment:
      return "StaleAssignment";
    case ErrorCode::StaleSelection:
      return "StaleSelection";
    case ErrorCode::SupersededCandidate:
      return "SupersededCandidate";
    case ErrorCode::RetiredLineage:
      return "RetiredLineage";
    case ErrorCode::CancelledAttempt:
      return "CancelledAttempt";
    case ErrorCode::DuplicateResult:
      return "DuplicateResult";
    case ErrorCode::LateCompletion:
      return "LateCompletion";
    case ErrorCode::UnknownWorkerIncarnation:
      return "UnknownWorkerIncarnation";
    case ErrorCode::AuthorityWithdrawn:
      return "AuthorityWithdrawn";
    case ErrorCode::RevalidationRequired:
      return "RevalidationRequired";
    case ErrorCode::GenerationRegression:
      return "GenerationRegression";

    // lineage
    case ErrorCode::LineageCycle:
      return "LineageCycle";
    case ErrorCode::LineageSelfParent:
      return "LineageSelfParent";
    case ErrorCode::LineageOrphan:
      return "LineageOrphan";
    case ErrorCode::LineageReparentForbidden:
      return "LineageReparentForbidden";

    // evaluation, selection, retention
    case ErrorCode::EvaluationIncomplete:
      return "EvaluationIncomplete";
    case ErrorCode::HardRequirementFailed:
      return "HardRequirementFailed";
    case ErrorCode::MandatoryEvidenceMissing:
      return "MandatoryEvidenceMissing";
    case ErrorCode::SelectionImpossible:
      return "SelectionImpossible";
    case ErrorCode::PolicyConflict:
      return "PolicyConflict";
    case ErrorCode::RankingFactorUnknown:
      return "RankingFactorUnknown";
    case ErrorCode::RetentionLimitExceeded:
      return "RetentionLimitExceeded";
    case ErrorCode::PromotionNotEligible:
      return "PromotionNotEligible";

    // budgets and resources
    case ErrorCode::BudgetExhausted:
      return "BudgetExhausted";
    case ErrorCode::ResourceExhausted:
      return "ResourceExhausted";
    case ErrorCode::QueueCapacityExceeded:
      return "QueueCapacityExceeded";
    case ErrorCode::OutputLimitExceeded:
      return "OutputLimitExceeded";
    case ErrorCode::AccountingImbalance:
      return "AccountingImbalance";

    // persistence
    case ErrorCode::PersistenceIoFailure:
      return "PersistenceIoFailure";
    case ErrorCode::PersistenceCorrupt:
      return "PersistenceCorrupt";
    case ErrorCode::PersistenceVersionUnsupported:
      return "PersistenceVersionUnsupported";
    case ErrorCode::PersistenceTruncated:
      return "PersistenceTruncated";
    case ErrorCode::PersistenceIntegrityMismatch:
      return "PersistenceIntegrityMismatch";
    case ErrorCode::PersistenceTrailingGarbage:
      return "PersistenceTrailingGarbage";
    case ErrorCode::PersistenceRejectedContent:
      return "PersistenceRejectedContent";

    // transport and protocol
    case ErrorCode::TransportFailure:
      return "TransportFailure";
    case ErrorCode::ProtocolViolation:
      return "ProtocolViolation";
    case ErrorCode::FrameTooLarge:
      return "FrameTooLarge";
    case ErrorCode::FrameCorrupt:
      return "FrameCorrupt";
    case ErrorCode::FrameTruncated:
      return "FrameTruncated";
    case ErrorCode::ConnectionClosed:
      return "ConnectionClosed";
    case ErrorCode::NotConnected:
      return "NotConnected";

    // filesystem and workspaces
    case ErrorCode::PathEscape:
      return "PathEscape";
    case ErrorCode::UnsafePath:
      return "UnsafePath";
    case ErrorCode::ReparsePointRejected:
      return "ReparsePointRejected";
    case ErrorCode::WorkspaceFailure:
      return "WorkspaceFailure";
    case ErrorCode::TransactionAborted:
      return "TransactionAborted";
    case ErrorCode::IoFailure:
      return "IoFailure";

    // process and lifecycle
    case ErrorCode::ProcessSpawnFailure:
      return "ProcessSpawnFailure";
    case ErrorCode::ProcessWaitFailure:
      return "ProcessWaitFailure";
    case ErrorCode::OutcomeUnknown:
      return "OutcomeUnknown";
    case ErrorCode::CoordinatorFailure:
      return "CoordinatorFailure";
    case ErrorCode::ShutdownInProgress:
      return "ShutdownInProgress";
    case ErrorCode::Cancelled:
      return "Cancelled";
    case ErrorCode::AmbiguousCompletion:
      return "AmbiguousCompletion";

    // capability
    case ErrorCode::Unsupported:
      return "Unsupported";
    case ErrorCode::Unavailable:
      return "Unavailable";

    case ErrorCode::Internal:
      return "Internal";

    default:
      return "ErrorCodeUnknown";
  }
}

bool is_stale_authority(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StaleCoordinatorEpoch:
    case ErrorCode::StaleWorkerBoot:
    case ErrorCode::StaleSession:
    case ErrorCode::StaleAuthority:
    case ErrorCode::StalePopulationGeneration:
    case ErrorCode::StaleTaskGeneration:
    case ErrorCode::StaleCandidateGeneration:
    case ErrorCode::StaleEvaluationGeneration:
    case ErrorCode::StalePolicyGeneration:
    case ErrorCode::StaleAttempt:
    case ErrorCode::StaleAssignment:
    case ErrorCode::StaleSelection:
    case ErrorCode::SupersededCandidate:
    case ErrorCode::RetiredLineage:
    case ErrorCode::CancelledAttempt:
    case ErrorCode::DuplicateResult:
    case ErrorCode::LateCompletion:
    case ErrorCode::UnknownWorkerIncarnation:
    case ErrorCode::AuthorityWithdrawn:
    case ErrorCode::RevalidationRequired:
    case ErrorCode::GenerationRegression:
      return true;
    default:
      return false;
  }
}

bool is_persistence_rejection(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::PersistenceCorrupt:
    case ErrorCode::PersistenceVersionUnsupported:
    case ErrorCode::PersistenceTruncated:
    case ErrorCode::PersistenceIntegrityMismatch:
    case ErrorCode::PersistenceTrailingGarbage:
    case ErrorCode::PersistenceRejectedContent:
      return true;
    default:
      return false;
  }
}

Status::Status(ErrorCode code, std::string_view message) : code_(code), message_(message) {}

std::string Status::to_string() const {
  std::string text(error_code_name(code_));
  if (code_ == ErrorCode::Ok) {
    return text;
  }
  text.append(": ");
  text.append(message_);
  return text;
}

Status Status::with_context(std::string_view context) const {
  std::string text(context);
  text.append(": ");
  text.append(message_);
  return Status(code_, std::string_view(text));
}

}  // namespace autonomous_foundry
