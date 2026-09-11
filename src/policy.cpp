// src/policy.cpp
//
// Policy: enumerator names, the canonical policy encoding, the policy digest,
// structural validation and the reference policy.
//
// Canonical encoding
// ------------------
//
// Every canonical encoding produced by this file is an explicit, field-tagged,
// length-prefixed byte string of the form
//
//     <field-name> '=' <decimal value length> ':' <raw value bytes> ';'
//
// Field names are fixed ASCII tags that never contain '=', ':' or ';'. The
// length counts raw value bytes, so a value may contain any byte without making
// the encoding ambiguous. Nothing is escaped and nothing is normalised: the
// encoding is a pure function of the logical value, and no two different
// logical values can produce the same bytes.
//
// Doubles are emitted as 16 lowercase hexadecimal digits of their IEEE-754
// binary64 representation, most significant byte first. The raw bit pattern is
// exact (no decimal rounding, no locale) and negative zero is folded onto
// positive zero because the two compare equal.
//
// Enumerator fields are emitted as their decimal ordinal, so even a value that
// arrived out of range from a snapshot encodes distinctly.
//
// A vector is emitted as a decimal count field followed by exactly that many
// element groups; a group is the fixed sequence of fields documented below.
//
// Field sequence emitted by encode_policy_canonical. FoundryPolicy::
// content_digest is deliberately absent: the digest is the output of hashing
// this encoding, so including it would be circular.
//
//   policy_id                 decimal raw identity
//   policy_generation         decimal generation value
//   name                      raw bytes
//   factor_count              decimal
//   factor_kind               decimal ordinal } repeated factor_count times in
//   factor_direction          decimal ordinal } the declared order, which is
//   factor_weight             binary64 hex      the order in which ranking
//   factor_key                raw bytes         consults the factors
//   selection_require_complete_mandatory   decimal 0|1
//   retention_retain_top_k                 decimal
//   retention_max_retained                 decimal
//   retention_max_per_lineage              decimal
//   retention_retain_selected              decimal 0|1
//   budget_max_candidate_attempts          decimal } the nine BudgetLimits
//   ...                                            } fields, in declaration
//   budget_max_evaluation_concurrency      decimal } order
//   max_generation_depth                   decimal
//   carry_forward_elite                    decimal 0|1
//
// Factor order is part of the contract: it is the documented evaluation order
// of the ranking, and the final factor is required to be the identity
// tie-break, so two policies that list the same factors in a different order
// are different policies and must not share a digest.

#include "autonomous_foundry/policy.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/limits.hpp"

namespace autonomous_foundry {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::size_t kBinary64HexDigits = 16;
constexpr std::uint64_t kNegativeZeroBits = 0x8000000000000000ull;

static_assert(sizeof(double) == sizeof(std::uint64_t),
              "canonical encoding requires IEEE-754 binary64 doubles");

void append_field(std::string& out, std::string_view name, std::string_view value) {
  out.append(name);
  out.push_back('=');
  out.append(std::to_string(value.size()));
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

void append_count(std::string& out, std::string_view name, std::size_t value) {
  append_field(out, name, std::to_string(value));
}

void append_u32(std::string& out, std::string_view name, std::uint32_t value) {
  append_field(out, name, std::to_string(value));
}

void append_u64(std::string& out, std::string_view name, std::uint64_t value) {
  append_field(out, name, std::to_string(value));
}

void append_bool(std::string& out, std::string_view name, bool value) {
  append_field(out, name, value ? "1" : "0");
}

void append_binary64(std::string& out, std::string_view name, double value) {
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
  append_field(out, name, digits);
}

}  // namespace

std::string_view ranking_factor_name(RankingFactorKind kind) noexcept {
  switch (kind) {
    case RankingFactorKind::QualityScore:
      return "QualityScore";
    case RankingFactorKind::EfficiencyScore:
      return "EfficiencyScore";
    case RankingFactorKind::EvaluationCompleteness:
      return "EvaluationCompleteness";
    case RankingFactorKind::LineageDepth:
      return "LineageDepth";
    case RankingFactorKind::AttemptCount:
      return "AttemptCount";
    case RankingFactorKind::ArtifactBytes:
      return "ArtifactBytes";
    case RankingFactorKind::CandidateIdTieBreak:
      return "CandidateIdTieBreak";
  }
  return "Unknown";
}

std::string encode_policy_canonical(const FoundryPolicy& policy) {
  std::string out;
  append_u64(out, "policy_id", policy.id.raw());
  append_u32(out, "policy_generation", policy.generation.value());
  append_field(out, "name", policy.name);

  append_count(out, "factor_count", policy.selection.factors.size());
  for (const RankingFactor& factor : policy.selection.factors) {
    append_u32(out, "factor_kind", static_cast<std::uint32_t>(factor.kind));
    append_u32(out, "factor_direction", static_cast<std::uint32_t>(factor.direction));
    append_binary64(out, "factor_weight", factor.weight);
    append_field(out, "factor_key", factor.key);
  }

  append_bool(out, "selection_require_complete_mandatory",
              policy.selection.require_complete_mandatory);
  append_u32(out, "retention_retain_top_k", policy.retention.retain_top_k);
  append_u32(out, "retention_max_retained", policy.retention.max_retained);
  append_u32(out, "retention_max_per_lineage", policy.retention.max_per_lineage);
  append_bool(out, "retention_retain_selected", policy.retention.retain_selected);

  const BudgetLimits& budgets = policy.budgets;
  append_u32(out, "budget_max_candidate_attempts", budgets.max_candidate_attempts);
  append_u32(out, "budget_max_worker_assignments", budgets.max_worker_assignments);
  append_u32(out, "budget_max_evaluation_attempts", budgets.max_evaluation_attempts);
  append_u32(out, "budget_max_retained_candidates", budgets.max_retained_candidates);
  append_u32(out, "budget_max_retries", budgets.max_retries);
  append_u32(out, "budget_max_candidates", budgets.max_candidates);
  append_u32(out, "budget_max_workers", budgets.max_workers);
  append_u32(out, "budget_max_active_attempts", budgets.max_active_attempts);
  append_u32(out, "budget_max_evaluation_concurrency", budgets.max_evaluation_concurrency);

  append_u32(out, "max_generation_depth", policy.max_generation_depth);
  append_bool(out, "carry_forward_elite", policy.carry_forward_elite);
  return out;
}

Result<std::string> compute_policy_digest(const FoundryPolicy& policy) {
  const std::string encoding = encode_policy_canonical(policy);
  return sha256_hex(encoding);
}

Status validate_policy(const FoundryPolicy& policy) {
  if (!policy.id.valid()) {
    return Status(ErrorCode::NullIdentity, std::string_view("policy id is the null identity"));
  }
  if (!policy.generation.valid()) {
    return Status(ErrorCode::InvalidArgument,
                  std::string_view("policy generation is generation 0, which is not a valid "
                                   "generation"));
  }
  if (policy.name.empty() || policy.name.size() > kMaxNameLength) {
    return Status(ErrorCode::LengthOutOfRange,
                  "policy name must be 1 to " + std::to_string(kMaxNameLength) +
                      " bytes long but is " + std::to_string(policy.name.size()) + " bytes");
  }

  const std::vector<RankingFactor>& factors = policy.selection.factors;
  if (factors.empty()) {
    return Status(ErrorCode::InvalidArgument,
                  "policy '" + policy.name +
                      "' declares no ranking factors, so ranking could not order candidates at "
                      "all");
  }
  if (factors.size() > kMaxRankingFactors) {
    return Status(ErrorCode::LengthOutOfRange,
                  "policy '" + policy.name + "' declares " + std::to_string(factors.size()) +
                      " ranking factors, which exceeds the maximum of " +
                      std::to_string(kMaxRankingFactors));
  }
  for (std::size_t index = 0; index < factors.size(); ++index) {
    const RankingFactor& factor = factors[index];
    const std::string context =
        "policy ranking factor[" + std::to_string(index) + "] '" + factor.key + "'";
    if (factor.key.empty()) {
      return Status(ErrorCode::InvalidArgument,
                    "policy ranking factor[" + std::to_string(index) +
                        "] has an empty key; every factor must be named so that it can appear "
                        "in a ranking explanation");
    }
    if (!std::isfinite(factor.weight)) {
      return Status(ErrorCode::InvalidArgument, context + " has a non-finite weight");
    }
    if (factor.weight < 0.0) {
      return Status(ErrorCode::InvalidArgument, context + " has a negative weight");
    }
    for (std::size_t other = 0; other < index; ++other) {
      if (factors[other].key == factor.key) {
        return Status(ErrorCode::DuplicateIdentity,
                      context + " repeats the factor key '" + factor.key +
                          "' already used by policy ranking factor[" + std::to_string(other) +
                          "]");
      }
    }
  }

  const RankingFactor& tie_break = factors.back();
  if (tie_break.kind != RankingFactorKind::CandidateIdTieBreak) {
    return Status(ErrorCode::PolicyConflict,
                  "policy '" + policy.name + "' ends with the ranking factor '" + tie_break.key +
                      "' of kind " + std::string(ranking_factor_name(tie_break.kind)) +
                      "; ranking must be a total order, so the last factor must be "
                      "CandidateIdTieBreak, the only quantity guaranteed to differ between two "
                      "candidates");
  }
  if (tie_break.direction != RankingDirection::LowerIsBetter) {
    return Status(ErrorCode::PolicyConflict,
                  "policy '" + policy.name + "' declares the CandidateIdTieBreak factor '" +
                      tie_break.key +
                      "' with direction HigherIsBetter; ranking must be a total order, and the "
                      "identity tie-break is defined as LowerIsBetter");
  }
  bool has_positive_weight = false;
  for (std::size_t index = 0; index + 1 < factors.size(); ++index) {
    if (factors[index].weight > 0.0) {
      has_positive_weight = true;
      break;
    }
  }
  if (!has_positive_weight) {
    return Status(ErrorCode::PolicyConflict,
                  "policy '" + policy.name +
                      "' gives every ranking factor before the candidate identity tie-break a "
                      "zero weight, so ranking could not prefer a better candidate; ranking must "
                      "be a total order over at least one weighted factor and the identity "
                      "tie-break");
  }

  const RetentionPolicy& retention = policy.retention;
  if (retention.retain_top_k < 1) {
    return Status(ErrorCode::InvalidArgument,
                  "policy '" + policy.name +
                      "' retains a top-k of 0; at least the winner must be retained");
  }
  if (retention.retain_top_k > retention.max_retained) {
    return Status(ErrorCode::RetentionLimitExceeded,
                  "policy '" + policy.name + "' retains a top-k of " +
                      std::to_string(retention.retain_top_k) +
                      " but allows at most " + std::to_string(retention.max_retained) +
                      " retained candidates");
  }
  if (retention.max_retained > policy.budgets.max_retained_candidates) {
    return Status(ErrorCode::RetentionLimitExceeded,
                  "policy '" + policy.name + "' allows " +
                      std::to_string(retention.max_retained) +
                      " retained candidates, which exceeds the policy budget of " +
                      std::to_string(policy.budgets.max_retained_candidates));
  }
  if (retention.max_per_lineage < 1) {
    return Status(ErrorCode::InvalidArgument,
                  "policy '" + policy.name +
                      "' allows 0 retained candidates per lineage, so no candidate could ever be "
                      "retained");
  }
  if (retention.max_per_lineage > retention.max_retained) {
    return Status(ErrorCode::RetentionLimitExceeded,
                  "policy '" + policy.name + "' allows " +
                      std::to_string(retention.max_per_lineage) +
                      " retained candidates per lineage, which exceeds the overall retention "
                      "limit of " + std::to_string(retention.max_retained));
  }

  if (policy.max_generation_depth < 1) {
    return Status(ErrorCode::InvalidArgument,
                  "policy '" + policy.name +
                      "' declares a maximum generation depth of 0, so no lineage could ever "
                      "advance");
  }
  if (policy.max_generation_depth > kMaxLineageDepth) {
    return Status(ErrorCode::LengthOutOfRange,
                  "policy '" + policy.name + "' declares a maximum generation depth of " +
                      std::to_string(policy.max_generation_depth) +
                      ", which exceeds the maximum of " + std::to_string(kMaxLineageDepth));
  }

  AF_TRY(policy.budgets.validate());
  return Status();
}

FoundryPolicy make_reference_policy(PolicyId id, PolicyGeneration generation) {
  FoundryPolicy policy;
  policy.id = id;
  policy.generation = generation;
  policy.name = "reference-v1";
  policy.selection.factors = {
      RankingFactor{RankingFactorKind::QualityScore, RankingDirection::HigherIsBetter, 1.0,
                    "quality"},
      RankingFactor{RankingFactorKind::EfficiencyScore, RankingDirection::HigherIsBetter, 0.5,
                    "efficiency"},
      RankingFactor{RankingFactorKind::EvaluationCompleteness, RankingDirection::HigherIsBetter,
                    0.25, "completeness"},
      RankingFactor{RankingFactorKind::LineageDepth, RankingDirection::LowerIsBetter, 0.1,
                    "depth"},
      RankingFactor{RankingFactorKind::CandidateIdTieBreak, RankingDirection::LowerIsBetter, 0.0,
                    "candidate"},
  };
  policy.selection.require_complete_mandatory = true;
  policy.retention.retain_top_k = 3;
  policy.retention.max_retained = 8;
  policy.retention.max_per_lineage = 2;
  policy.retention.retain_selected = true;
  policy.budgets = BudgetLimits{};
  policy.max_generation_depth = 16;
  policy.carry_forward_elite = false;
  policy.content_digest = sha256_hex(encode_policy_canonical(policy));
  return policy;
}

}  // namespace autonomous_foundry
