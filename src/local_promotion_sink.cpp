// src/local_promotion_sink.cpp
//
// The reference promotion sink: "local-file-sink".
//
// It records the handoff. It does not promote anything.
//
// ---------------------------------------------------------------------------
// File contract
// ---------------------------------------------------------------------------
//
// For a request with identity R:
//
//   <dir>/<R>.json                  canonical rendering of the request
//   <dir>/<R>.receipt.json          canonical rendering of the receipt
//
// where <R> is the lowercase hexadecimal form of the raw 64-bit request
// identity with no leading zeroes.
//
// The request file is written first and the receipt second, so a crash between
// the two leaves a request with no receipt - which reads as "handed over, no
// processing decision recorded yet" - rather than a receipt that claims a
// request nobody can find. Both files are written with atomic_write_file, so a
// concurrent reader sees either the old content or the new content and never a
// partial file.
//
// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
//
// The rendering is a flat, deterministic, JSON-shaped text document. It is
// produced here rather than delegated to the runtime's canonical byte encoding
// because a human has to be able to read a promotion request in the field, and
// because JSON-shaped output can be consumed by an adjacent promotion system
// without a bespoke parser. It is still canonical: every repeated field is
// emitted in a fixed order, every number is rendered with a locale independent
// convention, and every string is escaped so that no byte inside a value can be
// mistaken for structure.
//
//   * identities are rendered as their stable textual form, for example
//     "CandidateId:72620543991349305", alongside the raw value that an adjacent
//     system actually needs to compare.
//   * enumerators are rendered as their stable textual name and as their
//     decimal ordinal, so a value that arrived out of range is still visible
//     rather than being silently relabelled.
//   * an unset identity or generation renders as null rather than as an empty
//     string, so "absent" and "present but empty" stay distinguishable.
//   * binary64 scores are rendered as their exact IEEE-754 bit pattern in
//     hexadecimal, because a JSON decimal rendering is not exactly round
//     trippable through every JSON reader.
//
// The canonical_encoding object carries the SHA-256 of the runtime's canonical
// bytes (encode_promotion_request_canonical). A receiver can therefore verify
// that the file it read is exactly the request the runtime produced without
// having to reimplement this renderer.
//
// ASCII only, by construction: every byte of worker-influenced content that is
// not printable ASCII is emitted as an escape sequence.

#include "local_promotion_sink.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/workspace.hpp"

namespace autonomous_foundry {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::size_t kBinary64HexDigits = 16;
constexpr std::size_t kIndentWidth = 2;

void append_indent(std::string& out, std::size_t depth) { out.append(depth * kIndentWidth, ' '); }

void append_hex_byte(std::string& out, unsigned value) {
  out.push_back(kHexDigits[(value >> 4) & 0x0Fu]);
  out.push_back(kHexDigits[value & 0x0Fu]);
}

/// Render bytes as a JSON string. Printable ASCII passes through unchanged;
/// everything else becomes an escape, so the rendering is ASCII by
/// construction and never contains a raw control byte.
void append_json_string(std::string& out, std::string_view value) {
  out.push_back('"');
  for (const char raw : value) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':
        out.append("\\\"");
        continue;
      case '\\':
        out.append("\\\\");
        continue;
      case '\b':
        out.append("\\b");
        continue;
      case '\f':
        out.append("\\f");
        continue;
      case '\n':
        out.append("\\n");
        continue;
      case '\r':
        out.append("\\r");
        continue;
      case '\t':
        out.append("\\t");
        continue;
      default:
        break;
    }
    if (byte < 0x20u || byte >= 0x7Fu) {
      out.append("\\u00");
      append_hex_byte(out, byte);
      continue;
    }
    out.push_back(static_cast<char>(byte));
  }
  out.push_back('"');
}

/// Open a new member: a leading comma unless this is the first member.
void begin_member(std::string& out, std::size_t depth, std::string_view name, bool first) {
  if (!first) {
    out.push_back(',');
  }
  out.push_back('\n');
  append_indent(out, depth);
  append_json_string(out, name);
  out.append(": ");
}

void append_field(std::string& out, std::size_t depth, std::string_view name, std::string_view raw,
                  bool first) {
  begin_member(out, depth, name, first);
  append_json_string(out, raw);
}

void append_unsigned_field(std::string& out, std::size_t depth, std::string_view name,
                           std::uint64_t value, bool first) {
  begin_member(out, depth, name, first);
  out.append(std::to_string(value));
}

void append_boolean_field(std::string& out, std::size_t depth, std::string_view name, bool value,
                          bool first) {
  begin_member(out, depth, name, first);
  out.append(value ? "true" : "false");
}

/// An identity is either present - kind, raw value and stable text - or null.
void append_identity_field(std::string& out, std::size_t depth, std::string_view name,
                           std::uint64_t raw, std::string_view kind_name) {
  begin_member(out, depth, name, false);
  if (raw == 0) {
    out.append("null");
    return;
  }
  out.push_back('{');
  append_field(out, depth + 1, "kind", kind_name, true);
  append_unsigned_field(out, depth + 1, "raw", raw, false);
  append_field(out, depth + 1, "text", std::string(kind_name) + ":" + std::to_string(raw), false);
  out.push_back('\n');
  append_indent(out, depth);
  out.push_back('}');
}

/// A generation is either present as a decimal value or null.
void append_generation_field(std::string& out, std::size_t depth, std::string_view name,
                             std::uint32_t value) {
  begin_member(out, depth, name, false);
  if (value == 0) {
    out.append("null");
    return;
  }
  out.append(std::to_string(value));
}

/// An enumerator is rendered as its stable name plus its ordinal, so an
/// out-of-range value stays observable instead of being relabelled.
void append_enum_field(std::string& out, std::size_t depth, std::string_view name,
                       std::string_view text, std::uint32_t ordinal, bool first) {
  begin_member(out, depth, name, first);
  out.push_back('{');
  append_field(out, depth + 1, "name", text, true);
  append_unsigned_field(out, depth + 1, "ordinal", ordinal, false);
  out.push_back('\n');
  append_indent(out, depth);
  out.push_back('}');
}

/// Exact binary64 rendering: the sixteen hexadecimal digits of the bit pattern.
[[nodiscard]] std::string binary64_hex(double value) {
  static_assert(sizeof(double) == sizeof(std::uint64_t),
                "binary64 rendering requires 64-bit doubles");
  std::uint64_t bits = 0;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&value);
  for (std::size_t index = 0; index < sizeof(bits); ++index) {
    bits |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  std::string digits(kBinary64HexDigits, '0');
  for (std::size_t index = 0; index < digits.size(); ++index) {
    const unsigned shift = static_cast<unsigned>((digits.size() - 1 - index) * 4);
    digits[index] = kHexDigits[(bits >> shift) & 0x0Full];
  }
  return digits;
}

[[nodiscard]] const char* requirement_class_text(RequirementClass value) {
  switch (value) {
    case RequirementClass::Mandatory:
      return "Mandatory";
    case RequirementClass::Optional:
      return "Optional";
  }
  return "Unknown";
}

struct ArtifactLess {
  [[nodiscard]] bool operator()(const ArtifactRef& a, const ArtifactRef& b) const noexcept {
    if (a.name != b.name) {
      return a.name < b.name;
    }
    if (a.size_bytes != b.size_bytes) {
      return a.size_bytes < b.size_bytes;
    }
    return a.content_digest < b.content_digest;
  }
};

struct EvidenceLess {
  [[nodiscard]] bool operator()(const PromotionEvidenceSummary& a,
                                const PromotionEvidenceSummary& b) const noexcept {
    if (a.evaluator_key != b.evaluator_key) {
      return a.evaluator_key < b.evaluator_key;
    }
    return a.generation.value() < b.generation.value();
  }
};

/// File-name stem for a request identity: the decimal raw value. A null
/// identity still produces a usable name rather than an empty one.
[[nodiscard]] std::string identity_stem(PromotionRequestId id) {
  if (!id.valid()) {
    return "null-identity";
  }
  return std::to_string(id.raw());
}

/// The full canonical text rendering of one promotion request.
[[nodiscard]] std::string render_request(const PromotionRequest& request,
                                         std::string_view canonical_encoding_digest,
                                         std::size_t canonical_encoding_bytes) {
  std::string out;
  out.append("{\n");
  append_field(out, 1, "document", "autonomous-foundry.promotion-request", true);
  append_unsigned_field(out, 1, "document_version", 1, false);
  append_field(out, 1, "runtime_claim", "eligible-for-handoff; not promoted by this runtime", false);

  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "identity");
  out.append(": {");
  append_identity_field(out, 2, "request", request.id.raw(), "PromotionRequestId");
  append_identity_field(out, 2, "foundry", request.foundry.raw(), "FoundryId");
  append_identity_field(out, 2, "run", request.run.raw(), "FoundryRunId");
  append_unsigned_field(out, 2, "coordinator_epoch", request.coordinator_epoch.value(), false);
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');

  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "subject");
  out.append(": {");
  append_identity_field(out, 2, "population", request.population.raw(), "PopulationId");
  append_generation_field(out, 2, "population_generation", request.population_generation.value());
  append_identity_field(out, 2, "candidate", request.candidate.raw(), "CandidateId");
  append_generation_field(out, 2, "candidate_generation", request.candidate_generation.value());
  append_identity_field(out, 2, "task", request.task.raw(), "TaskId");
  append_generation_field(out, 2, "task_generation", request.task_generation.value());
  append_identity_field(out, 2, "lineage", request.lineage.raw(), "LineageId");
  append_generation_field(out, 2, "selection_generation", request.selection_generation.value());
  append_identity_field(out, 2, "policy", request.policy.raw(), "PolicyId");
  append_generation_field(out, 2, "policy_generation", request.policy_generation.value());
  append_generation_field(out, 2, "evidence_generation", request.evidence_generation.value());
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');

  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "eligibility");
  out.append(": {");
  append_enum_field(out, 2, "state", promotion_eligibility_state_name(request.eligibility),
                    static_cast<std::uint32_t>(request.eligibility), true);
  append_unsigned_field(out, 2, "outstanding_requirement_count",
                        request.outstanding_requirements.size(), false);
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');

  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "outstanding_requirements");
  out.append(": [");
  for (std::size_t index = 0; index < request.outstanding_requirements.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    out.push_back('\n');
    append_indent(out, 2);
    append_json_string(out, request.outstanding_requirements[index]);
  }
  if (!request.outstanding_requirements.empty()) {
    out.push_back('\n');
    append_indent(out, 1);
  }
  out.push_back(']');

  // Artifacts and evidence are sorted before rendering so that two requests
  // which differ only in the order the caller supplied those elements produce
  // byte-identical files.
  std::vector<ArtifactRef> artifacts = request.artifacts;
  std::sort(artifacts.begin(), artifacts.end(), ArtifactLess{});
  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "artifacts");
  out.append(": [");
  for (std::size_t index = 0; index < artifacts.size(); ++index) {
    const ArtifactRef& artifact = artifacts[index];
    if (index != 0) {
      out.push_back(',');
    }
    out.push_back('\n');
    append_indent(out, 2);
    out.push_back('{');
    append_field(out, 3, "name", artifact.name, true);
    append_unsigned_field(out, 3, "size_bytes", artifact.size_bytes, false);
    append_field(out, 3, "content_digest", artifact.content_digest, false);
    out.push_back('\n');
    append_indent(out, 2);
    out.push_back('}');
  }
  if (!artifacts.empty()) {
    out.push_back('\n');
    append_indent(out, 1);
  }
  out.push_back(']');

  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "artifact_set");
  out.append(": {");
  append_unsigned_field(out, 2, "count", artifacts.size(), true);
  append_unsigned_field(out, 2, "total_artifact_bytes", request.total_artifact_bytes, false);
  append_field(out, 2, "artifact_set_digest", request.artifact_set_digest, false);
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');

  std::vector<PromotionEvidenceSummary> evidence = request.mandatory_evidence;
  std::sort(evidence.begin(), evidence.end(), EvidenceLess{});
  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "mandatory_evidence");
  out.append(": [");
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    const PromotionEvidenceSummary& summary = evidence[index];
    if (index != 0) {
      out.push_back(',');
    }
    out.push_back('\n');
    append_indent(out, 2);
    out.push_back('{');
    append_field(out, 3, "evaluator_key", summary.evaluator_key, true);
    append_enum_field(out, 3, "evaluator_kind", evaluator_kind_name(summary.kind),
                      static_cast<std::uint32_t>(summary.kind), false);
    append_enum_field(out, 3, "outcome", evaluation_outcome_name(summary.outcome),
                      static_cast<std::uint32_t>(summary.outcome), false);
    append_enum_field(out, 3, "requirement_class",
                      requirement_class_text(summary.requirement_class),
                      static_cast<std::uint32_t>(summary.requirement_class), false);
    append_boolean_field(out, 3, "complete", summary.complete, false);
    append_boolean_field(out, 3, "has_score", summary.has_score, false);
    if (summary.has_score) {
      append_field(out, 3, "score_binary64_hex", binary64_hex(summary.score), false);
    }
    append_unsigned_field(out, 3, "generation", summary.generation.value(), false);
    append_field(out, 3, "evidence_digest", summary.evidence_digest, false);
    out.push_back('\n');
    append_indent(out, 2);
    out.push_back('}');
  }
  if (!evidence.empty()) {
    out.push_back('\n');
    append_indent(out, 1);
  }
  out.push_back(']');

  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "provenance");
  out.append(": {");
  append_unsigned_field(out, 2, "ancestry_count", request.ancestry.size(), true);
  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 2);
  append_json_string(out, "ancestry_root_first");
  out.append(": [");
  for (std::size_t index = 0; index < request.ancestry.size(); ++index) {
    if (index != 0) {
      out.append(", ");
    }
    out.append(std::to_string(request.ancestry[index].raw()));
  }
  out.push_back(']');
  append_unsigned_field(out, 2, "lineage_depth", request.lineage_depth, false);
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');

  append_field(out, 1, "canonical_state_digest", request.canonical_state_digest, false);

  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "canonical_encoding");
  out.append(": {");
  append_field(out, 2, "form", "encode_promotion_request_canonical", true);
  append_unsigned_field(out, 2, "canonical_encoding_bytes", canonical_encoding_bytes, false);
  append_field(out, 2, "canonical_encoding_sha256", canonical_encoding_digest, false);
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');

  out.push_back('\n');
  out.push_back('}');
  out.push_back('\n');
  return out;
}

/// The receipt rendering. This is what the sink decided, in the sink's own
/// voice, so the bytes on disk can never be mistaken for a promotion.
[[nodiscard]] std::string render_receipt(const PromotionReceipt& receipt,
                                         const PromotionRequest& request) {
  std::string out;
  out.append("{\n");
  append_field(out, 1, "document", "autonomous-foundry.promotion-receipt", true);
  append_unsigned_field(out, 1, "document_version", 1, false);
  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "request");
  out.append(": {");
  append_identity_field(out, 2, "id", request.id.raw(), "PromotionRequestId");
  append_identity_field(out, 2, "candidate", request.candidate.raw(), "CandidateId");
  append_generation_field(out, 2, "candidate_generation", request.candidate_generation.value());
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');
  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "receipt");
  out.append(": {");
  append_enum_field(out, 2, "handoff", promotion_handoff_state_name(receipt.handoff),
                    static_cast<std::uint32_t>(receipt.handoff), true);
  append_field(out, 2, "sink_name", receipt.sink_name, false);
  append_field(out, 2, "sink_reference", receipt.sink_reference, false);
  append_field(out, 2, "detail", receipt.detail, false);
  append_field(out, 2, "request_digest", receipt.request_digest, false);
  out.push_back('\n');
  append_indent(out, 1);
  out.push_back('}');
  out.push_back(',');
  out.push_back('\n');
  append_indent(out, 1);
  append_json_string(out, "meaning");
  out.append(": ");
  append_json_string(out,
                     "this sink accepted the request for its own processing; the artifact has "
                     "not been promoted and this runtime makes no promotion claim");
  out.push_back('\n');
  out.push_back('}');
  out.push_back('\n');
  return out;
}

}  // namespace

LocalPromotionSink::LocalPromotionSink(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::string LocalPromotionSink::name() const { return std::string(kLocalPromotionSinkName); }

Result<PromotionReceipt> LocalPromotionSink::submit(const PromotionRequest& request) {
  // Validate before touching the filesystem. A rejected request leaves no
  // partial state behind, so the directory always answers "was this handed
  // over?" truthfully.
  if (!request.id.valid()) {
    return Status(ErrorCode::NullIdentity,
                  "promotion request carries no request identity; a handoff that cannot be "
                  "named cannot be recorded");
  }
  if (request.artifacts.empty()) {
    return Status(ErrorCode::InvalidArgument,
                  "promotion request carries no artifacts; there is nothing to hand over");
  }
  if (request.artifacts.size() > kMaxArtifactsPerCandidate) {
    return Status(ErrorCode::LengthOutOfRange,
                  "promotion request declares " + std::to_string(request.artifacts.size()) +
                      " artifacts, which exceeds the bound of " +
                      std::to_string(kMaxArtifactsPerCandidate));
  }
  for (const ArtifactRef& artifact : request.artifacts) {
    AF_TRY(validate_artifact_ref(artifact));
  }

  {
    const std::lock_guard<std::mutex> guard(prepare_mutex_);
    if (!prepared_) {
      std::error_code error;
      std::filesystem::create_directories(directory_, error);
      if (error) {
        return Status(ErrorCode::IoFailure, "promotion sink directory '" +
                                                path_to_utf8(directory_) +
                                                "' could not be created: " + error.message());
      }
      AF_TRY(ensure_directory_plain(directory_));
      prepared_ = true;
    }
  }

  const std::string canonical_bytes = encode_promotion_request_canonical(request);
  const std::string canonical_digest = sha256_hex(canonical_bytes);

  const std::string stem = identity_stem(request.id);
  const std::filesystem::path request_path = directory_ / (stem + ".json");
  const std::filesystem::path receipt_path = directory_ / (stem + ".receipt.json");

  AF_TRY(atomic_write_file(request_path, render_request(request, canonical_digest,
                                                        canonical_bytes.size())));

  PromotionReceipt receipt;
  // The only handoff state this sink may ever report. AcceptedForProcessing
  // means the receiver owns its own decision from here; it is not a promotion.
  receipt.handoff = PromotionHandoffState::AcceptedForProcessing;
  receipt.sink_name = name();
  receipt.sink_reference = std::string(kLocalPromotionSinkName) + ":" + stem;
  receipt.detail = "request recorded as '" + path_to_utf8(request_path) +
                   "'; the artifact is not promoted by this runtime and the receiving system "
                   "has not yet decided anything";
  receipt.request_digest = canonical_digest;

  AF_TRY(atomic_write_file(receipt_path, render_receipt(receipt, request)));

  return receipt;
}

}  // namespace autonomous_foundry
