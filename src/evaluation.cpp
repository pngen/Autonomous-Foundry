// src/evaluation.cpp
//
// Evaluation evidence: enumerator names, the authority question, structural
// validation and the aggregate evidence view.
//
// Authority is the point of this file. A record is not a boolean and not a
// score, and an outcome is not a permission. A worker's own claim about its
// output is recorded as evidence about that worker, but it can never satisfy a
// mandatory requirement, because the producing process is not a disinterested
// party. Only an evaluator that the runtime itself drove -- a launched process,
// a static rule applied to artifacts, or a deterministic in-process reference
// computation -- may turn a complete PASS into mandatory authority.
//
// Evaluator keys are validated with exactly the same rule as the task side
// (non-empty, at most kMaxNameLength bytes, ASCII letters, digits, '.', '_'
// and '-'), so a key that a task can declare is a key that evidence can be
// filed under, and no other spelling can be looked up.

#include "autonomous_foundry/evaluation.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "autonomous_foundry/hash.hpp"

namespace autonomous_foundry {

namespace {

constexpr std::size_t kEvidenceDigestHexLength = 64;

std::string hex_byte(unsigned char value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string text(2, '0');
  text[0] = kDigits[(value >> 4) & 0x0Fu];
  text[1] = kDigits[value & 0x0Fu];
  return text;
}

Status require_identity(bool valid, std::string_view field) {
  if (!valid) {
    return Status(ErrorCode::NullIdentity, std::string(field) + " is the null identity");
  }
  return Status();
}

Status require_generation(bool valid, std::string_view field) {
  if (!valid) {
    return Status(ErrorCode::InvalidArgument,
                  std::string(field) + " is generation 0, which is not a valid generation");
  }
  return Status();
}

}  // namespace

std::string_view evaluator_kind_name(EvaluatorKind kind) noexcept {
  switch (kind) {
    case EvaluatorKind::ProcessCommand:
      return "ProcessCommand";
    case EvaluatorKind::SourcePolicy:
      return "SourcePolicy";
    case EvaluatorKind::PortableReference:
      return "PortableReference";
    case EvaluatorKind::WorkerSelfReport:
      return "WorkerSelfReport";
  }
  return "Unknown";
}

std::string_view evaluation_outcome_name(EvaluationOutcome outcome) noexcept {
  switch (outcome) {
    case EvaluationOutcome::Pass:
      return "Pass";
    case EvaluationOutcome::Fail:
      return "Fail";
    case EvaluationOutcome::Unknown:
      return "Unknown";
    case EvaluationOutcome::Unsupported:
      return "Unsupported";
    case EvaluationOutcome::Error:
      return "Error";
    case EvaluationOutcome::Cancelled:
      return "Cancelled";
  }
  return "Unknown";
}

bool outcome_satisfies_mandatory(EvaluationOutcome outcome) noexcept {
  return outcome == EvaluationOutcome::Pass;
}

bool evaluator_kind_is_authoritative(EvaluatorKind kind) noexcept {
  switch (kind) {
    case EvaluatorKind::ProcessCommand:
      return true;
    case EvaluatorKind::SourcePolicy:
      return true;
    case EvaluatorKind::PortableReference:
      return true;
    case EvaluatorKind::WorkerSelfReport:
      // The worker that produced the artifact is not a disinterested party.
      // Its report is evidence about the worker, never authority over the gate.
      return false;
  }
  return false;
}

Status validate_evaluation_record(const EvaluationRecord& record) {
  AF_TRY(require_identity(record.id.valid(), "evaluation id"));
  AF_TRY(require_generation(record.generation.valid(), "evaluation generation"));

  AF_TRY(require_identity(record.candidate.valid(), "evaluation candidate id"));
  AF_TRY(require_generation(record.candidate_generation.valid(),
                            "evaluation candidate generation"));
  AF_TRY(require_identity(record.task.valid(), "evaluation task id"));
  AF_TRY(require_generation(record.task_generation.valid(), "evaluation task generation"));
  AF_TRY(require_identity(record.population.valid(), "evaluation population id"));
  AF_TRY(require_generation(record.population_generation.valid(),
                            "evaluation population generation"));
  AF_TRY(require_identity(record.evaluator.valid(), "evaluation evaluator id"));

  if (record.evaluator_key.empty()) {
    return Status(ErrorCode::InvalidArgument,
                  std::string_view("evaluation evaluator key must not be empty; evidence that "
                                   "cannot be looked up by key cannot satisfy a declared "
                                   "requirement"));
  }
  if (record.evaluator_key.size() > kMaxNameLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "evaluation evaluator key is " + std::to_string(record.evaluator_key.size()) +
                      " bytes long, which exceeds the maximum of " +
                      std::to_string(kMaxNameLength) + " bytes");
  }
  for (std::size_t index = 0; index < record.evaluator_key.size(); ++index) {
    const char ch = record.evaluator_key[index];
    const bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    const bool digit = ch >= '0' && ch <= '9';
    if (!(letter || digit || ch == '.' || ch == '_' || ch == '-')) {
      return Status(ErrorCode::InvalidArgument,
                    "evaluation evaluator key contains byte 0x" +
                        hex_byte(static_cast<unsigned char>(ch)) + " at index " +
                        std::to_string(index) +
                        "; only ASCII letters, digits, '.', '_' and '-' are allowed");
    }
  }

  if (record.diagnostics.size() > kMaxDiagnosticsLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "evaluation diagnostics are " + std::to_string(record.diagnostics.size()) +
                      " bytes long, which exceeds the maximum of " +
                      std::to_string(kMaxDiagnosticsLength) + " bytes");
  }

  if (record.has_score && !std::isfinite(record.score)) {
    return Status(ErrorCode::InvalidArgument,
                  "evaluation record for key '" + record.evaluator_key +
                      "' carries a score that is not finite");
  }

  if (!record.evidence_digest.empty()) {
    if (record.evidence_digest.size() != kEvidenceDigestHexLength) {
      return Status(ErrorCode::MalformedEncoding,
                    "evaluation evidence digest must be " +
                        std::to_string(kEvidenceDigestHexLength) +
                        " lowercase hexadecimal characters but is " +
                        std::to_string(record.evidence_digest.size()) + " characters");
    }
    if (!is_lowercase_hex(record.evidence_digest, kEvidenceDigestHexLength)) {
      return Status(ErrorCode::MalformedEncoding,
                    "evaluation evidence digest is not lowercase hexadecimal: '" +
                        record.evidence_digest + "'");
    }
  }

  if (record.complete && !record.decided_epoch.valid()) {
    return Status(ErrorCode::MalformedEncoding,
                  "evaluation record for key '" + record.evaluator_key +
                      "' is complete but carries no coordinator epoch; a terminal record must "
                      "name the epoch under which it became terminal");
  }

  if (static_cast<std::uint8_t>(record.kind) >= kEvaluatorKindCount) {
    return Status(ErrorCode::InvalidArgument,
                  "evaluation record for key '" + record.evaluator_key +
                      "' names evaluator kind ordinal " +
                      std::to_string(static_cast<std::uint32_t>(record.kind)) +
                      ", which is not a known evaluator kind");
  }
  if (static_cast<std::uint8_t>(record.outcome) >= kEvaluationOutcomeCount) {
    return Status(ErrorCode::InvalidArgument,
                  "evaluation record for key '" + record.evaluator_key +
                      "' names outcome ordinal " +
                      std::to_string(static_cast<std::uint32_t>(record.outcome)) +
                      ", which is not a known evaluation outcome");
  }
  return Status();
}

const EvaluationRecord* CandidateEvidence::find(std::string_view evaluator_key) const noexcept {
  for (const EvaluationRecord& record : records) {
    // Evidence is bound to the exact candidate generation it was produced for.
    // A record that describes an older generation is not evidence about this
    // one, however complete, authoritative and passing it may be: accepting it
    // would let a rebuilt candidate inherit the verdict of the output it
    // replaced.
    if (record.candidate_generation != candidate_generation) {
      continue;
    }
    if (std::string_view(record.evaluator_key) == evaluator_key && record.complete &&
        evaluator_kind_is_authoritative(record.kind)) {
      return &record;
    }
  }
  return nullptr;
}

bool CandidateEvidence::has_complete_authoritative_pass(
    std::string_view evaluator_key) const noexcept {
  const EvaluationRecord* record = find(evaluator_key);
  return record != nullptr && record->authoritative_for_mandatory();
}

}  // namespace autonomous_foundry
