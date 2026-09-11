#include "autonomous_foundry/protocol.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/lineage.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/version.hpp"

// Implementation of the framed wire protocol.
//
// This translation unit is the trusted-parsing boundary of the runtime: every
// byte that arrives from a worker, a controller or an observer is decoded here,
// and nothing in a payload is believed before it has been validated. Four rules
// govern the whole file.
//
//   * Every element count read from the wire is checked against a protocol
//     ceiling and against the bytes that actually remain before a single byte
//     is reserved, so a hostile count can never drive an allocation.
//   * Every string length is checked against the remaining bytes before the
//     string is materialised.
//   * Every enumeration value is range checked; an out-of-range value is a
//     MalformedEncoding rejection rather than an invalid value carried into
//     runtime state.
//   * Every decoder consumes its payload exactly. Trailing bytes are a
//     rejection, because a sender that appends bytes is either confused or is
//     probing for a parser that silently ignores them.
//
// The field order of every message mirrors the declaration order of the frozen
// struct in protocol.hpp, and the order is restated in a comment above each
// encoder so that the wire format can be audited without reading the decoder.

namespace autonomous_foundry {

namespace {

// ---------------------------------------------------------------------------
// Wire bounds
// ---------------------------------------------------------------------------

/// Ceiling for vectors that carry no tighter domain limit. 4096 is sixteen
/// times kMaxArtifactNameLength: far above every payload the runtime itself
/// produces, far below any count that could be turned into a large allocation
/// by a hostile frame.
constexpr std::uint32_t kMaxProtocolElements =
    static_cast<std::uint32_t>(kMaxArtifactNameLength) * 16u;

/// Artifact lists are bounded by the domain limit for one candidate.
constexpr std::uint32_t kMaxProtocolArtifacts =
    static_cast<std::uint32_t>(kMaxArtifactsPerCandidate);

/// Ranking factor lists are bounded by the policy limit.
constexpr std::uint32_t kMaxProtocolRankingFactors =
    static_cast<std::uint32_t>(kMaxRankingFactors);

// Minimum encoded size of one element of a vector field. These are
// deliberately conservative lower bounds: their only job is to reject an
// impossible count before anything is reserved, so an underestimate is safe
// while an overestimate could reject a legal payload.
constexpr std::uint64_t kMinIdentityBytes = 8;
constexpr std::uint64_t kMinStringBytes = 4;
constexpr std::uint64_t kMinArtifactRefBytes = 16;
constexpr std::uint64_t kMinRankingFactorBytes = 12;
constexpr std::uint64_t kMinRankingFactorValueBytes = 31;
constexpr std::uint64_t kMinRankingEntryBytes = 40;
constexpr std::uint64_t kMinExclusionEntryBytes = 14;
constexpr std::uint64_t kMinRetentionEntryBytes = 21;
constexpr std::uint64_t kMinEvaluationRecordBytes = 97;
constexpr std::uint64_t kMinLineageNodeBytes = 45;
constexpr std::uint64_t kMinPromotionEvidenceBytes = 25;
constexpr std::uint64_t kMinInputFileBytes = 12;
constexpr std::uint64_t kMinRequirementBytes = 26;

// Enumeration cardinalities that the frozen headers do not export. Each one is
// the number of declared enumerators of the corresponding enumeration.
constexpr std::size_t kSessionRoleCount = 3;
constexpr std::size_t kRequirementClassCount = 2;
constexpr std::size_t kRankingDirectionCount = 2;
constexpr std::size_t kSelectionDecisionStateCount = 4;
constexpr std::size_t kRetentionOutcomeCount = 7;
constexpr std::size_t kPromotionEligibilityCount = 4;
constexpr std::size_t kPromotionHandoffCount = 5;

// ---------------------------------------------------------------------------
// Little-endian primitives
// ---------------------------------------------------------------------------

/// Append one little-endian unsigned value of the given width.
template <typename UInt>
void append_little_endian(std::string& buffer, UInt value) {
  const std::uint64_t wide = static_cast<std::uint64_t>(value);
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    const std::uint64_t shift = static_cast<std::uint64_t>(index) * 8u;
    buffer.push_back(static_cast<char>(static_cast<unsigned char>((wide >> shift) & 0xFFu)));
  }
}

/// Read a little-endian unsigned value from at most eight bytes. Every caller
/// has already taken exactly the field width, so the loop bound is the field
/// width itself.
[[nodiscard]] std::uint64_t load_little_endian(std::string_view bytes) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const std::uint64_t part =
        static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index]));
    const std::uint64_t shift = static_cast<std::uint64_t>(index) * 8u;
    value |= part << shift;
  }
  return value;
}

/// Lowercase hexadecimal rendering of a 32-bit wire value, for diagnostics.
[[nodiscard]] std::string to_hex32(std::uint32_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string text("0x");
  for (int shift = 28; shift >= 0; shift -= 4) {
    const std::uint32_t nibble = (value >> shift) & 0xFu;
    text.push_back(kDigits[nibble]);
  }
  return text;
}

// ---------------------------------------------------------------------------
// Bounds-checked readers
// ---------------------------------------------------------------------------

/// Read a vector element count and prove, before the caller reserves anything,
/// that the payload can actually hold that many elements. An impossible count
/// is LengthOutOfRange rather than an allocation.
[[nodiscard]] Result<std::uint32_t> read_count(ByteReader& reader, std::string_view field,
                                               std::uint32_t ceiling,
                                               std::uint64_t min_element_bytes) {
  std::uint32_t count = 0;
  AF_TRY_ASSIGN(count, reader.u32());
  if (count > ceiling) {
    return Status(ErrorCode::LengthOutOfRange,
                  "field '" + std::string(field) + "' declares " + std::to_string(count) +
                      " elements, above the accepted ceiling of " + std::to_string(ceiling));
  }
  if (count == 0) {
    return count;
  }
  const std::uint64_t required = static_cast<std::uint64_t>(count) * min_element_bytes;
  const std::uint64_t available = static_cast<std::uint64_t>(reader.remaining());
  if (required > available) {
    return Status(ErrorCode::LengthOutOfRange,
                  "field '" + std::string(field) + "' declares " + std::to_string(count) +
                      " elements needing at least " + std::to_string(required) +
                      " bytes but only " + std::to_string(available) + " remain");
  }
  return count;
}

// ---------------------------------------------------------------------------
// Shared field codecs, in dependency order
// ---------------------------------------------------------------------------

// str name, u64 size_bytes, str content_digest
void write_artifact_ref(ByteWriter& writer, const ArtifactRef& value) {
  writer.str(value.name);
  writer.u64(value.size_bytes);
  writer.str(value.content_digest);
}

Result<ArtifactRef> read_artifact_ref(ByteReader& reader) {
  ArtifactRef value;
  AF_TRY_ASSIGN(value.name, reader.str());
  AF_TRY_ASSIGN(value.size_bytes, reader.u64());
  AF_TRY_ASSIGN(value.content_digest, reader.str());
  return value;
}

void write_artifact_list(ByteWriter& writer, const std::vector<ArtifactRef>& values) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (const ArtifactRef& value : values) {
    write_artifact_ref(writer, value);
  }
}

Result<std::vector<ArtifactRef>> read_artifact_list(ByteReader& reader, std::string_view field) {
  std::uint32_t count = 0;
  AF_TRY_ASSIGN(count, read_count(reader, field, kMaxProtocolArtifacts, kMinArtifactRefBytes));
  std::vector<ArtifactRef> values;
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    ArtifactRef value;
    AF_TRY_ASSIGN(value, read_artifact_ref(reader));
    values.push_back(std::move(value));
  }
  return values;
}

void write_string_list(ByteWriter& writer, const std::vector<std::string>& values) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (const std::string& value : values) {
    writer.str(value);
  }
}

Result<std::vector<std::string>> read_string_list(ByteReader& reader, std::string_view field) {
  std::uint32_t count = 0;
  AF_TRY_ASSIGN(count, read_count(reader, field, kMaxProtocolElements, kMinStringBytes));
  std::vector<std::string> values;
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string value;
    AF_TRY_ASSIGN(value, reader.str());
    values.push_back(std::move(value));
  }
  return values;
}

template <typename Tag>
void write_identity_list(ByteWriter& writer, const std::vector<StrongId<Tag>>& values) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (const StrongId<Tag>& value : values) {
    writer.identity(value);
  }
}

template <typename Tag>
Result<std::vector<StrongId<Tag>>> read_identity_list(ByteReader& reader, std::string_view field) {
  std::uint32_t count = 0;
  AF_TRY_ASSIGN(count, read_count(reader, field, kMaxProtocolElements, kMinIdentityBytes));
  std::vector<StrongId<Tag>> values;
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    StrongId<Tag> value;
    AF_TRY_ASSIGN(value, reader.identity<Tag>(field));
    values.push_back(value);
  }
  return values;
}

// nine u32 budget fields, in declaration order
void write_budget_limits(ByteWriter& writer, const BudgetLimits& value) {
  writer.u32(value.max_candidate_attempts);
  writer.u32(value.max_worker_assignments);
  writer.u32(value.max_evaluation_attempts);
  writer.u32(value.max_retained_candidates);
  writer.u32(value.max_retries);
  writer.u32(value.max_candidates);
  writer.u32(value.max_workers);
  writer.u32(value.max_active_attempts);
  writer.u32(value.max_evaluation_concurrency);
}

Result<BudgetLimits> read_budget_limits(ByteReader& reader) {
  BudgetLimits value;
  AF_TRY_ASSIGN(value.max_candidate_attempts, reader.u32());
  AF_TRY_ASSIGN(value.max_worker_assignments, reader.u32());
  AF_TRY_ASSIGN(value.max_evaluation_attempts, reader.u32());
  AF_TRY_ASSIGN(value.max_retained_candidates, reader.u32());
  AF_TRY_ASSIGN(value.max_retries, reader.u32());
  AF_TRY_ASSIGN(value.max_candidates, reader.u32());
  AF_TRY_ASSIGN(value.max_workers, reader.u32());
  AF_TRY_ASSIGN(value.max_active_attempts, reader.u32());
  AF_TRY_ASSIGN(value.max_evaluation_concurrency, reader.u32());
  return value;
}

// str name, str content, str content_digest
void write_input_file(ByteWriter& writer, const InputFile& value) {
  writer.str(value.name);
  writer.str(value.content);
  writer.str(value.content_digest);
}

Result<InputFile> read_input_file(ByteReader& reader) {
  InputFile value;
  AF_TRY_ASSIGN(value.name, reader.str());
  AF_TRY_ASSIGN(value.content, reader.str());
  AF_TRY_ASSIGN(value.content_digest, reader.str());
  return value;
}

// u8 requirement_class, str evaluator_key, str description, bool
// has_score_floor, f64 score_floor, f64 ranking_weight
void write_evaluation_requirement(ByteWriter& writer, const EvaluationRequirement& value) {
  writer.u8(static_cast<std::uint8_t>(value.requirement_class));
  writer.str(value.evaluator_key);
  writer.str(value.description);
  writer.boolean(value.has_score_floor);
  writer.f64(value.score_floor);
  writer.f64(value.ranking_weight);
}

Result<EvaluationRequirement> read_evaluation_requirement(ByteReader& reader) {
  EvaluationRequirement value;
  std::uint8_t requirement_class = 0;
  AF_TRY_ASSIGN(requirement_class, reader.u8());
  if (static_cast<std::size_t>(requirement_class) >= kRequirementClassCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "evaluation requirement class " + std::to_string(requirement_class) +
                      " is not a declared requirement class");
  }
  value.requirement_class = static_cast<RequirementClass>(requirement_class);
  AF_TRY_ASSIGN(value.evaluator_key, reader.str());
  AF_TRY_ASSIGN(value.description, reader.str());
  AF_TRY_ASSIGN(value.has_score_floor, reader.boolean());
  AF_TRY_ASSIGN(value.score_floor, reader.f64());
  AF_TRY_ASSIGN(value.ranking_weight, reader.f64());
  return value;
}

// identity id, generation generation, str name, str objective,
// u32 input count + InputFile, u32 required_outputs count + str,
// u32 requirement count + EvaluationRequirement,
// u32 hard_constraints count + str, u32 closure_criteria count + str,
// identity policy, generation policy_generation, BudgetLimits,
// str content_digest
void write_task_spec(ByteWriter& writer, const TaskSpec& value) {
  writer.identity(value.id);
  writer.generation(value.generation);
  writer.str(value.name);
  writer.str(value.objective);
  writer.u32(static_cast<std::uint32_t>(value.inputs.size()));
  for (const InputFile& input : value.inputs) {
    write_input_file(writer, input);
  }
  write_string_list(writer, value.required_outputs);
  writer.u32(static_cast<std::uint32_t>(value.requirements.size()));
  for (const EvaluationRequirement& requirement : value.requirements) {
    write_evaluation_requirement(writer, requirement);
  }
  write_string_list(writer, value.hard_constraints);
  write_string_list(writer, value.closure_criteria);
  writer.identity(value.policy);
  writer.generation(value.policy_generation);
  write_budget_limits(writer, value.budgets);
  writer.str(value.content_digest);
}

Result<TaskSpec> read_task_spec(ByteReader& reader) {
  TaskSpec value;
  AF_TRY_ASSIGN(value.id, reader.identity<TaskIdTag>("task.id"));
  AF_TRY_ASSIGN(value.generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.name, reader.str());
  AF_TRY_ASSIGN(value.objective, reader.str());
  std::uint32_t input_count = 0;
  AF_TRY_ASSIGN(input_count,
                read_count(reader, "task.inputs", kMaxProtocolElements, kMinInputFileBytes));
  value.inputs.reserve(input_count);
  for (std::uint32_t input_index = 0; input_index < input_count; ++input_index) {
    InputFile input;
    AF_TRY_ASSIGN(input, read_input_file(reader));
    value.inputs.push_back(std::move(input));
  }
  AF_TRY_ASSIGN(value.required_outputs, read_string_list(reader, "task.required_outputs"));
  std::uint32_t requirement_count = 0;
  AF_TRY_ASSIGN(
      requirement_count,
      read_count(reader, "task.requirements", kMaxProtocolElements, kMinRequirementBytes));
  value.requirements.reserve(requirement_count);
  for (std::uint32_t requirement_index = 0; requirement_index < requirement_count;
       ++requirement_index) {
    EvaluationRequirement requirement;
    AF_TRY_ASSIGN(requirement, read_evaluation_requirement(reader));
    value.requirements.push_back(std::move(requirement));
  }
  AF_TRY_ASSIGN(value.hard_constraints, read_string_list(reader, "task.hard_constraints"));
  AF_TRY_ASSIGN(value.closure_criteria, read_string_list(reader, "task.closure_criteria"));
  AF_TRY_ASSIGN(value.policy, reader.identity<PolicyIdTag>("task.policy"));
  AF_TRY_ASSIGN(value.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(value.budgets, read_budget_limits(reader));
  AF_TRY_ASSIGN(value.content_digest, reader.str());
  return value;
}

// u8 kind, u8 direction, f64 weight, str key
void write_ranking_factor(ByteWriter& writer, const RankingFactor& value) {
  writer.u8(static_cast<std::uint8_t>(value.kind));
  writer.u8(static_cast<std::uint8_t>(value.direction));
  writer.f64(value.weight);
  writer.str(value.key);
}

Result<RankingFactor> read_ranking_factor(ByteReader& reader) {
  RankingFactor value;
  std::uint8_t kind = 0;
  AF_TRY_ASSIGN(kind, reader.u8());
  if (static_cast<std::size_t>(kind) >= kRankingFactorKindCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "ranking factor kind " + std::to_string(kind) +
                      " is not a declared ranking factor kind");
  }
  value.kind = static_cast<RankingFactorKind>(kind);
  std::uint8_t direction = 0;
  AF_TRY_ASSIGN(direction, reader.u8());
  if (static_cast<std::size_t>(direction) >= kRankingDirectionCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "ranking direction " + std::to_string(direction) +
                      " is not a declared ranking direction");
  }
  value.direction = static_cast<RankingDirection>(direction);
  AF_TRY_ASSIGN(value.weight, reader.f64());
  AF_TRY_ASSIGN(value.key, reader.str());
  return value;
}

// identity id, generation generation, str name,
// u32 factor count + RankingFactor, bool require_complete_mandatory,
// u32 retain_top_k, u32 max_retained, u32 max_per_lineage, bool retain_selected,
// BudgetLimits, u32 max_generation_depth, bool carry_forward_elite,
// str content_digest
void write_policy(ByteWriter& writer, const FoundryPolicy& value) {
  writer.identity(value.id);
  writer.generation(value.generation);
  writer.str(value.name);
  writer.u32(static_cast<std::uint32_t>(value.selection.factors.size()));
  for (const RankingFactor& factor : value.selection.factors) {
    write_ranking_factor(writer, factor);
  }
  writer.boolean(value.selection.require_complete_mandatory);
  writer.u32(value.retention.retain_top_k);
  writer.u32(value.retention.max_retained);
  writer.u32(value.retention.max_per_lineage);
  writer.boolean(value.retention.retain_selected);
  write_budget_limits(writer, value.budgets);
  writer.u32(value.max_generation_depth);
  writer.boolean(value.carry_forward_elite);
  writer.str(value.content_digest);
}

Result<FoundryPolicy> read_policy(ByteReader& reader) {
  FoundryPolicy value;
  AF_TRY_ASSIGN(value.id, reader.identity<PolicyIdTag>("policy.id"));
  AF_TRY_ASSIGN(value.generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(value.name, reader.str());
  std::uint32_t factor_count = 0;
  AF_TRY_ASSIGN(
      factor_count,
      read_count(reader, "policy.factors", kMaxProtocolRankingFactors, kMinRankingFactorBytes));
  value.selection.factors.reserve(factor_count);
  for (std::uint32_t factor_index = 0; factor_index < factor_count; ++factor_index) {
    RankingFactor factor;
    AF_TRY_ASSIGN(factor, read_ranking_factor(reader));
    value.selection.factors.push_back(std::move(factor));
  }
  AF_TRY_ASSIGN(value.selection.require_complete_mandatory, reader.boolean());
  AF_TRY_ASSIGN(value.retention.retain_top_k, reader.u32());
  AF_TRY_ASSIGN(value.retention.max_retained, reader.u32());
  AF_TRY_ASSIGN(value.retention.max_per_lineage, reader.u32());
  AF_TRY_ASSIGN(value.retention.retain_selected, reader.boolean());
  AF_TRY_ASSIGN(value.budgets, read_budget_limits(reader));
  AF_TRY_ASSIGN(value.max_generation_depth, reader.u32());
  AF_TRY_ASSIGN(value.carry_forward_elite, reader.boolean());
  AF_TRY_ASSIGN(value.content_digest, reader.str());
  return value;
}

// str name, identity task, generation task_generation, identity policy,
// generation policy_generation, u32 candidate_budget, u32 worker_budget,
// u32 population_index, identity predecessor, identity seed_candidate,
// generation seed_candidate_generation, identity seed_lineage,
// bool require_promotion_request
void write_population_spec(ByteWriter& writer, const PopulationSpec& value) {
  writer.str(value.name);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.policy);
  writer.generation(value.policy_generation);
  writer.u32(value.candidate_budget);
  writer.u32(value.worker_budget);
  writer.u32(value.population_index);
  writer.identity(value.predecessor);
  writer.identity(value.seed_candidate);
  writer.generation(value.seed_candidate_generation);
  writer.identity(value.seed_lineage);
  writer.boolean(value.require_promotion_request);
}

Result<PopulationSpec> read_population_spec(ByteReader& reader) {
  PopulationSpec value;
  AF_TRY_ASSIGN(value.name, reader.str());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("spec.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.policy, reader.identity<PolicyIdTag>("spec.policy"));
  AF_TRY_ASSIGN(value.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(value.candidate_budget, reader.u32());
  AF_TRY_ASSIGN(value.worker_budget, reader.u32());
  AF_TRY_ASSIGN(value.population_index, reader.u32());
  AF_TRY_ASSIGN(value.predecessor,
                reader.optional_identity<PopulationIdTag>("spec.predecessor"));
  AF_TRY_ASSIGN(value.seed_candidate,
                reader.optional_identity<CandidateIdTag>("spec.seed_candidate"));
  AF_TRY_ASSIGN(value.seed_candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.seed_lineage,
                reader.optional_identity<LineageIdTag>("spec.seed_lineage"));
  AF_TRY_ASSIGN(value.require_promotion_request, reader.boolean());
  return value;
}

// identity id, generation generation, identity run, str name, identity task,
// generation task_generation, identity policy, generation policy_generation,
// u32 candidate_budget, u32 worker_budget, u32 population_index,
// identity predecessor, identity seed_candidate,
// generation seed_candidate_generation, identity seed_lineage, u8 state,
// epoch created_epoch, epoch state_epoch, generation committed_selection,
// bool retention_committed, generation retention_selection_generation,
// bool require_promotion_request, u32 candidate count + identity,
// u64 promotion_requests, identity committed_promotion_request,
// str status_detail, u32 closure_blockers count + str
void write_population_record(ByteWriter& writer, const PopulationRecord& value) {
  writer.identity(value.id);
  writer.generation(value.generation);
  writer.identity(value.run);
  writer.str(value.name);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.policy);
  writer.generation(value.policy_generation);
  writer.u32(value.candidate_budget);
  writer.u32(value.worker_budget);
  writer.u32(value.population_index);
  writer.identity(value.predecessor);
  writer.identity(value.seed_candidate);
  writer.generation(value.seed_candidate_generation);
  writer.identity(value.seed_lineage);
  writer.u8(static_cast<std::uint8_t>(value.state));
  writer.epoch(value.created_epoch);
  writer.epoch(value.state_epoch);
  writer.generation(value.committed_selection);
  writer.boolean(value.retention_committed);
  writer.generation(value.retention_selection_generation);
  writer.boolean(value.require_promotion_request);
  write_identity_list(writer, value.candidates);
  writer.u64(value.promotion_requests);
  writer.identity(value.committed_promotion_request);
  writer.str(value.status_detail);
  write_string_list(writer, value.closure_blockers);
}

Result<PopulationRecord> read_population_record(ByteReader& reader) {
  PopulationRecord value;
  AF_TRY_ASSIGN(value.id, reader.identity<PopulationIdTag>("population.id"));
  AF_TRY_ASSIGN(value.generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.run, reader.identity<FoundryRunIdTag>("population.run"));
  AF_TRY_ASSIGN(value.name, reader.str());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("population.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.policy, reader.identity<PolicyIdTag>("population.policy"));
  AF_TRY_ASSIGN(value.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(value.candidate_budget, reader.u32());
  AF_TRY_ASSIGN(value.worker_budget, reader.u32());
  AF_TRY_ASSIGN(value.population_index, reader.u32());
  AF_TRY_ASSIGN(value.predecessor,
                reader.optional_identity<PopulationIdTag>("population.predecessor"));
  AF_TRY_ASSIGN(value.seed_candidate,
                reader.optional_identity<CandidateIdTag>("population.seed_candidate"));
  AF_TRY_ASSIGN(value.seed_candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.seed_lineage,
                reader.optional_identity<LineageIdTag>("population.seed_lineage"));
  std::uint8_t state = 0;
  AF_TRY_ASSIGN(state, reader.u8());
  if (static_cast<std::size_t>(state) >= kPopulationStateCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "population state " + std::to_string(state) + " is not a declared state");
  }
  value.state = static_cast<PopulationState>(state);
  AF_TRY_ASSIGN(value.created_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.state_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.committed_selection, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.retention_committed, reader.boolean());
  AF_TRY_ASSIGN(value.retention_selection_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.require_promotion_request, reader.boolean());
  AF_TRY_ASSIGN(value.candidates,
                read_identity_list<CandidateIdTag>(reader, "population.candidates"));
  AF_TRY_ASSIGN(value.promotion_requests, reader.u64());
  AF_TRY_ASSIGN(
      value.committed_promotion_request,
      reader.optional_identity<PromotionRequestIdTag>("population.commit_request"));
  AF_TRY_ASSIGN(value.status_detail, reader.str());
  AF_TRY_ASSIGN(value.closure_blockers,
                read_string_list(reader, "population.closure_blockers"));
  return value;
}

// identity id, generation generation, identity population,
// generation population_generation, identity task, generation task_generation,
// identity lineage, u32 parent count + identity, u32 depth,
// identity producer_worker (zero allowed), identity producer_boot (zero allowed),
// identity attempt (zero allowed),
// generation attempt_generation, identity assignment (zero allowed), epoch created_epoch,
// epoch state_changed_epoch, u8 state, u32 artifact count + ArtifactRef,
// str artifact_set_digest, u32 attempt_count, generation evidence_generation,
// generation evaluation_generation, generation selection_generation,
// bool selected, bool retained, bool promotion_eligible,
// bool promotion_requested, str diversity_key, str failure_reason,
// str superseded_by_reason
void write_candidate_record(ByteWriter& writer, const CandidateRecord& value) {
  writer.identity(value.id);
  writer.generation(value.generation);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.lineage);
  write_identity_list(writer, value.parents);
  writer.u32(value.depth);
  writer.identity(value.producer_worker);
  writer.identity(value.producer_boot);
  writer.identity(value.attempt);
  writer.generation(value.attempt_generation);
  writer.identity(value.assignment);
  writer.epoch(value.created_epoch);
  writer.epoch(value.state_changed_epoch);
  writer.u8(static_cast<std::uint8_t>(value.state));
  write_artifact_list(writer, value.artifacts);
  writer.str(value.artifact_set_digest);
  writer.u32(value.attempt_count);
  writer.generation(value.evidence_generation);
  writer.generation(value.evaluation_generation);
  writer.generation(value.selection_generation);
  writer.boolean(value.selected);
  writer.boolean(value.retained);
  writer.boolean(value.promotion_eligible);
  writer.boolean(value.promotion_requested);
  writer.str(value.diversity_key);
  writer.str(value.failure_reason);
  writer.str(value.superseded_by_reason);
}

Result<CandidateRecord> read_candidate_record(ByteReader& reader) {
  CandidateRecord value;
  AF_TRY_ASSIGN(value.id, reader.identity<CandidateIdTag>("candidate.id"));
  AF_TRY_ASSIGN(value.generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.population,
                reader.identity<PopulationIdTag>("candidate.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("candidate.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.lineage, reader.identity<LineageIdTag>("candidate.lineage"));
  AF_TRY_ASSIGN(value.parents,
                read_identity_list<CandidateIdTag>(reader, "candidate.parents"));
  AF_TRY_ASSIGN(value.depth, reader.u32());
  AF_TRY_ASSIGN(value.producer_worker,
                reader.optional_identity<WorkerIdTag>("candidate.producer_worker"));
  AF_TRY_ASSIGN(value.producer_boot,
                reader.optional_identity<WorkerBootIdTag>("candidate.producer_boot"));
  AF_TRY_ASSIGN(value.attempt, reader.optional_identity<AttemptIdTag>("candidate.attempt"));
  AF_TRY_ASSIGN(value.attempt_generation, reader.generation<AttemptIdTag>());
  AF_TRY_ASSIGN(value.assignment,
                reader.optional_identity<AssignmentIdTag>("candidate.assignment"));
  AF_TRY_ASSIGN(value.created_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.state_changed_epoch, reader.epoch());
  std::uint8_t state = 0;
  AF_TRY_ASSIGN(state, reader.u8());
  if (static_cast<std::size_t>(state) >= kCandidateStateCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "candidate state " + std::to_string(state) + " is not a declared state");
  }
  value.state = static_cast<CandidateState>(state);
  AF_TRY_ASSIGN(value.artifacts, read_artifact_list(reader, "candidate.artifacts"));
  AF_TRY_ASSIGN(value.artifact_set_digest, reader.str());
  AF_TRY_ASSIGN(value.attempt_count, reader.u32());
  AF_TRY_ASSIGN(value.evidence_generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(value.evaluation_generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(value.selection_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.selected, reader.boolean());
  AF_TRY_ASSIGN(value.retained, reader.boolean());
  AF_TRY_ASSIGN(value.promotion_eligible, reader.boolean());
  AF_TRY_ASSIGN(value.promotion_requested, reader.boolean());
  AF_TRY_ASSIGN(value.diversity_key, reader.str());
  AF_TRY_ASSIGN(value.failure_reason, reader.str());
  AF_TRY_ASSIGN(value.superseded_by_reason, reader.str());
  return value;
}

// identity candidate, generation candidate_generation, identity lineage,
// u32 parent count + identity, u32 depth, epoch created_epoch, bool retired,
// epoch retired_epoch, str retirement_reason
void write_lineage_node(ByteWriter& writer, const LineageNode& value) {
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.identity(value.lineage);
  write_identity_list(writer, value.parents);
  writer.u32(value.depth);
  writer.epoch(value.created_epoch);
  writer.boolean(value.retired);
  writer.epoch(value.retired_epoch);
  writer.str(value.retirement_reason);
}

Result<LineageNode> read_lineage_node(ByteReader& reader) {
  LineageNode value;
  AF_TRY_ASSIGN(value.candidate,
                reader.identity<CandidateIdTag>("lineage.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.lineage, reader.identity<LineageIdTag>("lineage.lineage"));
  AF_TRY_ASSIGN(value.parents, read_identity_list<CandidateIdTag>(reader, "lineage.parents"));
  AF_TRY_ASSIGN(value.depth, reader.u32());
  AF_TRY_ASSIGN(value.created_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.retired, reader.boolean());
  AF_TRY_ASSIGN(value.retired_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.retirement_reason, reader.str());
  return value;
}

// twenty u64 counters, in declaration order
void write_statistics(ByteWriter& writer, const FoundryStatistics& value) {
  writer.u64(value.task_revisions);
  writer.u64(value.policy_revisions);
  writer.u64(value.populations_created);
  writer.u64(value.populations_closed);
  writer.u64(value.candidate_slots_created);
  writer.u64(value.candidates_published);
  writer.u64(value.attempts_authorized);
  writer.u64(value.attempts_completed);
  writer.u64(value.attempts_failed);
  writer.u64(value.attempts_cancelled);
  writer.u64(value.attempts_outcome_unknown);
  writer.u64(value.evaluations_recorded);
  writer.u64(value.selections_prepared);
  writer.u64(value.selections_committed);
  writer.u64(value.retentions_committed);
  writer.u64(value.promotion_requests);
  writer.u64(value.stale_authority_rejections);
  writer.u64(value.duplicate_rejections);
  writer.u64(value.late_rejections);
  writer.u64(value.illegal_transition_rejections);
}

Result<FoundryStatistics> read_statistics(ByteReader& reader) {
  FoundryStatistics value;
  AF_TRY_ASSIGN(value.task_revisions, reader.u64());
  AF_TRY_ASSIGN(value.policy_revisions, reader.u64());
  AF_TRY_ASSIGN(value.populations_created, reader.u64());
  AF_TRY_ASSIGN(value.populations_closed, reader.u64());
  AF_TRY_ASSIGN(value.candidate_slots_created, reader.u64());
  AF_TRY_ASSIGN(value.candidates_published, reader.u64());
  AF_TRY_ASSIGN(value.attempts_authorized, reader.u64());
  AF_TRY_ASSIGN(value.attempts_completed, reader.u64());
  AF_TRY_ASSIGN(value.attempts_failed, reader.u64());
  AF_TRY_ASSIGN(value.attempts_cancelled, reader.u64());
  AF_TRY_ASSIGN(value.attempts_outcome_unknown, reader.u64());
  AF_TRY_ASSIGN(value.evaluations_recorded, reader.u64());
  AF_TRY_ASSIGN(value.selections_prepared, reader.u64());
  AF_TRY_ASSIGN(value.selections_committed, reader.u64());
  AF_TRY_ASSIGN(value.retentions_committed, reader.u64());
  AF_TRY_ASSIGN(value.promotion_requests, reader.u64());
  AF_TRY_ASSIGN(value.stale_authority_rejections, reader.u64());
  AF_TRY_ASSIGN(value.duplicate_rejections, reader.u64());
  AF_TRY_ASSIGN(value.late_rejections, reader.u64());
  AF_TRY_ASSIGN(value.illegal_transition_rejections, reader.u64());
  return value;
}

// identity id, generation generation, identity candidate,
// generation candidate_generation, identity task, generation task_generation,
// identity population, generation population_generation, identity evaluator,
// str evaluator_key, u8 kind, u8 requirement_class, u8 outcome, bool complete,
// bool has_score, f64 score, epoch decided_epoch, str diagnostics,
// str evidence_digest, u64 duration_micros
void write_evaluation_record(ByteWriter& writer, const EvaluationRecord& value) {
  writer.identity(value.id);
  writer.generation(value.generation);
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.evaluator);
  writer.str(value.evaluator_key);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  writer.u8(static_cast<std::uint8_t>(value.requirement_class));
  writer.u8(static_cast<std::uint8_t>(value.outcome));
  writer.boolean(value.complete);
  writer.boolean(value.has_score);
  writer.f64(value.score);
  writer.epoch(value.decided_epoch);
  writer.str(value.diagnostics);
  writer.str(value.evidence_digest);
  writer.u64(value.duration_micros);
}

Result<EvaluationRecord> read_evaluation_record(ByteReader& reader) {
  EvaluationRecord value;
  AF_TRY_ASSIGN(value.id, reader.identity<EvaluationIdTag>("evaluation.id"));
  AF_TRY_ASSIGN(value.generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(value.candidate,
                reader.identity<CandidateIdTag>("evaluation.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("evaluation.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.population,
                reader.identity<PopulationIdTag>("evaluation.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.evaluator,
                reader.identity<EvaluatorIdTag>("evaluation.evaluator"));
  AF_TRY_ASSIGN(value.evaluator_key, reader.str());
  std::uint8_t kind = 0;
  AF_TRY_ASSIGN(kind, reader.u8());
  if (static_cast<std::size_t>(kind) >= kEvaluatorKindCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "evaluator kind " + std::to_string(kind) + " is not a declared evaluator kind");
  }
  value.kind = static_cast<EvaluatorKind>(kind);
  std::uint8_t requirement_class = 0;
  AF_TRY_ASSIGN(requirement_class, reader.u8());
  if (static_cast<std::size_t>(requirement_class) >= kRequirementClassCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "evaluation requirement class " + std::to_string(requirement_class) +
                      " is not a declared requirement class");
  }
  value.requirement_class = static_cast<RequirementClass>(requirement_class);
  std::uint8_t outcome = 0;
  AF_TRY_ASSIGN(outcome, reader.u8());
  if (static_cast<std::size_t>(outcome) >= kEvaluationOutcomeCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "evaluation outcome " + std::to_string(outcome) +
                      " is not a declared evaluation outcome");
  }
  value.outcome = static_cast<EvaluationOutcome>(outcome);
  AF_TRY_ASSIGN(value.complete, reader.boolean());
  AF_TRY_ASSIGN(value.has_score, reader.boolean());
  AF_TRY_ASSIGN(value.score, reader.f64());
  AF_TRY_ASSIGN(value.decided_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.diagnostics, reader.str());
  AF_TRY_ASSIGN(value.evidence_digest, reader.str());
  AF_TRY_ASSIGN(value.duration_micros, reader.u64());
  return value;
}

// str key, u8 kind, u8 direction, f64 weight, f64 raw_value, bool available,
// f64 contribution
void write_ranking_factor_value(ByteWriter& writer, const RankingFactorValue& value) {
  writer.str(value.key);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  writer.u8(static_cast<std::uint8_t>(value.direction));
  writer.f64(value.weight);
  writer.f64(value.raw_value);
  writer.boolean(value.available);
  writer.f64(value.contribution);
}

Result<RankingFactorValue> read_ranking_factor_value(ByteReader& reader) {
  RankingFactorValue value;
  AF_TRY_ASSIGN(value.key, reader.str());
  std::uint8_t kind = 0;
  AF_TRY_ASSIGN(kind, reader.u8());
  if (static_cast<std::size_t>(kind) >= kRankingFactorKindCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "ranking factor kind " + std::to_string(kind) +
                      " is not a declared ranking factor kind");
  }
  value.kind = static_cast<RankingFactorKind>(kind);
  std::uint8_t direction = 0;
  AF_TRY_ASSIGN(direction, reader.u8());
  if (static_cast<std::size_t>(direction) >= kRankingDirectionCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "ranking direction " + std::to_string(direction) +
                      " is not a declared ranking direction");
  }
  value.direction = static_cast<RankingDirection>(direction);
  AF_TRY_ASSIGN(value.weight, reader.f64());
  AF_TRY_ASSIGN(value.raw_value, reader.f64());
  AF_TRY_ASSIGN(value.available, reader.boolean());
  AF_TRY_ASSIGN(value.contribution, reader.f64());
  return value;
}

// identity candidate, generation candidate_generation, u32 rank,
// f64 total_score, u32 factor count + RankingFactorValue, u64 artifact_bytes,
// u32 lineage_depth, str diversity_key
void write_ranking_entry(ByteWriter& writer, const RankingEntry& value) {
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.u32(value.rank);
  writer.f64(value.total_score);
  writer.u32(static_cast<std::uint32_t>(value.factors.size()));
  for (const RankingFactorValue& factor : value.factors) {
    write_ranking_factor_value(writer, factor);
  }
  writer.u64(value.artifact_bytes);
  writer.u32(value.lineage_depth);
  writer.str(value.diversity_key);
}

Result<RankingEntry> read_ranking_entry(ByteReader& reader) {
  RankingEntry value;
  AF_TRY_ASSIGN(value.candidate,
                reader.identity<CandidateIdTag>("ranking.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.rank, reader.u32());
  AF_TRY_ASSIGN(value.total_score, reader.f64());
  std::uint32_t factor_count = 0;
  AF_TRY_ASSIGN(factor_count,
                read_count(reader, "ranking.factors", kMaxProtocolRankingFactors,
                           kMinRankingFactorValueBytes));
  value.factors.reserve(factor_count);
  for (std::uint32_t factor_index = 0; factor_index < factor_count; ++factor_index) {
    RankingFactorValue factor;
    AF_TRY_ASSIGN(factor, read_ranking_factor_value(reader));
    value.factors.push_back(std::move(factor));
  }
  AF_TRY_ASSIGN(value.artifact_bytes, reader.u64());
  AF_TRY_ASSIGN(value.lineage_depth, reader.u32());
  AF_TRY_ASSIGN(value.diversity_key, reader.str());
  return value;
}

// identity candidate, generation candidate_generation, u8 stage, u8 reason,
// str detail
void write_exclusion_entry(ByteWriter& writer, const ExclusionEntry& value) {
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.u8(static_cast<std::uint8_t>(value.stage));
  writer.u8(static_cast<std::uint8_t>(value.reason));
  writer.str(value.detail);
}

Result<ExclusionEntry> read_exclusion_entry(ByteReader& reader) {
  ExclusionEntry value;
  AF_TRY_ASSIGN(value.candidate,
                reader.identity<CandidateIdTag>("exclusion.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  std::uint8_t stage = 0;
  AF_TRY_ASSIGN(stage, reader.u8());
  if (static_cast<std::size_t>(stage) >= kSelectionStageCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "selection stage " + std::to_string(stage) + " is not a declared stage");
  }
  value.stage = static_cast<SelectionStage>(stage);
  std::uint8_t reason = 0;
  AF_TRY_ASSIGN(reason, reader.u8());
  if (static_cast<std::size_t>(reason) >= kExclusionReasonCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "exclusion reason " + std::to_string(reason) +
                      " is not a declared exclusion reason");
  }
  value.reason = static_cast<ExclusionReason>(reason);
  AF_TRY_ASSIGN(value.detail, reader.str());
  return value;
}

// generation generation, identity population, generation population_generation,
// identity task, generation task_generation, identity policy,
// generation policy_generation, epoch coordinator_epoch, u8 state,
// identity selected, generation selected_generation,
// u32 ranking count + RankingEntry, u32 excluded count + ExclusionEntry,
// str canonical_state_digest, str rationale
void write_selection_decision(ByteWriter& writer, const SelectionDecision& value) {
  writer.generation(value.generation);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.policy);
  writer.generation(value.policy_generation);
  writer.epoch(value.coordinator_epoch);
  writer.u8(static_cast<std::uint8_t>(value.state));
  writer.identity(value.selected);
  writer.generation(value.selected_generation);
  writer.u32(static_cast<std::uint32_t>(value.ranking.size()));
  for (const RankingEntry& entry : value.ranking) {
    write_ranking_entry(writer, entry);
  }
  writer.u32(static_cast<std::uint32_t>(value.excluded.size()));
  for (const ExclusionEntry& entry : value.excluded) {
    write_exclusion_entry(writer, entry);
  }
  writer.str(value.canonical_state_digest);
  writer.str(value.rationale);
}

Result<SelectionDecision> read_selection_decision(ByteReader& reader) {
  SelectionDecision value;
  AF_TRY_ASSIGN(value.generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.population,
                reader.identity<PopulationIdTag>("decision.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("decision.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.policy, reader.identity<PolicyIdTag>("decision.policy"));
  AF_TRY_ASSIGN(value.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(value.coordinator_epoch, reader.epoch());
  std::uint8_t state = 0;
  AF_TRY_ASSIGN(state, reader.u8());
  if (static_cast<std::size_t>(state) >= kSelectionDecisionStateCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "selection decision state " + std::to_string(state) +
                      " is not a declared decision state");
  }
  value.state = static_cast<SelectionDecisionState>(state);
  AF_TRY_ASSIGN(value.selected,
                reader.optional_identity<CandidateIdTag>("decision.selected"));
  AF_TRY_ASSIGN(value.selected_generation, reader.generation<CandidateIdTag>());
  std::uint32_t ranking_count = 0;
  AF_TRY_ASSIGN(ranking_count,
                read_count(reader, "decision.ranking", kMaxProtocolElements,
                           kMinRankingEntryBytes));
  value.ranking.reserve(ranking_count);
  for (std::uint32_t ranking_index = 0; ranking_index < ranking_count; ++ranking_index) {
    RankingEntry entry;
    AF_TRY_ASSIGN(entry, read_ranking_entry(reader));
    value.ranking.push_back(std::move(entry));
  }
  std::uint32_t excluded_count = 0;
  AF_TRY_ASSIGN(excluded_count,
                read_count(reader, "decision.excluded", kMaxProtocolElements,
                           kMinExclusionEntryBytes));
  value.excluded.reserve(excluded_count);
  for (std::uint32_t excluded_index = 0; excluded_index < excluded_count; ++excluded_index) {
    ExclusionEntry entry;
    AF_TRY_ASSIGN(entry, read_exclusion_entry(reader));
    value.excluded.push_back(std::move(entry));
  }
  AF_TRY_ASSIGN(value.canonical_state_digest, reader.str());
  AF_TRY_ASSIGN(value.rationale, reader.str());
  return value;
}

// identity candidate, generation candidate_generation, u8 outcome, u32 rank,
// str lineage_key, str detail
void write_retention_entry(ByteWriter& writer, const RetentionDecisionEntry& value) {
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.u8(static_cast<std::uint8_t>(value.outcome));
  writer.u32(value.rank);
  writer.str(value.lineage_key);
  writer.str(value.detail);
}

Result<RetentionDecisionEntry> read_retention_entry(ByteReader& reader) {
  RetentionDecisionEntry value;
  AF_TRY_ASSIGN(value.candidate,
                reader.identity<CandidateIdTag>("retention.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  std::uint8_t outcome = 0;
  AF_TRY_ASSIGN(outcome, reader.u8());
  if (static_cast<std::size_t>(outcome) >= kRetentionOutcomeCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "retention outcome " + std::to_string(outcome) +
                      " is not a declared retention outcome");
  }
  value.outcome = static_cast<RetentionOutcome>(outcome);
  AF_TRY_ASSIGN(value.rank, reader.u32());
  AF_TRY_ASSIGN(value.lineage_key, reader.str());
  AF_TRY_ASSIGN(value.detail, reader.str());
  return value;
}

// generation selection_generation, identity population,
// generation population_generation, identity policy,
// generation policy_generation, epoch coordinator_epoch,
// u32 entry count + RetentionDecisionEntry, u32 retained count + identity,
// u32 retired count + identity, str canonical_state_digest, bool committed
void write_retention_decision(ByteWriter& writer, const RetentionDecision& value) {
  writer.generation(value.selection_generation);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.policy);
  writer.generation(value.policy_generation);
  writer.epoch(value.coordinator_epoch);
  writer.u32(static_cast<std::uint32_t>(value.entries.size()));
  for (const RetentionDecisionEntry& entry : value.entries) {
    write_retention_entry(writer, entry);
  }
  write_identity_list(writer, value.retained);
  write_identity_list(writer, value.retired);
  writer.str(value.canonical_state_digest);
  writer.boolean(value.committed);
}

Result<RetentionDecision> read_retention_decision(ByteReader& reader) {
  RetentionDecision value;
  AF_TRY_ASSIGN(value.selection_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.population,
                reader.identity<PopulationIdTag>("retention.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.policy, reader.identity<PolicyIdTag>("retention.policy"));
  AF_TRY_ASSIGN(value.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(value.coordinator_epoch, reader.epoch());
  std::uint32_t entry_count = 0;
  AF_TRY_ASSIGN(entry_count,
                read_count(reader, "retention.entries", kMaxProtocolElements,
                           kMinRetentionEntryBytes));
  value.entries.reserve(entry_count);
  for (std::uint32_t entry_index = 0; entry_index < entry_count; ++entry_index) {
    RetentionDecisionEntry entry;
    AF_TRY_ASSIGN(entry, read_retention_entry(reader));
    value.entries.push_back(std::move(entry));
  }
  AF_TRY_ASSIGN(value.retained,
                read_identity_list<CandidateIdTag>(reader, "retention.retained"));
  AF_TRY_ASSIGN(value.retired, read_identity_list<CandidateIdTag>(reader, "retention.retired"));
  AF_TRY_ASSIGN(value.canonical_state_digest, reader.str());
  AF_TRY_ASSIGN(value.committed, reader.boolean());
  return value;
}

// str evaluator_key, u8 kind, u8 outcome, u8 requirement_class, bool complete,
// bool has_score, f64 score, generation generation, str evidence_digest
void write_promotion_evidence_summary(ByteWriter& writer,
                                      const PromotionEvidenceSummary& value) {
  writer.str(value.evaluator_key);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  writer.u8(static_cast<std::uint8_t>(value.outcome));
  writer.u8(static_cast<std::uint8_t>(value.requirement_class));
  writer.boolean(value.complete);
  writer.boolean(value.has_score);
  writer.f64(value.score);
  writer.generation(value.generation);
  writer.str(value.evidence_digest);
}

Result<PromotionEvidenceSummary> read_promotion_evidence_summary(ByteReader& reader) {
  PromotionEvidenceSummary value;
  AF_TRY_ASSIGN(value.evaluator_key, reader.str());
  std::uint8_t kind = 0;
  AF_TRY_ASSIGN(kind, reader.u8());
  if (static_cast<std::size_t>(kind) >= kEvaluatorKindCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "promotion evidence evaluator kind " + std::to_string(kind) +
                      " is not a declared evaluator kind");
  }
  value.kind = static_cast<EvaluatorKind>(kind);
  std::uint8_t outcome = 0;
  AF_TRY_ASSIGN(outcome, reader.u8());
  if (static_cast<std::size_t>(outcome) >= kEvaluationOutcomeCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "promotion evidence outcome " + std::to_string(outcome) +
                      " is not a declared evaluation outcome");
  }
  value.outcome = static_cast<EvaluationOutcome>(outcome);
  std::uint8_t requirement_class = 0;
  AF_TRY_ASSIGN(requirement_class, reader.u8());
  if (static_cast<std::size_t>(requirement_class) >= kRequirementClassCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "promotion evidence requirement class " + std::to_string(requirement_class) +
                      " is not a declared requirement class");
  }
  value.requirement_class = static_cast<RequirementClass>(requirement_class);
  AF_TRY_ASSIGN(value.complete, reader.boolean());
  AF_TRY_ASSIGN(value.has_score, reader.boolean());
  AF_TRY_ASSIGN(value.score, reader.f64());
  AF_TRY_ASSIGN(value.generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(value.evidence_digest, reader.str());
  return value;
}

// identity id, identity foundry, identity run, epoch coordinator_epoch,
// identity population, generation population_generation, identity candidate,
// generation candidate_generation, identity task, generation task_generation,
// identity lineage, generation selection_generation, identity policy,
// generation policy_generation, generation evidence_generation, u8 eligibility,
// u32 outstanding count + str, u32 artifact count + ArtifactRef,
// u64 total_artifact_bytes, str artifact_set_digest,
// u32 evidence count + PromotionEvidenceSummary, u32 ancestry count + identity,
// u32 lineage_depth, str canonical_state_digest
void write_promotion_request(ByteWriter& writer, const PromotionRequest& value) {
  writer.identity(value.id);
  writer.identity(value.foundry);
  writer.identity(value.run);
  writer.epoch(value.coordinator_epoch);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.lineage);
  writer.generation(value.selection_generation);
  writer.identity(value.policy);
  writer.generation(value.policy_generation);
  writer.generation(value.evidence_generation);
  writer.u8(static_cast<std::uint8_t>(value.eligibility));
  write_string_list(writer, value.outstanding_requirements);
  write_artifact_list(writer, value.artifacts);
  writer.u64(value.total_artifact_bytes);
  writer.str(value.artifact_set_digest);
  writer.u32(static_cast<std::uint32_t>(value.mandatory_evidence.size()));
  for (const PromotionEvidenceSummary& summary : value.mandatory_evidence) {
    write_promotion_evidence_summary(writer, summary);
  }
  write_identity_list(writer, value.ancestry);
  writer.u32(value.lineage_depth);
  writer.str(value.canonical_state_digest);
}

Result<PromotionRequest> read_promotion_request(ByteReader& reader) {
  PromotionRequest value;
  AF_TRY_ASSIGN(value.id, reader.identity<PromotionRequestIdTag>("promotion.id"));
  AF_TRY_ASSIGN(value.foundry, reader.identity<FoundryIdTag>("promotion.foundry"));
  AF_TRY_ASSIGN(value.run, reader.identity<FoundryRunIdTag>("promotion.run"));
  AF_TRY_ASSIGN(value.coordinator_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.population, reader.identity<PopulationIdTag>("promotion.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.candidate, reader.identity<CandidateIdTag>("promotion.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("promotion.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.lineage, reader.identity<LineageIdTag>("promotion.lineage"));
  AF_TRY_ASSIGN(value.selection_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.policy, reader.identity<PolicyIdTag>("promotion.policy"));
  AF_TRY_ASSIGN(value.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(value.evidence_generation, reader.generation<EvaluationIdTag>());
  std::uint8_t eligibility = 0;
  AF_TRY_ASSIGN(eligibility, reader.u8());
  if (static_cast<std::size_t>(eligibility) >= kPromotionEligibilityCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "promotion eligibility " + std::to_string(eligibility) +
                      " is not a declared eligibility state");
  }
  value.eligibility = static_cast<PromotionEligibilityState>(eligibility);
  AF_TRY_ASSIGN(value.outstanding_requirements,
                read_string_list(reader, "promotion.outstanding_requirements"));
  AF_TRY_ASSIGN(value.artifacts, read_artifact_list(reader, "promotion.artifacts"));
  AF_TRY_ASSIGN(value.total_artifact_bytes, reader.u64());
  AF_TRY_ASSIGN(value.artifact_set_digest, reader.str());
  std::uint32_t evidence_count = 0;
  AF_TRY_ASSIGN(evidence_count,
                read_count(reader, "promotion.mandatory_evidence", kMaxProtocolElements,
                           kMinPromotionEvidenceBytes));
  value.mandatory_evidence.reserve(evidence_count);
  for (std::uint32_t evidence_index = 0; evidence_index < evidence_count; ++evidence_index) {
    PromotionEvidenceSummary summary;
    AF_TRY_ASSIGN(summary, read_promotion_evidence_summary(reader));
    value.mandatory_evidence.push_back(std::move(summary));
  }
  AF_TRY_ASSIGN(value.ancestry,
                read_identity_list<CandidateIdTag>(reader, "promotion.ancestry"));
  AF_TRY_ASSIGN(value.lineage_depth, reader.u32());
  AF_TRY_ASSIGN(value.canonical_state_digest, reader.str());
  return value;
}

// u8 handoff, str sink_name, str sink_reference, str detail,
// str request_digest
void write_promotion_receipt(ByteWriter& writer, const PromotionReceipt& value) {
  writer.u8(static_cast<std::uint8_t>(value.handoff));
  writer.str(value.sink_name);
  writer.str(value.sink_reference);
  writer.str(value.detail);
  writer.str(value.request_digest);
}

Result<PromotionReceipt> read_promotion_receipt(ByteReader& reader) {
  PromotionReceipt value;
  std::uint8_t handoff = 0;
  AF_TRY_ASSIGN(handoff, reader.u8());
  if (static_cast<std::size_t>(handoff) >= kPromotionHandoffCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "promotion handoff state " + std::to_string(handoff) +
                      " is not a declared handoff state");
  }
  value.handoff = static_cast<PromotionHandoffState>(handoff);
  AF_TRY_ASSIGN(value.sink_name, reader.str());
  AF_TRY_ASSIGN(value.sink_reference, reader.str());
  AF_TRY_ASSIGN(value.detail, reader.str());
  AF_TRY_ASSIGN(value.request_digest, reader.str());
  return value;
}

// epoch coordinator_epoch, identity run, identity worker, identity boot,
// identity session, generation session_generation
void write_worker_session_authority(ByteWriter& writer, const WorkerSessionAuthority& value) {
  writer.epoch(value.coordinator_epoch);
  writer.identity(value.run);
  writer.identity(value.worker);
  writer.identity(value.boot);
  writer.identity(value.session);
  writer.generation(value.session_generation);
}

Result<WorkerSessionAuthority> read_worker_session_authority(ByteReader& reader) {
  WorkerSessionAuthority value;
  AF_TRY_ASSIGN(value.coordinator_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.run, reader.identity<FoundryRunIdTag>("session.run"));
  AF_TRY_ASSIGN(value.worker, reader.identity<WorkerIdTag>("session.worker"));
  AF_TRY_ASSIGN(value.boot, reader.identity<WorkerBootIdTag>("session.boot"));
  AF_TRY_ASSIGN(value.session, reader.identity<SessionIdTag>("session.session"));
  AF_TRY_ASSIGN(value.session_generation, reader.generation<SessionIdTag>());
  return value;
}

// the worker session block, then identity population,
// generation population_generation, identity task, generation task_generation,
// identity candidate, generation candidate_generation, identity attempt,
// generation attempt_generation, identity assignment (zero allowed)
void write_worker_operation_authority(ByteWriter& writer, const WorkerOperationAuthority& value) {
  write_worker_session_authority(writer, value.session);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.identity(value.attempt);
  writer.generation(value.attempt_generation);
  writer.identity(value.assignment);
}

Result<WorkerOperationAuthority> read_worker_operation_authority(ByteReader& reader) {
  WorkerOperationAuthority value;
  AF_TRY_ASSIGN(value.session, read_worker_session_authority(reader));
  AF_TRY_ASSIGN(value.population,
                reader.identity<PopulationIdTag>("authority.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("authority.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.candidate,
                reader.identity<CandidateIdTag>("authority.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.attempt, reader.identity<AttemptIdTag>("authority.attempt"));
  AF_TRY_ASSIGN(value.attempt_generation, reader.generation<AttemptIdTag>());
  // An operation authority is issued when the slot is authorized, before a
  // dispatch assignment exists, so the assignment is legitimately absent on the
  // wire: the worker echoes back exactly the authority it was handed. Reading it
  // strictly made that state unrepresentable and turned a stale-boot refusal
  // into an undecodable frame. The core compares whatever arrives against the
  // attempt's own assignment, so absence is a value here, not an error.
  AF_TRY_ASSIGN(value.assignment,
                reader.optional_identity<AssignmentIdTag>("authority.assignment"));
  return value;
}

// epoch coordinator_epoch, identity run, identity controller,
// identity session, generation session_generation
void write_controller_session_authority(ByteWriter& writer,
                                        const ControllerSessionAuthority& value) {
  writer.epoch(value.coordinator_epoch);
  writer.identity(value.run);
  writer.identity(value.controller);
  writer.identity(value.session);
  writer.generation(value.session_generation);
}

Result<ControllerSessionAuthority> read_controller_session_authority(ByteReader& reader) {
  ControllerSessionAuthority value;
  AF_TRY_ASSIGN(value.coordinator_epoch, reader.epoch());
  AF_TRY_ASSIGN(value.run, reader.identity<FoundryRunIdTag>("session.run"));
  // The controller identity is nullable on the wire. A session that has not
  // been told which controller it is speaking for sends none, and the
  // coordinator resolves the field to the controller authority it issued for
  // its own operator surface rather than refusing the operation. A controller
  // identity a sender does name is still carried through and still checked
  // against durable state wherever the operation it authorizes needs it.
  AF_TRY_ASSIGN(value.controller,
                reader.optional_identity<ControllerIdTag>("session.controller"));
  AF_TRY_ASSIGN(value.session, reader.identity<SessionIdTag>("session.session"));
  AF_TRY_ASSIGN(value.session_generation, reader.generation<SessionIdTag>());
  return value;
}

// the controller session block, then identity population,
// generation population_generation, identity task, generation task_generation,
// identity policy, generation policy_generation
void write_controller_operation_authority(ByteWriter& writer,
                                          const ControllerOperationAuthority& value) {
  write_controller_session_authority(writer, value.session);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.policy);
  writer.generation(value.policy_generation);
}

Result<ControllerOperationAuthority> read_controller_operation_authority(ByteReader& reader) {
  ControllerOperationAuthority value;
  AF_TRY_ASSIGN(value.session, read_controller_session_authority(reader));
  AF_TRY_ASSIGN(value.population,
                reader.identity<PopulationIdTag>("authority.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("authority.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.policy, reader.identity<PolicyIdTag>("authority.policy"));
  AF_TRY_ASSIGN(value.policy_generation, reader.generation<PolicyIdTag>());
  return value;
}

// identity attempt, generation attempt_generation, identity assignment,
// identity population, generation population_generation, identity task,
// generation task_generation, identity candidate,
// generation candidate_generation, str task_name, str objective,
// u32 required_outputs count + str, u32 input_files count + (str, str),
// str workspace_root, str reference_strategy
void write_attempt_package(ByteWriter& writer, const AttemptPackage& value) {
  writer.identity(value.attempt);
  writer.generation(value.attempt_generation);
  writer.identity(value.assignment);
  writer.identity(value.population);
  writer.generation(value.population_generation);
  writer.identity(value.task);
  writer.generation(value.task_generation);
  writer.identity(value.candidate);
  writer.generation(value.candidate_generation);
  writer.str(value.task_name);
  writer.str(value.objective);
  write_string_list(writer, value.required_outputs);
  writer.u32(static_cast<std::uint32_t>(value.input_files.size()));
  for (const std::pair<std::string, std::string>& input : value.input_files) {
    writer.str(input.first);
    writer.str(input.second);
  }
  writer.str(value.workspace_root);
  writer.str(value.reference_strategy);
}

Result<AttemptPackage> read_attempt_package(ByteReader& reader) {
  AttemptPackage value;
  AF_TRY_ASSIGN(value.attempt, reader.identity<AttemptIdTag>("package.attempt"));
  AF_TRY_ASSIGN(value.attempt_generation, reader.generation<AttemptIdTag>());
  AF_TRY_ASSIGN(value.assignment,
                reader.identity<AssignmentIdTag>("package.assignment"));
  AF_TRY_ASSIGN(value.population,
                reader.identity<PopulationIdTag>("package.population"));
  AF_TRY_ASSIGN(value.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(value.task, reader.identity<TaskIdTag>("package.task"));
  AF_TRY_ASSIGN(value.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(value.candidate,
                reader.identity<CandidateIdTag>("package.candidate"));
  AF_TRY_ASSIGN(value.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(value.task_name, reader.str());
  AF_TRY_ASSIGN(value.objective, reader.str());
  AF_TRY_ASSIGN(value.required_outputs,
                read_string_list(reader, "package.required_outputs"));
  std::uint32_t input_count = 0;
  AF_TRY_ASSIGN(input_count,
                read_count(reader, "package.input_files", kMaxProtocolElements,
                           2 * kMinStringBytes));
  value.input_files.reserve(input_count);
  for (std::uint32_t input_index = 0; input_index < input_count; ++input_index) {
    std::pair<std::string, std::string> input;
    AF_TRY_ASSIGN(input.first, reader.str());
    AF_TRY_ASSIGN(input.second, reader.str());
    value.input_files.push_back(std::move(input));
  }
  AF_TRY_ASSIGN(value.workspace_root, reader.str());
  AF_TRY_ASSIGN(value.reference_strategy, reader.str());
  return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Message type names
// ---------------------------------------------------------------------------

std::string_view message_type_name(MessageType type) noexcept {
  switch (type) {
    case MessageType::Invalid:
      return "Invalid";
    case MessageType::Hello:
      return "Hello";
    case MessageType::HelloAck:
      return "HelloAck";
    case MessageType::WorkerReady:
      return "WorkerReady";
    case MessageType::Revalidate:
      return "Revalidate";
    case MessageType::RevalidateAck:
      return "RevalidateAck";
    case MessageType::Heartbeat:
      return "Heartbeat";
    case MessageType::HeartbeatAck:
      return "HeartbeatAck";
    case MessageType::CreateTask:
      return "CreateTask";
    case MessageType::TaskCreated:
      return "TaskCreated";
    case MessageType::DefinePolicy:
      return "DefinePolicy";
    case MessageType::PolicyDefined:
      return "PolicyDefined";
    case MessageType::CreatePopulation:
      return "CreatePopulation";
    case MessageType::PopulationCreated:
      return "PopulationCreated";
    case MessageType::StartPopulation:
      return "StartPopulation";
    case MessageType::PopulationStarted:
      return "PopulationStarted";
    case MessageType::ClosePopulation:
      return "ClosePopulation";
    case MessageType::PopulationClosed:
      return "PopulationClosed";
    case MessageType::AdvancePopulation:
      return "AdvancePopulation";
    case MessageType::PopulationAdvanced:
      return "PopulationAdvanced";
    case MessageType::RequestSelection:
      return "RequestSelection";
    case MessageType::SelectionDecisionMessage:
      return "SelectionDecisionMessage";
    case MessageType::RequestRetention:
      return "RequestRetention";
    case MessageType::RetentionDecisionMessage:
      return "RetentionDecisionMessage";
    case MessageType::RequestPromotion:
      return "RequestPromotion";
    case MessageType::PromotionRequestMessage:
      return "PromotionRequestMessage";
    case MessageType::RequestRevalidation:
      return "RequestRevalidation";
    case MessageType::RevalidationApplied:
      return "RevalidationApplied";
    case MessageType::CancelAttemptRequest:
      return "CancelAttemptRequest";
    case MessageType::AttemptCancelled:
      return "AttemptCancelled";
    case MessageType::AssignAttempt:
      return "AssignAttempt";
    case MessageType::AttemptAccepted:
      return "AttemptAccepted";
    case MessageType::AttemptResult:
      return "AttemptResult";
    case MessageType::CandidatePublish:
      return "CandidatePublish";
    case MessageType::PublishAck:
      return "PublishAck";
    case MessageType::AttemptFailed:
      return "AttemptFailed";
    case MessageType::EvaluationReport:
      return "EvaluationReport";
    case MessageType::SelfReport:
      return "SelfReport";
    case MessageType::QueryPopulation:
      return "QueryPopulation";
    case MessageType::PopulationDetail:
      return "PopulationDetail";
    case MessageType::QueryCandidate:
      return "QueryCandidate";
    case MessageType::CandidateDetail:
      return "CandidateDetail";
    case MessageType::QueryLineage:
      return "QueryLineage";
    case MessageType::LineageDetail:
      return "LineageDetail";
    case MessageType::QueryStatistics:
      return "QueryStatistics";
    case MessageType::StatisticsDetail:
      return "StatisticsDetail";
    case MessageType::QueryAudit:
      return "QueryAudit";
    case MessageType::AuditDetail:
      return "AuditDetail";
    case MessageType::ShutdownCoordinator:
      return "ShutdownCoordinator";
    case MessageType::ShutdownAck:
      return "ShutdownAck";
    case MessageType::ErrorResponse:
      return "ErrorResponse";
    case MessageType::ControlAck:
      return "ControlAck";
  }
  return "MessageTypeUnknown";
}

// ---------------------------------------------------------------------------
// Frame layer
// ---------------------------------------------------------------------------

// The wire header is written in this order and is always 28 bytes:
//
//   u32 magic, u16 version, u16 type, u32 flags, u64 sequence,
//   u32 payload_length, u32 payload_crc32c
//
// payload_length and payload_crc32c are derived from the payload that is
// actually being framed rather than copied from the caller's header: a frame
// whose declared length or digest disagrees with its bytes is a corrupt frame
// by construction, and the encoder must not be able to mint one.
std::string encode_frame(const FrameHeader& header, std::string_view payload) {
  if (payload.size() > static_cast<std::size_t>(kAbsoluteMaxFramePayloadBytes)) {
    // The frozen signature returns the frame buffer itself, so the rejection is
    // reported as an empty buffer. A frame is never shorter than its 28 byte
    // header, so an empty result can only mean "not encoded"; callers that need
    // to distinguish must check for it before sending.
    return std::string();
  }
  ByteWriter writer;
  writer.u32(header.magic);
  writer.u16(header.version);
  writer.u16(static_cast<std::uint16_t>(header.type));
  writer.u32(header.flags);
  writer.u64(header.sequence);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u32(crc32c(payload));
  // The payload follows the fixed header as raw bytes. ByteWriter::bytes() is
  // the length-prefixed string form used inside payloads, so it is deliberately
  // not used here: a frame that length-prefixed its own payload would declare a
  // different CRC base than the one it wrote.
  std::string frame = writer.take();
  frame.append(payload.data(), payload.size());
  return frame;
}

Result<Frame> decode_frame(std::string_view buffer, std::size_t* consumed) {
  if (consumed == nullptr) {
    return Status(ErrorCode::InvalidArgument,
                  "decode_frame requires a consumed out-parameter");
  }
  if (buffer.size() < kFrameHeaderBytes) {
    // A partial header consumes nothing: the caller must prepend more bytes and
    // retry with the whole buffer. consumed is deliberately left untouched.
    return Status(ErrorCode::FrameTruncated, "incomplete");
  }

  const std::uint32_t magic = static_cast<std::uint32_t>(load_little_endian(buffer.substr(0, 4)));
  if (magic != kFrameMagic) {
    // Fatal: the byte stream is not this protocol at all, so the caller closes
    // the connection instead of trying to resynchronise.
    return Status(ErrorCode::ProtocolViolation,
                  "frame magic " + to_hex32(magic) + " does not match the expected " +
                      to_hex32(kFrameMagic));
  }

  const std::uint16_t version =
      static_cast<std::uint16_t>(load_little_endian(buffer.substr(4, 2)));
  if (version != kProtocolVersion) {
    return Status(ErrorCode::ProtocolViolation,
                  "frame protocol version " + std::to_string(version) +
                      " is not the supported version " + std::to_string(kProtocolVersion));
  }

  const std::uint32_t payload_length =
      static_cast<std::uint32_t>(load_little_endian(buffer.substr(20, 4)));
  if (payload_length > kMaxFramePayloadBytes) {
    // Checked before a single byte is allocated for the payload.
    return Status(ErrorCode::FrameTooLarge,
                  "frame payload length " + std::to_string(payload_length) +
                      " exceeds the accepted maximum " + std::to_string(kMaxFramePayloadBytes));
  }

  const std::size_t total = kFrameHeaderBytes + static_cast<std::size_t>(payload_length);
  if (buffer.size() < total) {
    return Status(ErrorCode::FrameTruncated, "incomplete");
  }

  const std::uint32_t declared_crc =
      static_cast<std::uint32_t>(load_little_endian(buffer.substr(24, 4)));
  const std::string_view payload = buffer.substr(kFrameHeaderBytes, payload_length);
  const std::uint32_t actual_crc = crc32c(payload);
  if (actual_crc != declared_crc) {
    return Status(ErrorCode::FrameCorrupt,
                  "frame payload CRC-32C " + to_hex32(actual_crc) +
                      " does not match the declared " + to_hex32(declared_crc));
  }

  Frame frame;
  frame.header.magic = magic;
  frame.header.version = version;
  frame.header.type =
      static_cast<MessageType>(static_cast<std::uint16_t>(load_little_endian(buffer.substr(6, 2))));
  frame.header.flags = static_cast<std::uint32_t>(load_little_endian(buffer.substr(8, 4)));
  frame.header.sequence = load_little_endian(buffer.substr(12, 8));
  frame.header.payload_length = payload_length;
  frame.header.payload_crc32c = declared_crc;
  frame.payload = std::string(payload);

  *consumed = total;
  return frame;
}

// A split read is not a corrupt frame. FrameTruncated means "the bytes seen so
// far are a valid prefix of a frame and the caller must read more"; every other
// transport error means the bytes seen are wrong. The transport layer uses this
// predicate to make that distinction without matching on individual codes, so
// only FrameTruncated reports true here.
bool status_is_incomplete(const Status& status) noexcept {
  return status.code() == ErrorCode::FrameTruncated;
}

// ---------------------------------------------------------------------------
// ByteWriter
// ---------------------------------------------------------------------------

void ByteWriter::u8(std::uint8_t value) {
  buffer_.push_back(static_cast<char>(static_cast<unsigned char>(value)));
}

void ByteWriter::u16(std::uint16_t value) {
  append_little_endian<std::uint16_t>(buffer_, value);
}

void ByteWriter::u32(std::uint32_t value) {
  append_little_endian<std::uint32_t>(buffer_, value);
}

void ByteWriter::u64(std::uint64_t value) {
  append_little_endian<std::uint64_t>(buffer_, value);
}

void ByteWriter::i64(std::int64_t value) {
  append_little_endian<std::uint64_t>(buffer_, std::bit_cast<std::uint64_t>(value));
}

void ByteWriter::f64(double value) {
  append_little_endian<std::uint64_t>(buffer_, std::bit_cast<std::uint64_t>(value));
}

void ByteWriter::boolean(bool value) {
  u8(static_cast<std::uint8_t>(value ? 1u : 0u));
}

void ByteWriter::str(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  buffer_.append(value.data(), value.size());
}

// bytes() is the same wire form as str(): a u32 length followed by that many
// raw bytes. The two names exist so that a caller can state which of the two
// things it means, and the encoding is deliberately identical.
void ByteWriter::bytes(std::string_view value) {
  str(value);
}

// ---------------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------------

// The single bounds-checked primitive. Every read in this file, including every
// fixed-width integer and every string, goes through it, so there is exactly
// one place where "not enough bytes left" is decided.
Result<std::string_view> ByteReader::take(std::size_t count) {
  const std::size_t available = data_.size() - offset_;
  if (count > available) {
    return Status(ErrorCode::MalformedEncoding,
                  "field needs " + std::to_string(count) + " bytes but only " +
                      std::to_string(available) + " remain");
  }
  const std::string_view slice = data_.substr(offset_, count);
  offset_ += count;
  return slice;
}

Result<std::uint8_t> ByteReader::u8() {
  std::string_view slice;
  AF_TRY_ASSIGN(slice, take(1));
  return static_cast<std::uint8_t>(static_cast<unsigned char>(slice[0]));
}

Result<std::uint16_t> ByteReader::u16() {
  std::string_view slice;
  AF_TRY_ASSIGN(slice, take(2));
  return static_cast<std::uint16_t>(load_little_endian(slice));
}

Result<std::uint32_t> ByteReader::u32() {
  std::string_view slice;
  AF_TRY_ASSIGN(slice, take(4));
  return static_cast<std::uint32_t>(load_little_endian(slice));
}

Result<std::uint64_t> ByteReader::u64() {
  std::string_view slice;
  AF_TRY_ASSIGN(slice, take(8));
  return load_little_endian(slice);
}

Result<std::int64_t> ByteReader::i64() {
  std::string_view slice;
  AF_TRY_ASSIGN(slice, take(8));
  return std::bit_cast<std::int64_t>(load_little_endian(slice));
}

Result<double> ByteReader::f64() {
  std::string_view slice;
  AF_TRY_ASSIGN(slice, take(8));
  return std::bit_cast<double>(load_little_endian(slice));
}

Result<bool> ByteReader::boolean() {
  std::uint8_t raw = 0;
  AF_TRY_ASSIGN(raw, u8());
  if (raw > 1u) {
    // Only the canonical 0 and 1 are accepted. A wider "nonzero means true"
    // rule would let a sender smuggle a second encoding of the same value into
    // a durable record that other systems digest.
    return Status(ErrorCode::MalformedEncoding,
                  "boolean field carries " + std::to_string(raw) +
                      " instead of the canonical 0 or 1");
  }
  return raw != 0u;
}

Result<std::string> ByteReader::str() {
  std::uint32_t length = 0;
  AF_TRY_ASSIGN(length, u32());
  // The declared length is checked against the bytes that remain before the
  // string is materialised, so a hostile length can never resize a buffer.
  if (static_cast<std::size_t>(length) > remaining()) {
    return Status(ErrorCode::MalformedEncoding,
                  "string declares " + std::to_string(length) + " bytes but only " +
                      std::to_string(remaining()) + " remain");
  }
  std::string_view slice;
  AF_TRY_ASSIGN(slice, take(static_cast<std::size_t>(length)));
  return std::string(slice);
}

Result<std::string> ByteReader::bytes() {
  return str();
}

Result<CoordinatorEpoch> ByteReader::epoch() {
  std::uint64_t raw = 0;
  AF_TRY_ASSIGN(raw, u64());
  return CoordinatorEpoch::from_value(raw);
}

Status ByteReader::require_exhausted() const {
  if (remaining() > 0) {
    return Status(ErrorCode::MalformedEncoding,
                  "payload carries " + std::to_string(remaining()) + " trailing bytes");
  }
  return Status();
}

// ---------------------------------------------------------------------------
// Session messages
// ---------------------------------------------------------------------------

// u8 role, identity run (nullable), identity worker (nullable), identity boot
// (nullable), identity controller (nullable), str label, str capability,
// u32 process_id, epoch observed_epoch
//
// Every identity a Hello may carry is nullable. A first connection has no run
// to name: the run, the epoch and the session are what the HelloAck hands back,
// so requiring the sender to already know them would make the handshake
// impossible on exactly the connection it exists for.
Result<std::string> encode_payload(const HelloMessage& message) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(message.role));
  writer.identity(message.run);
  writer.identity(message.worker);
  writer.identity(message.boot);
  writer.identity(message.controller);
  writer.str(message.label);
  writer.str(message.capability);
  writer.u32(message.process_id);
  writer.epoch(message.observed_epoch);
  return writer.take();
}

Result<HelloMessage> decode_hello(std::string_view payload) {
  ByteReader reader(payload);
  HelloMessage message;
  std::uint8_t role = 0;
  AF_TRY_ASSIGN(role, reader.u8());
  if (static_cast<std::size_t>(role) >= kSessionRoleCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "hello role " + std::to_string(role) + " is not a declared session role");
  }
  message.role = static_cast<SessionRole>(role);
  AF_TRY_ASSIGN(message.run, reader.optional_identity<FoundryRunIdTag>("hello.run"));
  AF_TRY_ASSIGN(message.worker, reader.optional_identity<WorkerIdTag>("hello.worker"));
  AF_TRY_ASSIGN(message.boot, reader.optional_identity<WorkerBootIdTag>("hello.boot"));
  AF_TRY_ASSIGN(message.controller,
                reader.optional_identity<ControllerIdTag>("hello.controller"));
  AF_TRY_ASSIGN(message.label, reader.str());
  AF_TRY_ASSIGN(message.capability, reader.str());
  AF_TRY_ASSIGN(message.process_id, reader.u32());
  AF_TRY_ASSIGN(message.observed_epoch, reader.epoch());
  AF_TRY(reader.require_exhausted());
  return message;
}

// epoch coordinator_epoch, identity run, identity session,
// generation session_generation, bool revalidation_required, str detail
Result<std::string> encode_payload(const HelloAckMessage& message) {
  ByteWriter writer;
  writer.epoch(message.coordinator_epoch);
  writer.identity(message.run);
  writer.identity(message.session);
  writer.generation(message.session_generation);
  writer.boolean(message.revalidation_required);
  writer.str(message.detail);
  return writer.take();
}

Result<HelloAckMessage> decode_hello_ack(std::string_view payload) {
  ByteReader reader(payload);
  HelloAckMessage message;
  AF_TRY_ASSIGN(message.coordinator_epoch, reader.epoch());
  AF_TRY_ASSIGN(message.run, reader.identity<FoundryRunIdTag>("hello_ack.run"));
  AF_TRY_ASSIGN(message.session, reader.identity<SessionIdTag>("hello_ack.session"));
  AF_TRY_ASSIGN(message.session_generation, reader.generation<SessionIdTag>());
  AF_TRY_ASSIGN(message.revalidation_required, reader.boolean());
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker session block, then str capability
Result<std::string> encode_payload(const WorkerReadyMessage& message) {
  ByteWriter writer;
  write_worker_session_authority(writer, message.session);
  writer.str(message.capability);
  return writer.take();
}

Result<WorkerReadyMessage> decode_worker_ready(std::string_view payload) {
  ByteReader reader(payload);
  WorkerReadyMessage message;
  AF_TRY_ASSIGN(message.session, read_worker_session_authority(reader));
  AF_TRY_ASSIGN(message.capability, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker session block, then str detail
Result<std::string> encode_payload(const RevalidateMessage& message) {
  ByteWriter writer;
  write_worker_session_authority(writer, message.session);
  writer.str(message.detail);
  return writer.take();
}

Result<RevalidateMessage> decode_revalidate(std::string_view payload) {
  ByteReader reader(payload);
  RevalidateMessage message;
  AF_TRY_ASSIGN(message.session, read_worker_session_authority(reader));
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker session block, then bool accepted, str detail
Result<std::string> encode_payload(const RevalidateAckMessage& message) {
  ByteWriter writer;
  write_worker_session_authority(writer, message.session);
  writer.boolean(message.accepted);
  writer.str(message.detail);
  return writer.take();
}

Result<RevalidateAckMessage> decode_revalidate_ack(std::string_view payload) {
  ByteReader reader(payload);
  RevalidateAckMessage message;
  AF_TRY_ASSIGN(message.session, read_worker_session_authority(reader));
  AF_TRY_ASSIGN(message.accepted, reader.boolean());
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker session block, then u64 sequence
Result<std::string> encode_payload(const HeartbeatMessage& message) {
  ByteWriter writer;
  write_worker_session_authority(writer, message.session);
  writer.u64(message.sequence);
  return writer.take();
}

Result<HeartbeatMessage> decode_heartbeat(std::string_view payload) {
  ByteReader reader(payload);
  HeartbeatMessage message;
  AF_TRY_ASSIGN(message.session, read_worker_session_authority(reader));
  AF_TRY_ASSIGN(message.sequence, reader.u64());
  AF_TRY(reader.require_exhausted());
  return message;
}

// ---------------------------------------------------------------------------
// Control plane
// ---------------------------------------------------------------------------

// the controller session block, then TaskSpec
Result<std::string> encode_payload(const CreateTaskMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  write_task_spec(writer, message.task);
  return writer.take();
}

Result<CreateTaskMessage> decode_create_task(std::string_view payload) {
  ByteReader reader(payload);
  CreateTaskMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY_ASSIGN(message.task, read_task_spec(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity task, generation generation, str content_digest, str detail
Result<std::string> encode_payload(const TaskCreatedMessage& message) {
  ByteWriter writer;
  writer.identity(message.task);
  writer.generation(message.generation);
  writer.str(message.content_digest);
  writer.str(message.detail);
  return writer.take();
}

Result<TaskCreatedMessage> decode_task_created(std::string_view payload) {
  ByteReader reader(payload);
  TaskCreatedMessage message;
  AF_TRY_ASSIGN(message.task,
                reader.identity<TaskIdTag>("task_created.task"));
  AF_TRY_ASSIGN(message.generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(message.content_digest, reader.str());
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller session block, then FoundryPolicy
Result<std::string> encode_payload(const DefinePolicyMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  write_policy(writer, message.policy);
  return writer.take();
}

Result<DefinePolicyMessage> decode_define_policy(std::string_view payload) {
  ByteReader reader(payload);
  DefinePolicyMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY_ASSIGN(message.policy, read_policy(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity policy, generation generation, str content_digest
Result<std::string> encode_payload(const PolicyDefinedMessage& message) {
  ByteWriter writer;
  writer.identity(message.policy);
  writer.generation(message.generation);
  writer.str(message.content_digest);
  return writer.take();
}

Result<PolicyDefinedMessage> decode_policy_defined(std::string_view payload) {
  ByteReader reader(payload);
  PolicyDefinedMessage message;
  AF_TRY_ASSIGN(message.policy,
                reader.identity<PolicyIdTag>("policy_defined.policy"));
  AF_TRY_ASSIGN(message.generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(message.content_digest, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller session block, then PopulationSpec
Result<std::string> encode_payload(const CreatePopulationMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  write_population_spec(writer, message.spec);
  return writer.take();
}

Result<CreatePopulationMessage> decode_create_population(std::string_view payload) {
  ByteReader reader(payload);
  CreatePopulationMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY_ASSIGN(message.spec, read_population_spec(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity population, generation generation
Result<std::string> encode_payload(const PopulationCreatedMessage& message) {
  ByteWriter writer;
  writer.identity(message.population);
  writer.generation(message.generation);
  return writer.take();
}

Result<PopulationCreatedMessage> decode_population_created(std::string_view payload) {
  ByteReader reader(payload);
  PopulationCreatedMessage message;
  AF_TRY_ASSIGN(message.population,
                reader.identity<PopulationIdTag>("population_created.population"));
  AF_TRY_ASSIGN(message.generation, reader.generation<PopulationIdTag>());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block
Result<std::string> encode_payload(const StartPopulationMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  return writer.take();
}

Result<StartPopulationMessage> decode_start_population(std::string_view payload) {
  ByteReader reader(payload);
  StartPopulationMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity population, generation generation, u8 state, str detail
Result<std::string> encode_payload(const PopulationStateMessage& message) {
  ByteWriter writer;
  writer.identity(message.population);
  writer.generation(message.generation);
  writer.u8(static_cast<std::uint8_t>(message.state));
  writer.str(message.detail);
  return writer.take();
}

Result<PopulationStateMessage> decode_population_state(std::string_view payload) {
  ByteReader reader(payload);
  PopulationStateMessage message;
  AF_TRY_ASSIGN(message.population,
                reader.identity<PopulationIdTag>("population_state.population"));
  AF_TRY_ASSIGN(message.generation, reader.generation<PopulationIdTag>());
  std::uint8_t state = 0;
  AF_TRY_ASSIGN(state, reader.u8());
  if (static_cast<std::size_t>(state) >= kPopulationStateCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "population state " + std::to_string(state) + " is not a declared state");
  }
  message.state = static_cast<PopulationState>(state);
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block
Result<std::string> encode_payload(const ClosePopulationMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  return writer.take();
}

Result<ClosePopulationMessage> decode_close_population(std::string_view payload) {
  ByteReader reader(payload);
  ClosePopulationMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block, then str next_name
Result<std::string> encode_payload(const AdvancePopulationMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  writer.str(message.next_name);
  return writer.take();
}

Result<AdvancePopulationMessage> decode_advance_population(std::string_view payload) {
  ByteReader reader(payload);
  AdvancePopulationMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY_ASSIGN(message.next_name, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity predecessor, identity successor, generation successor_generation,
// identity seed_candidate, generation seed_candidate_generation, str detail
Result<std::string> encode_payload(const PopulationAdvancedMessage& message) {
  ByteWriter writer;
  writer.identity(message.predecessor);
  writer.identity(message.successor);
  writer.generation(message.successor_generation);
  writer.identity(message.seed_candidate);
  writer.generation(message.seed_candidate_generation);
  writer.str(message.detail);
  return writer.take();
}

Result<PopulationAdvancedMessage> decode_population_advanced(std::string_view payload) {
  ByteReader reader(payload);
  PopulationAdvancedMessage message;
  AF_TRY_ASSIGN(
      message.predecessor,
      reader.identity<PopulationIdTag>("population_advanced.predecessor"));
  AF_TRY_ASSIGN(
      message.successor,
      reader.identity<PopulationIdTag>("population_advanced.successor"));
  AF_TRY_ASSIGN(message.successor_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(
      message.seed_candidate,
      reader.identity<CandidateIdTag>("population_advanced.seed_candidate"));
  AF_TRY_ASSIGN(message.seed_candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block
Result<std::string> encode_payload(const RequestSelectionMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  return writer.take();
}

Result<RequestSelectionMessage> decode_request_selection(std::string_view payload) {
  ByteReader reader(payload);
  RequestSelectionMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block
Result<std::string> encode_payload(const RequestRetentionMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  return writer.take();
}

Result<RequestRetentionMessage> decode_request_retention(std::string_view payload) {
  ByteReader reader(payload);
  RequestRetentionMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block
Result<std::string> encode_payload(const RequestPromotionMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  return writer.take();
}

Result<RequestPromotionMessage> decode_request_promotion(std::string_view payload) {
  ByteReader reader(payload);
  RequestPromotionMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block, then str reason
Result<std::string> encode_payload(const RequestRevalidationMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  writer.str(message.reason);
  return writer.take();
}

Result<RequestRevalidationMessage> decode_request_revalidation(std::string_view payload) {
  ByteReader reader(payload);
  RequestRevalidationMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY_ASSIGN(message.reason, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller operation authority block, then identity attempt,
// generation attempt_generation, str reason
Result<std::string> encode_payload(const CancelAttemptRequestMessage& message) {
  ByteWriter writer;
  write_controller_operation_authority(writer, message.authority);
  writer.identity(message.attempt);
  writer.generation(message.attempt_generation);
  writer.str(message.reason);
  return writer.take();
}

Result<CancelAttemptRequestMessage> decode_cancel_attempt_request(std::string_view payload) {
  ByteReader reader(payload);
  CancelAttemptRequestMessage message;
  AF_TRY_ASSIGN(message.authority, read_controller_operation_authority(reader));
  AF_TRY_ASSIGN(message.attempt,
                reader.identity<AttemptIdTag>("cancel_request.attempt"));
  AF_TRY_ASSIGN(message.attempt_generation, reader.generation<AttemptIdTag>());
  AF_TRY_ASSIGN(message.reason, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity attempt, u8 state, str detail
Result<std::string> encode_payload(const AttemptCancelledMessage& message) {
  ByteWriter writer;
  writer.identity(message.attempt);
  writer.u8(static_cast<std::uint8_t>(message.state));
  writer.str(message.detail);
  return writer.take();
}

Result<AttemptCancelledMessage> decode_attempt_cancelled(std::string_view payload) {
  ByteReader reader(payload);
  AttemptCancelledMessage message;
  AF_TRY_ASSIGN(message.attempt,
                reader.identity<AttemptIdTag>("attempt_cancelled.attempt"));
  std::uint8_t state = 0;
  AF_TRY_ASSIGN(state, reader.u8());
  if (static_cast<std::size_t>(state) >= kAttemptStateCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "attempt state " + std::to_string(state) + " is not a declared state");
  }
  message.state = static_cast<AttemptState>(state);
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// ---------------------------------------------------------------------------
// Data plane
// ---------------------------------------------------------------------------

// the worker session block, then AttemptPackage
Result<std::string> encode_payload(const AssignAttemptMessage& message) {
  ByteWriter writer;
  write_worker_session_authority(writer, message.session);
  write_attempt_package(writer, message.package);
  return writer.take();
}

Result<AssignAttemptMessage> decode_assign_attempt(std::string_view payload) {
  ByteReader reader(payload);
  AssignAttemptMessage message;
  AF_TRY_ASSIGN(message.session, read_worker_session_authority(reader));
  AF_TRY_ASSIGN(message.package, read_attempt_package(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker operation authority block
Result<std::string> encode_payload(const AttemptAcceptedMessage& message) {
  ByteWriter writer;
  write_worker_operation_authority(writer, message.authority);
  return writer.take();
}

Result<AttemptAcceptedMessage> decode_attempt_accepted(std::string_view payload) {
  ByteReader reader(payload);
  AttemptAcceptedMessage message;
  AF_TRY_ASSIGN(message.authority, read_worker_operation_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker operation authority block, then str detail, u64 duration_micros
Result<std::string> encode_payload(const AttemptResultMessage& message) {
  ByteWriter writer;
  write_worker_operation_authority(writer, message.authority);
  writer.str(message.detail);
  writer.u64(message.duration_micros);
  return writer.take();
}

Result<AttemptResultMessage> decode_attempt_result(std::string_view payload) {
  ByteReader reader(payload);
  AttemptResultMessage message;
  AF_TRY_ASSIGN(message.authority, read_worker_operation_authority(reader));
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY_ASSIGN(message.duration_micros, reader.u64());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker operation authority block, then u32 artifact count + ArtifactRef,
// str declared_strategy, bool self_reported_success, str self_report_detail
Result<std::string> encode_payload(const CandidatePublishMessage& message) {
  ByteWriter writer;
  write_worker_operation_authority(writer, message.authority);
  write_artifact_list(writer, message.artifacts);
  writer.str(message.declared_strategy);
  writer.boolean(message.self_reported_success);
  writer.str(message.self_report_detail);
  return writer.take();
}

Result<CandidatePublishMessage> decode_candidate_publish(std::string_view payload) {
  ByteReader reader(payload);
  CandidatePublishMessage message;
  AF_TRY_ASSIGN(message.authority, read_worker_operation_authority(reader));
  AF_TRY_ASSIGN(message.artifacts, read_artifact_list(reader, "publish.artifacts"));
  AF_TRY_ASSIGN(message.declared_strategy, reader.str());
  AF_TRY_ASSIGN(message.self_reported_success, reader.boolean());
  AF_TRY_ASSIGN(message.self_report_detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity candidate, generation candidate_generation, bool accepted, str detail
Result<std::string> encode_payload(const PublishAckMessage& message) {
  ByteWriter writer;
  writer.identity(message.candidate);
  writer.generation(message.candidate_generation);
  writer.boolean(message.accepted);
  writer.str(message.detail);
  return writer.take();
}

Result<PublishAckMessage> decode_publish_ack(std::string_view payload) {
  ByteReader reader(payload);
  PublishAckMessage message;
  AF_TRY_ASSIGN(message.candidate,
                reader.identity<CandidateIdTag>("publish_ack.candidate"));
  AF_TRY_ASSIGN(message.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(message.accepted, reader.boolean());
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker operation authority block, then str reason
Result<std::string> encode_payload(const AttemptFailedMessage& message) {
  ByteWriter writer;
  write_worker_operation_authority(writer, message.authority);
  writer.str(message.reason);
  return writer.take();
}

Result<AttemptFailedMessage> decode_attempt_failed(std::string_view payload) {
  ByteReader reader(payload);
  AttemptFailedMessage message;
  AF_TRY_ASSIGN(message.authority, read_worker_operation_authority(reader));
  AF_TRY_ASSIGN(message.reason, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// EvaluationRecord, in its canonical field order
Result<std::string> encode_payload(const EvaluationReportMessage& message) {
  ByteWriter writer;
  write_evaluation_record(writer, message.record);
  return writer.take();
}

Result<EvaluationReportMessage> decode_evaluation_report(std::string_view payload) {
  ByteReader reader(payload);
  EvaluationReportMessage message;
  AF_TRY_ASSIGN(message.record, read_evaluation_record(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the worker operation authority block, then str claim, bool claimed_success
Result<std::string> encode_payload(const SelfReportMessage& message) {
  ByteWriter writer;
  write_worker_operation_authority(writer, message.authority);
  writer.str(message.claim);
  writer.boolean(message.claimed_success);
  return writer.take();
}

Result<SelfReportMessage> decode_self_report(std::string_view payload) {
  ByteReader reader(payload);
  SelfReportMessage message;
  AF_TRY_ASSIGN(message.authority, read_worker_operation_authority(reader));
  AF_TRY_ASSIGN(message.claim, reader.str());
  AF_TRY_ASSIGN(message.claimed_success, reader.boolean());
  AF_TRY(reader.require_exhausted());
  return message;
}

// ---------------------------------------------------------------------------
// Query plane
// ---------------------------------------------------------------------------

// the controller session block, then identity population
Result<std::string> encode_payload(const QueryPopulationMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  writer.identity(message.population);
  return writer.take();
}

Result<QueryPopulationMessage> decode_query_population(std::string_view payload) {
  ByteReader reader(payload);
  QueryPopulationMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY_ASSIGN(message.population,
                reader.identity<PopulationIdTag>("query_population.population"));
  AF_TRY(reader.require_exhausted());
  return message;
}

// PopulationRecord, then u32 committed_selection_generation,
// bool retention_committed, u64 candidate_count,
// u32 closure_blockers count + str
Result<std::string> encode_payload(const PopulationDetailMessage& message) {
  ByteWriter writer;
  write_population_record(writer, message.population);
  writer.u32(message.committed_selection_generation);
  writer.boolean(message.retention_committed);
  writer.u64(message.candidate_count);
  write_string_list(writer, message.closure_blockers);
  return writer.take();
}

Result<PopulationDetailMessage> decode_population_detail(std::string_view payload) {
  ByteReader reader(payload);
  PopulationDetailMessage message;
  AF_TRY_ASSIGN(message.population, read_population_record(reader));
  AF_TRY_ASSIGN(message.committed_selection_generation, reader.u32());
  AF_TRY_ASSIGN(message.retention_committed, reader.boolean());
  AF_TRY_ASSIGN(message.candidate_count, reader.u64());
  AF_TRY_ASSIGN(message.closure_blockers,
                read_string_list(reader, "population_detail.closure_blockers"));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller session block, then identity candidate
Result<std::string> encode_payload(const QueryCandidateMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  writer.identity(message.candidate);
  return writer.take();
}

Result<QueryCandidateMessage> decode_query_candidate(std::string_view payload) {
  ByteReader reader(payload);
  QueryCandidateMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY_ASSIGN(message.candidate,
                reader.identity<CandidateIdTag>("query_candidate.candidate"));
  AF_TRY(reader.require_exhausted());
  return message;
}

// CandidateRecord, then u32 evaluation count + EvaluationRecord
Result<std::string> encode_payload(const CandidateDetailMessage& message) {
  ByteWriter writer;
  write_candidate_record(writer, message.candidate);
  writer.u32(static_cast<std::uint32_t>(message.evaluations.size()));
  for (const EvaluationRecord& record : message.evaluations) {
    write_evaluation_record(writer, record);
  }
  return writer.take();
}

Result<CandidateDetailMessage> decode_candidate_detail(std::string_view payload) {
  ByteReader reader(payload);
  CandidateDetailMessage message;
  AF_TRY_ASSIGN(message.candidate, read_candidate_record(reader));
  std::uint32_t evaluation_count = 0;
  AF_TRY_ASSIGN(evaluation_count,
                read_count(reader, "candidate_detail.evaluations", kMaxProtocolElements,
                           kMinEvaluationRecordBytes));
  message.evaluations.reserve(evaluation_count);
  for (std::uint32_t evaluation_index = 0; evaluation_index < evaluation_count;
       ++evaluation_index) {
    EvaluationRecord record;
    AF_TRY_ASSIGN(record, read_evaluation_record(reader));
    message.evaluations.push_back(std::move(record));
  }
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller session block, then identity root
Result<std::string> encode_payload(const QueryLineageMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  writer.identity(message.root);
  return writer.take();
}

Result<QueryLineageMessage> decode_query_lineage(std::string_view payload) {
  ByteReader reader(payload);
  QueryLineageMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY_ASSIGN(message.root, reader.identity<CandidateIdTag>("query_lineage.root"));
  AF_TRY(reader.require_exhausted());
  return message;
}

// u32 node count + LineageNode
Result<std::string> encode_payload(const LineageDetailMessage& message) {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(message.nodes.size()));
  for (const LineageNode& node : message.nodes) {
    write_lineage_node(writer, node);
  }
  return writer.take();
}

Result<LineageDetailMessage> decode_lineage_detail(std::string_view payload) {
  ByteReader reader(payload);
  LineageDetailMessage message;
  std::uint32_t node_count = 0;
  AF_TRY_ASSIGN(node_count,
                read_count(reader, "lineage_detail.nodes", kMaxProtocolElements,
                           kMinLineageNodeBytes));
  message.nodes.reserve(node_count);
  for (std::uint32_t node_index = 0; node_index < node_count; ++node_index) {
    LineageNode node;
    AF_TRY_ASSIGN(node, read_lineage_node(reader));
    message.nodes.push_back(std::move(node));
  }
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller session block
Result<std::string> encode_payload(const QueryStatisticsMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  return writer.take();
}

Result<QueryStatisticsMessage> decode_query_statistics(std::string_view payload) {
  ByteReader reader(payload);
  QueryStatisticsMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// FoundryStatistics, then u64 revision, epoch epoch, identity run
Result<std::string> encode_payload(const StatisticsDetailMessage& message) {
  ByteWriter writer;
  write_statistics(writer, message.statistics);
  writer.u64(message.revision);
  writer.epoch(message.epoch);
  writer.identity(message.run);
  return writer.take();
}

Result<StatisticsDetailMessage> decode_statistics_detail(std::string_view payload) {
  ByteReader reader(payload);
  StatisticsDetailMessage message;
  AF_TRY_ASSIGN(message.statistics, read_statistics(reader));
  AF_TRY_ASSIGN(message.revision, reader.u64());
  AF_TRY_ASSIGN(message.epoch, reader.epoch());
  AF_TRY_ASSIGN(message.run,
                reader.identity<FoundryRunIdTag>("statistics_detail.run"));
  AF_TRY(reader.require_exhausted());
  return message;
}

// the controller session block
Result<std::string> encode_payload(const QueryAuditMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  return writer.take();
}

Result<QueryAuditMessage> decode_query_audit(std::string_view payload) {
  ByteReader reader(payload);
  QueryAuditMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// u32 violation count + str
Result<std::string> encode_payload(const AuditDetailMessage& message) {
  ByteWriter writer;
  write_string_list(writer, message.violations);
  return writer.take();
}

Result<AuditDetailMessage> decode_audit_detail(std::string_view payload) {
  ByteReader reader(payload);
  AuditDetailMessage message;
  AF_TRY_ASSIGN(message.violations, read_string_list(reader, "audit_detail.violations"));
  AF_TRY(reader.require_exhausted());
  return message;
}

// SelectionDecision, then bool committed
Result<std::string> encode_payload(const SelectionDecisionMessage& message) {
  ByteWriter writer;
  write_selection_decision(writer, message.decision);
  writer.boolean(message.committed);
  return writer.take();
}

Result<SelectionDecisionMessage> decode_selection_decision(std::string_view payload) {
  ByteReader reader(payload);
  SelectionDecisionMessage message;
  AF_TRY_ASSIGN(message.decision, read_selection_decision(reader));
  AF_TRY_ASSIGN(message.committed, reader.boolean());
  AF_TRY(reader.require_exhausted());
  return message;
}

// RetentionDecision
Result<std::string> encode_payload(const RetentionDecisionMessage& message) {
  ByteWriter writer;
  write_retention_decision(writer, message.decision);
  return writer.take();
}

Result<RetentionDecisionMessage> decode_retention_decision(std::string_view payload) {
  ByteReader reader(payload);
  RetentionDecisionMessage message;
  AF_TRY_ASSIGN(message.decision, read_retention_decision(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// PromotionRequest, then PromotionReceipt
Result<std::string> encode_payload(const PromotionRequestMessage& message) {
  ByteWriter writer;
  write_promotion_request(writer, message.request);
  write_promotion_receipt(writer, message.receipt);
  return writer.take();
}

Result<PromotionRequestMessage> decode_promotion_request(std::string_view payload) {
  ByteReader reader(payload);
  PromotionRequestMessage message;
  AF_TRY_ASSIGN(message.request, read_promotion_request(reader));
  AF_TRY_ASSIGN(message.receipt, read_promotion_receipt(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// ---------------------------------------------------------------------------
// Lifecycle and error responses
// ---------------------------------------------------------------------------

// the controller session block
Result<std::string> encode_payload(const ShutdownCoordinatorMessage& message) {
  ByteWriter writer;
  write_controller_session_authority(writer, message.session);
  return writer.take();
}

Result<ShutdownCoordinatorMessage> decode_shutdown_coordinator(std::string_view payload) {
  ByteReader reader(payload);
  ShutdownCoordinatorMessage message;
  AF_TRY_ASSIGN(message.session, read_controller_session_authority(reader));
  AF_TRY(reader.require_exhausted());
  return message;
}

// bool ok, u16 code, str detail. The code is the raw ErrorCode value: the enum
// is an open reporting surface, so an unknown code is carried rather than
// rejected, and consumers compare it against the codes they understand.
Result<std::string> encode_payload(const ControlAckMessage& message) {
  ByteWriter writer;
  writer.boolean(message.ok);
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.str(message.detail);
  return writer.take();
}

Result<ControlAckMessage> decode_control_ack(std::string_view payload) {
  ByteReader reader(payload);
  ControlAckMessage message;
  AF_TRY_ASSIGN(message.ok, reader.boolean());
  std::uint16_t code = 0;
  AF_TRY_ASSIGN(code, reader.u16());
  message.code = static_cast<ErrorCode>(code);
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// u16 code (raw ErrorCode value), str detail, u16 in_reply_to (raw MessageType
// value; MessageTypeUnknown is a legal value here)
Result<std::string> encode_payload(const ErrorResponseMessage& message) {
  ByteWriter writer;
  writer.u16(static_cast<std::uint16_t>(message.code));
  writer.str(message.detail);
  writer.u16(static_cast<std::uint16_t>(message.in_reply_to));
  return writer.take();
}

Result<ErrorResponseMessage> decode_error_response(std::string_view payload) {
  ByteReader reader(payload);
  ErrorResponseMessage message;
  std::uint16_t code = 0;
  AF_TRY_ASSIGN(code, reader.u16());
  message.code = static_cast<ErrorCode>(code);
  AF_TRY_ASSIGN(message.detail, reader.str());
  std::uint16_t in_reply_to = 0;
  AF_TRY_ASSIGN(in_reply_to, reader.u16());
  message.in_reply_to = static_cast<MessageType>(in_reply_to);
  AF_TRY(reader.require_exhausted());
  return message;
}

// identity population, generation generation, u8 state, str detail
Result<std::string> encode_payload(const RevalidationAppliedMessage& message) {
  ByteWriter writer;
  writer.identity(message.population);
  writer.generation(message.generation);
  writer.u8(static_cast<std::uint8_t>(message.state));
  writer.str(message.detail);
  return writer.take();
}

Result<RevalidationAppliedMessage> decode_revalidation_applied(std::string_view payload) {
  ByteReader reader(payload);
  RevalidationAppliedMessage message;
  AF_TRY_ASSIGN(
      message.population,
      reader.identity<PopulationIdTag>("revalidation_applied.population"));
  AF_TRY_ASSIGN(message.generation, reader.generation<PopulationIdTag>());
  std::uint8_t state = 0;
  AF_TRY_ASSIGN(state, reader.u8());
  if (static_cast<std::size_t>(state) >= kPopulationStateCount) {
    return Status(ErrorCode::MalformedEncoding,
                  "population state " + std::to_string(state) + " is not a declared state");
  }
  message.state = static_cast<PopulationState>(state);
  AF_TRY_ASSIGN(message.detail, reader.str());
  AF_TRY(reader.require_exhausted());
  return message;
}

// ---------------------------------------------------------------------------
// Shared sub-codecs
// ---------------------------------------------------------------------------

Result<std::string> encode_evaluation_record(const EvaluationRecord& record) {
  ByteWriter writer;
  write_evaluation_record(writer, record);
  return writer.take();
}

Result<EvaluationRecord> decode_evaluation_record(std::string_view payload) {
  ByteReader reader(payload);
  EvaluationRecord record;
  AF_TRY_ASSIGN(record, read_evaluation_record(reader));
  AF_TRY(reader.require_exhausted());
  return record;
}

Result<std::string> encode_selection_decision(const SelectionDecision& decision) {
  ByteWriter writer;
  write_selection_decision(writer, decision);
  return writer.take();
}

// Standalone form: the buffer must hold exactly one decision. The message
// decoder above uses the reader form directly, so a decision that is part of a
// larger message is not required to exhaust its enclosing payload.
Result<SelectionDecision> decode_selection_decision_value(std::string_view payload) {
  ByteReader reader(payload);
  SelectionDecision decision;
  AF_TRY_ASSIGN(decision, read_selection_decision(reader));
  AF_TRY(reader.require_exhausted());
  return decision;
}

Result<std::string> encode_retention_decision(const RetentionDecision& decision) {
  ByteWriter writer;
  write_retention_decision(writer, decision);
  return writer.take();
}

Result<RetentionDecision> decode_retention_decision_value(std::string_view payload) {
  ByteReader reader(payload);
  RetentionDecision decision;
  AF_TRY_ASSIGN(decision, read_retention_decision(reader));
  AF_TRY(reader.require_exhausted());
  return decision;
}

Result<std::string> encode_promotion_request_value(const PromotionRequest& request) {
  ByteWriter writer;
  write_promotion_request(writer, request);
  return writer.take();
}

Result<PromotionRequest> decode_promotion_request_value(std::string_view payload) {
  ByteReader reader(payload);
  PromotionRequest request;
  AF_TRY_ASSIGN(request, read_promotion_request(reader));
  AF_TRY(reader.require_exhausted());
  return request;
}

Result<std::string> encode_promotion_receipt(const PromotionReceipt& receipt) {
  ByteWriter writer;
  write_promotion_receipt(writer, receipt);
  return writer.take();
}

Result<PromotionReceipt> decode_promotion_receipt(std::string_view payload) {
  ByteReader reader(payload);
  PromotionReceipt receipt;
  AF_TRY_ASSIGN(receipt, read_promotion_receipt(reader));
  AF_TRY(reader.require_exhausted());
  return receipt;
}

Result<std::string> encode_worker_session_authority(const WorkerSessionAuthority& authority) {
  ByteWriter writer;
  write_worker_session_authority(writer, authority);
  return writer.take();
}

// The authority decoders take the caller's reader: an authority block is always
// followed by more message fields, so they consume exactly their own bytes and
// leave the reader positioned at the next field.
Result<WorkerSessionAuthority> decode_worker_session_authority(ByteReader& reader) {
  return read_worker_session_authority(reader);
}

Result<std::string> encode_worker_operation_authority(const WorkerOperationAuthority& authority) {
  ByteWriter writer;
  write_worker_operation_authority(writer, authority);
  return writer.take();
}

Result<WorkerOperationAuthority> decode_worker_operation_authority(ByteReader& reader) {
  return read_worker_operation_authority(reader);
}

Result<std::string> encode_controller_session_authority(
    const ControllerSessionAuthority& authority) {
  ByteWriter writer;
  write_controller_session_authority(writer, authority);
  return writer.take();
}

Result<ControllerSessionAuthority> decode_controller_session_authority(ByteReader& reader) {
  return read_controller_session_authority(reader);
}

Result<std::string> encode_controller_operation_authority(
    const ControllerOperationAuthority& authority) {
  ByteWriter writer;
  write_controller_operation_authority(writer, authority);
  return writer.take();
}

Result<ControllerOperationAuthority> decode_controller_operation_authority(ByteReader& reader) {
  return read_controller_operation_authority(reader);
}

}  // namespace autonomous_foundry



