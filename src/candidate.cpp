// src/candidate.cpp
//
// Candidate records: enumerator names, lifecycle legality, structural
// validation and the explicitly labelled reference diversity key.
//
// "Candidate exists" and "candidate is valid" are separate facts. Nothing in
// this file ever infers the second from the first: validate_candidate_record
// re-derives every structural property from the record itself and reports the
// first violation it finds with a concrete, machine-readable code.

#include "autonomous_foundry/candidate.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/limits.hpp"

namespace autonomous_foundry {

namespace {

/// Maximum number of parents a candidate may declare. The private foundry core
/// header defines the same bound; it is repeated here so that this translation
/// unit never depends on a private header.
inline constexpr std::size_t kMaxCandidateParents = 8;

/// Maximum number of production attempts that may be spent on one candidate
/// slot before the record is considered structurally impossible.
inline constexpr std::uint32_t kMaxAttemptCount = 1024;

/// Parse exactly eight lowercase hexadecimal digits into a 32-bit value.
/// Returns false for any other character, so a malformed digest can never be
/// silently reinterpreted as a bucket.
[[nodiscard]] bool parse_first_hex32(std::string_view text, std::uint32_t& out) noexcept {
  if (text.size() < 8) {
    return false;
  }
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    const char ch = text[index];
    unsigned digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<unsigned>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<unsigned>(ch - 'a') + 10u;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  out = value;
  return true;
}

}  // namespace

std::string_view candidate_state_name(CandidateState state) noexcept {
  switch (state) {
    case CandidateState::Registered:
      return "Registered";
    case CandidateState::Producing:
      return "Producing";
    case CandidateState::Published:
      return "Published";
    case CandidateState::Evaluating:
      return "Evaluating";
    case CandidateState::Evaluated:
      return "Evaluated";
    case CandidateState::RevalidationRequired:
      return "RevalidationRequired";
    case CandidateState::Selected:
      return "Selected";
    case CandidateState::Retained:
      return "Retained";
    case CandidateState::Retired:
      return "Retired";
    case CandidateState::Superseded:
      return "Superseded";
    case CandidateState::ProductionFailed:
      return "ProductionFailed";
    case CandidateState::ProductionCancelled:
      return "ProductionCancelled";
    case CandidateState::Disqualified:
      return "Disqualified";
  }
  return "Unknown";
}

bool candidate_state_is_terminal(CandidateState state) noexcept {
  switch (state) {
    case CandidateState::Retired:
    case CandidateState::Superseded:
    case CandidateState::ProductionFailed:
    case CandidateState::ProductionCancelled:
    case CandidateState::Disqualified:
      return true;
    case CandidateState::Registered:
    case CandidateState::Producing:
    case CandidateState::Published:
    case CandidateState::Evaluating:
    case CandidateState::Evaluated:
    case CandidateState::RevalidationRequired:
    case CandidateState::Selected:
    case CandidateState::Retained:
      return false;
  }
  return false;
}

bool candidate_transition_is_legal(CandidateState from, CandidateState to) noexcept {
  // A self transition is always legal: it is how a caller records "the
  // candidate is still where the runtime already said it was" without
  // inventing a new state.
  if (from == to) {
    return true;
  }
  switch (from) {
    case CandidateState::Registered:
      switch (to) {
        case CandidateState::Producing:
        case CandidateState::ProductionFailed:
        case CandidateState::ProductionCancelled:
        case CandidateState::Disqualified:
          return true;
        default:
          return false;
      }
    case CandidateState::Producing:
      switch (to) {
        case CandidateState::Registered:
        case CandidateState::Published:
        case CandidateState::ProductionFailed:
        case CandidateState::ProductionCancelled:
          return true;
        default:
          return false;
      }
    case CandidateState::Published:
      switch (to) {
        case CandidateState::Evaluating:
        case CandidateState::RevalidationRequired:
        case CandidateState::Superseded:
        case CandidateState::Disqualified:
        case CandidateState::ProductionCancelled:
          return true;
        default:
          return false;
      }
    case CandidateState::Evaluating:
      switch (to) {
        case CandidateState::Evaluated:
        case CandidateState::RevalidationRequired:
        case CandidateState::Disqualified:
        case CandidateState::Superseded:
        // An explicit cancellation is final even while the candidate is under
        // evaluation: otherwise a cancelled attempt still leaves the candidate
        // able to reach Evaluated and enter ranking.
        case CandidateState::ProductionCancelled:
          return true;
        default:
          return false;
      }
    case CandidateState::Evaluated:
      switch (to) {
        case CandidateState::Selected:
        case CandidateState::Retained:
        case CandidateState::Retired:
        case CandidateState::Disqualified:
        case CandidateState::Superseded:
        case CandidateState::RevalidationRequired:
          return true;
        default:
          return false;
      }
    case CandidateState::RevalidationRequired:
      switch (to) {
        case CandidateState::Evaluating:
        case CandidateState::Evaluated:
        case CandidateState::Retired:
        case CandidateState::Superseded:
        case CandidateState::Disqualified:
          return true;
        default:
          return false;
      }
    case CandidateState::Selected:
      switch (to) {
        case CandidateState::Retained:
        case CandidateState::Retired:
        case CandidateState::Superseded:
          return true;
        default:
          return false;
      }
    case CandidateState::Retained:
      switch (to) {
        case CandidateState::Selected:
        case CandidateState::Retired:
        case CandidateState::Superseded:
          return true;
        default:
          return false;
      }
    case CandidateState::Retired:
    case CandidateState::Superseded:
    case CandidateState::ProductionFailed:
    case CandidateState::ProductionCancelled:
    case CandidateState::Disqualified:
      // Terminal states admit no successor other than themselves. Published
      // history is never rewritten into a different terminal outcome.
      return false;
  }
  return false;
}

Status validate_candidate_record(const CandidateRecord& record) {
  if (!record.id.valid()) {
    return Status(ErrorCode::NullIdentity, std::string_view{"candidate id is the null identity"});
  }
  if (!record.generation.valid()) {
    return Status(ErrorCode::InvalidArgument,
                  "candidate " + record.id.to_string() +
                      " carries generation 0, which is never a valid generation");
  }
  if (!record.population.valid()) {
    return Status(ErrorCode::NullIdentity,
                  "candidate " + record.id.to_string() +
                      " names the null population identity");
  }
  if (!record.population_generation.valid()) {
    return Status(ErrorCode::InvalidArgument,
                  "candidate " + record.id.to_string() +
                      " carries population generation 0, which is never a valid generation");
  }
  if (!record.task.valid()) {
    return Status(ErrorCode::NullIdentity,
                  "candidate " + record.id.to_string() + " names the null task identity");
  }
  if (!record.task_generation.valid()) {
    return Status(ErrorCode::InvalidArgument,
                  "candidate " + record.id.to_string() +
                      " carries task generation 0, which is never a valid generation");
  }
  if (!record.lineage.valid()) {
    return Status(ErrorCode::NullIdentity,
                  "candidate " + record.id.to_string() + " names the null lineage identity");
  }

  if (record.parents.size() > kMaxCandidateParents) {
    return Status(ErrorCode::LengthOutOfRange,
                  "candidate " + record.id.to_string() + " declares " +
                      std::to_string(record.parents.size()) + " parents, which exceeds the maximum of " +
                      std::to_string(kMaxCandidateParents));
  }
  for (std::size_t index = 0; index < record.parents.size(); ++index) {
    const CandidateId parent = record.parents[index];
    if (!parent.valid()) {
      return Status(ErrorCode::NullIdentity,
                    "candidate " + record.id.to_string() + " parent[" + std::to_string(index) +
                        "] is the null identity");
    }
    if (parent == record.id) {
      return Status(ErrorCode::LineageSelfParent,
                    "candidate " + record.id.to_string() + " declares itself as parent[" +
                        std::to_string(index) + "]");
    }
  }
  if (record.depth > kMaxLineageDepth) {
    return Status(ErrorCode::LengthOutOfRange,
                  "candidate " + record.id.to_string() + " declares lineage depth " +
                      std::to_string(record.depth) + ", which exceeds the maximum of " +
                      std::to_string(kMaxLineageDepth));
  }

  if (record.attempt_count > kMaxAttemptCount) {
    return Status(ErrorCode::LengthOutOfRange,
                  "candidate " + record.id.to_string() + " reports " +
                      std::to_string(record.attempt_count) +
                      " production attempts, which exceeds the maximum of " +
                      std::to_string(kMaxAttemptCount));
  }

  if (record.artifacts.size() > kMaxArtifactsPerCandidate) {
    return Status(ErrorCode::LengthOutOfRange,
                  "candidate " + record.id.to_string() + " declares " +
                      std::to_string(record.artifacts.size()) +
                      " artifact references, which exceeds the maximum of " +
                      std::to_string(kMaxArtifactsPerCandidate));
  }
  for (const ArtifactRef& artifact : record.artifacts) {
    AF_TRY(validate_artifact_ref(artifact));
  }

  if (record.artifacts.empty()) {
    if (!record.artifact_set_digest.empty()) {
      return Status(ErrorCode::MalformedEncoding,
                    "candidate " + record.id.to_string() +
                        " declares no artifacts but carries the artifact set digest '" +
                        record.artifact_set_digest + "'");
    }
  } else {
    const std::string expected = artifact_set_digest(record.artifacts);
    if (record.artifact_set_digest != expected) {
      return Status(ErrorCode::MalformedEncoding,
                    "candidate " + record.id.to_string() + " declares artifact set digest '" +
                        record.artifact_set_digest + "' but its artifact references encode to '" +
                        expected + "'");
    }
  }

  if (record.failure_reason.size() > kMaxDiagnosticsLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "candidate " + record.id.to_string() + " failure reason is " +
                      std::to_string(record.failure_reason.size()) +
                      " bytes long, which exceeds the maximum of " +
                      std::to_string(kMaxDiagnosticsLength) + " bytes");
  }
  if (record.superseded_by_reason.size() > kMaxDiagnosticsLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "candidate " + record.id.to_string() + " superseded-by reason is " +
                      std::to_string(record.superseded_by_reason.size()) +
                      " bytes long, which exceeds the maximum of " +
                      std::to_string(kMaxDiagnosticsLength) + " bytes");
  }
  return Status();
}

std::string reference_diversity_key(std::string_view declared_strategy,
                                    const std::vector<ArtifactRef>& artifacts) {
  // REFERENCE / SYNTHETIC SIGNAL.
  //
  // This key is derived from two declared, structural facts and nothing else:
  // the strategy label the producer declared, and a four-way bucket of the
  // first published artifact's content digest. It measures whether two
  // candidates were produced under a different declared strategy label or
  // landed in a different digest bucket. It is NOT semantic novelty, NOT an
  // embedding distance, and NOT a statement that one solution differs from
  // another in what it actually does. Consumers must treat it as a cheap
  // reference signal for lineage-diversity accounting only.
  std::string key(declared_strategy);
  key.push_back('-');
  if (artifacts.empty()) {
    key.append("empty");
    return key;
  }

  std::uint32_t digest_prefix = 0;
  const bool parsed = parse_first_hex32(artifacts.front().content_digest, digest_prefix);
  if (!parsed) {
    // A digest that is not eight lowercase hexadecimal characters cannot be
    // bucketed. Folding it onto bucket 0 would silently claim a measurement
    // that was never made.
    key.append("unbucketable");
    return key;
  }

  const std::uint32_t bucket = digest_prefix % 4u;
  key.append("b");
  key.append(std::to_string(bucket));
  return key;
}

}  // namespace autonomous_foundry