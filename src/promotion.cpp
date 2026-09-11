// src/promotion.cpp
//
// Promotion eligibility and the canonical promotion-request encoding.
//
// Autonomous Foundry does not promote artifacts. It determines that a selected
// candidate is eligible to be handed to an independent promotion system and
// emits a request that the receiver can re-verify on its own evidence. The
// vocabulary in this file deliberately has no "Promoted" value: only the
// receiving system may claim that, and it does so outside this runtime.
//
// Canonical encoding
// ------------------
//
// encode_promotion_request_canonical emits an explicit, field-tagged,
// length-prefixed byte string. Two field shapes exist and nothing else:
//
//     <tag> '=' <decimal byte length> ':' <raw value bytes> ';'   (text)
//     <tag> '=' <decimal value> ';'                               (integer)
//
// Tags are fixed ASCII names that never contain '=', ':' or ';'. A tag appears
// at most once, except for the tags inside a repeated element group, and every
// element group of one vector is preceded by a count field. The length of a
// text field counts raw value bytes, so a value may itself contain '=', ':' or
// ';' without making the encoding ambiguous, and nothing is escaped or
// normalised. Enumerator fields are emitted as their decimal ordinal, so a
// value that arrived out of range from a snapshot still encodes distinctly.
// Identity and generation fields are emitted as decimal raw values: the null
// identity is 0 and the invalid generation is 0, so an unset field is visible
// in the bytes rather than silently absent.
//
// The order of a vector in memory is not part of the logical value, so the two
// order-dependent vectors are sorted before they are emitted:
//
//   * mandatory_evidence by (evaluator_key, generation.value())
//   * artifacts by (name, size_bytes, content_digest)
//
// Two requests that differ only in the order the caller supplied those
// elements therefore produce byte-identical encodings and an identical digest.
//
// Field sequence emitted by encode_promotion_request_canonical:
//
//   request_id, foundry, run, coordinator_epoch,
//   population, population_generation,
//   candidate, candidate_generation,
//   task, task_generation,
//   lineage, selection_generation, policy, policy_generation,
//   evidence_generation,
//   eligibility                      decimal ordinal
//   outstanding_count                decimal
//   requirement                      text    } repeated outstanding_count
//                                            } times, in the declared order,
//                                            } which is the order the
//                                            } requirements were evaluated
//   artifact_count                   decimal
//   artifact_name                    text    } repeated artifact_count times
//   artifact_size_bytes              decimal } after sorting by (name,
//   artifact_content_digest          text    } size_bytes, content_digest)
//   total_artifact_bytes             decimal
//   artifact_set_digest              text
//   mandatory_evidence_count         decimal
//   evidence_key                     text    } repeated
//   evidence_kind                    decimal } mandatory_evidence_count
//   evidence_outcome                 decimal } times after sorting by
//   evidence_requirement_class       decimal } (evaluator_key,
//   evidence_complete                decimal } generation.value())
//   evidence_has_score               decimal
//   evidence_score                   binary64, 16 lowercase hex digits, emitted
//                                            only when has_score is 1
//   evidence_generation              decimal
//   evidence_digest                  text
//   ancestry_count                   decimal
//   ancestor                         decimal } repeated ancestry_count times,
//                                            } root first: the order is part of
//                                            } the provenance value
//   lineage_depth                    decimal
//
// PromotionRequest::canonical_state_digest is deliberately absent: it is a hash
// output, and including it would be circular.

#include "autonomous_foundry/promotion.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"

namespace autonomous_foundry {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::size_t kBinary64HexDigits = 16;
constexpr std::uint64_t kNegativeZeroBits = 0x8000000000000000ull;

static_assert(sizeof(double) == sizeof(std::uint64_t),
              "canonical encoding requires IEEE-754 binary64 doubles");

void append_field(std::string& out, std::string_view tag, std::string_view value) {
  out.append(tag);
  out.push_back('=');
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

void append_u64(std::string& out, std::string_view tag, std::uint64_t value) {
  append_field(out, tag, std::to_string(value));
}

void append_u32(std::string& out, std::string_view tag, std::uint32_t value) {
  append_field(out, tag, std::to_string(value));
}

void append_bool(std::string& out, std::string_view tag, bool value) {
  append_field(out, tag, value ? "1" : "0");
}

void append_count(std::string& out, std::string_view tag, std::size_t value) {
  append_field(out, tag, std::to_string(value));
}

void append_binary64(std::string& out, std::string_view tag, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  if (bits == kNegativeZeroBits) {
    bits = 0;
  }
  std::string digits(kBinary64HexDigits, '0');
  for (std::size_t index = 0; index < digits.size(); ++index) {
    const unsigned shift = static_cast<unsigned>((digits.size() - 1 - index) * 4);
    digits[index] = kHexDigits[(bits >> shift) & 0x0Full];
  }
  append_field(out, tag, digits);
}

/// Ascending order by (evaluator_key, generation) -- the identity of a piece of
/// evidence, independent of the order the caller supplied it in.
[[nodiscard]] bool evidence_order(const PromotionEvidenceSummary& a,
                                  const PromotionEvidenceSummary& b) {
  if (a.evaluator_key != b.evaluator_key) {
    return a.evaluator_key < b.evaluator_key;
  }
  return a.generation.value() < b.generation.value();
}

/// Ascending order by (name, size_bytes, content_digest): the same total order
/// ArtifactRef::operator< defines, spelled out here so the artifact encoding
/// contract is visible in this file.
[[nodiscard]] bool artifact_order(const ArtifactRef& a, const ArtifactRef& b) {
  if (a.name != b.name) {
    return a.name < b.name;
  }
  if (a.size_bytes != b.size_bytes) {
    return a.size_bytes < b.size_bytes;
  }
  return a.content_digest < b.content_digest;
}

}  // namespace

std::string_view promotion_eligibility_state_name(PromotionEligibilityState state) noexcept {
  switch (state) {
    case PromotionEligibilityState::NotEvaluated:
      return "NotEvaluated";
    case PromotionEligibilityState::Eligible:
      return "Eligible";
    case PromotionEligibilityState::NotEligible:
      return "NotEligible";
    case PromotionEligibilityState::Withdrawn:
      return "Withdrawn";
  }
  return "Unknown";
}

std::string_view promotion_handoff_state_name(PromotionHandoffState state) noexcept {
  switch (state) {
    case PromotionHandoffState::NotRequested:
      return "NotRequested";
    case PromotionHandoffState::Requested:
      return "Requested";
    case PromotionHandoffState::AcceptedForProcessing:
      return "AcceptedForProcessing";
    case PromotionHandoffState::DeclinedBySink:
      return "DeclinedBySink";
    case PromotionHandoffState::SinkUnavailable:
      return "SinkUnavailable";
  }
  return "Unknown";
}

PromotionSink::~PromotionSink() = default;

std::string encode_promotion_request_canonical(const PromotionRequest& request) {
  std::string out;

  append_u64(out, "request_id", request.id.raw());
  append_u64(out, "foundry", request.foundry.raw());
  append_u64(out, "run", request.run.raw());
  append_u64(out, "coordinator_epoch", request.coordinator_epoch.value());

  append_u64(out, "population", request.population.raw());
  append_u32(out, "population_generation", request.population_generation.value());
  append_u64(out, "candidate", request.candidate.raw());
  append_u32(out, "candidate_generation", request.candidate_generation.value());
  append_u64(out, "task", request.task.raw());
  append_u32(out, "task_generation", request.task_generation.value());
  append_u64(out, "lineage", request.lineage.raw());
  append_u32(out, "selection_generation", request.selection_generation.value());
  append_u64(out, "policy", request.policy.raw());
  append_u32(out, "policy_generation", request.policy_generation.value());
  append_u32(out, "evidence_generation", request.evidence_generation.value());

  append_u32(out, "eligibility", static_cast<std::uint32_t>(request.eligibility));

  append_count(out, "outstanding_count", request.outstanding_requirements.size());
  for (const std::string& requirement : request.outstanding_requirements) {
    append_field(out, "requirement", requirement);
  }

  std::vector<ArtifactRef> artifacts = request.artifacts;
  std::sort(artifacts.begin(), artifacts.end(), artifact_order);
  append_count(out, "artifact_count", artifacts.size());
  for (const ArtifactRef& artifact : artifacts) {
    append_field(out, "artifact_name", artifact.name);
    append_u64(out, "artifact_size_bytes", artifact.size_bytes);
    append_field(out, "artifact_content_digest", artifact.content_digest);
  }
  append_u64(out, "total_artifact_bytes", request.total_artifact_bytes);
  append_field(out, "artifact_set_digest", request.artifact_set_digest);

  std::vector<PromotionEvidenceSummary> evidence = request.mandatory_evidence;
  std::sort(evidence.begin(), evidence.end(), evidence_order);
  append_count(out, "mandatory_evidence_count", evidence.size());
  for (const PromotionEvidenceSummary& summary : evidence) {
    append_field(out, "evidence_key", summary.evaluator_key);
    append_u32(out, "evidence_kind", static_cast<std::uint32_t>(summary.kind));
    append_u32(out, "evidence_outcome", static_cast<std::uint32_t>(summary.outcome));
    append_u32(out, "evidence_requirement_class",
               static_cast<std::uint32_t>(summary.requirement_class));
    append_bool(out, "evidence_complete", summary.complete);
    append_bool(out, "evidence_has_score", summary.has_score);
    if (summary.has_score) {
      append_binary64(out, "evidence_score", summary.score);
    }
    append_u32(out, "evidence_generation", summary.generation.value());
    append_field(out, "evidence_digest", summary.evidence_digest);
  }

  append_count(out, "ancestry_count", request.ancestry.size());
  for (const CandidateId ancestor : request.ancestry) {
    append_u64(out, "ancestor", ancestor.raw());
  }
  append_u32(out, "lineage_depth", request.lineage_depth);
  return out;
}

}  // namespace autonomous_foundry
