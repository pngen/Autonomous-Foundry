#include "autonomous_foundry/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/attempt.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/lineage.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/promotion.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/version.hpp"
#include "autonomous_foundry/worker.hpp"
#include "autonomous_foundry/workspace.hpp"

// Snapshot codec.
//
// Layout (little endian throughout):
//
//   offset 0   magic "AFSN"
//   offset 4   u32 format version
//   offset 8   u64 payload length
//   offset 16  u32 payload CRC-32C
//   offset 20  u32 reserved (written as zero, rejected when non-zero)
//   offset 24  payload, exactly payload_length bytes
//
// Payload:
//
//   u32 record_count, then record_count repetitions of
//       { u8 record_type, u32 body_length, body }
//
// Record kinds, in the fixed serialization order:
//
//   1  identity header
//   2  statistics
//   3  task
//   4  policy
//   5  population
//   6  candidate
//   7  worker
//   8  attempt
//   9  assignment
//   10 evaluation
//   11 selection decision
//   12 retention decision
//   13 promotion request
//   14 lineage node
//   15 budget ledger
//
// Every map is emitted in ascending identity order so that identical state
// always produces a byte identical image. Every identity, every generation and
// every enum is range checked on the way in, no count is trusted, and every
// nested collection count is required to fit inside the bytes that remain.
//
// One deliberate detail: BudgetLedger::restore() accepts only the counters, so
// the ledger record carries the nine BudgetLimits fields ahead of the nine
// (reserved, consumed, released) counter triples. Both halves are encoded in
// declaration order and both halves are restored on read.

namespace autonomous_foundry {
namespace {

// ---------------------------------------------------------------------------
// Scalar primitives
// ---------------------------------------------------------------------------

void append_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  out.push_back(static_cast<char>((value >> 16) & 0xFFu));
  out.push_back(static_cast<char>((value >> 24) & 0xFFu));
}

void append_u64(std::string& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

/// Rewrite a previously reserved little endian u32 slot.
void patch_u32(std::string& out, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    out[offset + index] = static_cast<char>((value >> (8 * index)) & 0xFFu);
  }
}

class Writer {
 public:
  void u8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }

  void boolean(bool value) { u8(value ? 1u : 0u); }

  void u32(std::uint32_t value) { append_u32(out_, value); }

  void u64(std::uint64_t value) { append_u64(out_, value); }

  void f64(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "a snapshot double must be 64 bits");
    const auto* source = reinterpret_cast<const unsigned char*>(&value);
    for (unsigned index = 0; index < sizeof(bits); ++index) {
      bits |= static_cast<std::uint64_t>(source[index]) << (8 * index);
    }
    append_u64(out_, bits);
  }

  void str(std::string_view value) {
    append_u32(out_, static_cast<std::uint32_t>(value.size()));
    out_.append(value.data(), value.size());
  }

  template <typename Id>
  void identity(Id id) {
    append_u64(out_, id.raw());
  }

  template <typename Tag>
  void generation(Generation<Tag> value) {
    append_u32(out_, value.value());
  }

  void epoch(CoordinatorEpoch value) { append_u64(out_, value.value()); }

  template <std::size_t N>
  void counters(const std::array<std::uint32_t, N>& values) {
    for (const std::uint32_t value : values) {
      append_u32(out_, value);
    }
  }

  void string_list(const std::vector<std::string>& values) {
    append_u32(out_, static_cast<std::uint32_t>(values.size()));
    for (const std::string& value : values) {
      str(value);
    }
  }

  void id_list(const std::vector<CandidateId>& values) {
    append_u32(out_, static_cast<std::uint32_t>(values.size()));
    for (const CandidateId value : values) {
      identity(value);
    }
  }

  /// Reserve a u32 slot so that a record body can be framed once its content
  /// has been produced.
  [[nodiscard]] std::size_t reserve_u32() {
    const std::size_t offset = out_.size();
    append_u32(out_, 0u);
    return offset;
  }

  [[nodiscard]] std::string take() noexcept { return std::move(out_); }

 private:
  std::string out_;
};

class Reader {
 public:
  explicit Reader(std::string_view data) noexcept : data_(data) {}

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] const std::string& field() const noexcept { return field_; }

  void set_field(std::string_view field) { field_.assign(field); }

  [[nodiscard]] Result<std::uint8_t> u8() {
    if (remaining() < 1) {
      return short_buffer();
    }
    const auto value = static_cast<std::uint8_t>(static_cast<unsigned char>(data_[offset_]));
    offset_ += 1;
    return value;
  }

  [[nodiscard]] Result<std::uint32_t> u32() {
    if (remaining() < 4) {
      return short_buffer();
    }
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data_[offset_ + index]))
               << (8 * index);
    }
    offset_ += 4;
    return value;
  }

  [[nodiscard]] Result<std::uint64_t> u64() {
    if (remaining() < 8) {
      return short_buffer();
    }
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data_[offset_ + index]))
               << (8 * index);
    }
    offset_ += 8;
    return value;
  }

  [[nodiscard]] Result<double> f64() {
    std::uint64_t bits{}; AF_TRY_ASSIGN(bits, u64());
    double value = 0.0;
    auto* target = reinterpret_cast<unsigned char*>(&value);
    for (unsigned index = 0; index < sizeof(value); ++index) {
      target[index] = static_cast<unsigned char>((bits >> (8 * index)) & 0xFFu);
    }
    return value;
  }

  [[nodiscard]] Result<bool> boolean() {
    std::uint8_t value{}; AF_TRY_ASSIGN(value, u8());
    if (value > 1u) {
      return Status(ErrorCode::MalformedEncoding,
                    "field '" + field_ + "' holds " + std::to_string(value) +
                        " but only 0 or 1 is a valid boolean");
    }
    return value != 0u;
  }

  /// Take exactly count bytes that the caller has already bounded. Used for a
  /// record body whose length was read as a separate header field.
  [[nodiscard]] Result<std::string> take_bytes(std::size_t count, std::string_view name) {
    if (remaining() < count) {
      return Status(ErrorCode::PersistenceTruncated,
                    "field '" + std::string(name) + "' declares " + std::to_string(count) +
                        " bytes but only " + std::to_string(remaining()) + " remain");
    }
    std::string value(data_.substr(offset_, count));
    offset_ += count;
    return value;
  }
  /// Read a length prefixed byte string.
  ///
  /// The declared length is checked against the snapshot string ceiling before
  /// anything is materialised, and against the bytes that remain before
  /// anything is copied, so a hostile length field can never drive an
  /// allocation larger than the payload that is actually present.
  [[nodiscard]] Result<std::string> str() {
    std::uint32_t length{}; AF_TRY_ASSIGN(length, u32());
    if (static_cast<std::uint64_t>(length) > kMaxSnapshotStringBytes) {
      return Status(ErrorCode::LengthOutOfRange,
                    "field '" + field_ + "' declares " + std::to_string(length) +
                        " bytes, beyond the " + std::to_string(kMaxSnapshotStringBytes) +
                        " byte snapshot string ceiling");
    }
    if (remaining() < length) {
      return Status(ErrorCode::MalformedEncoding,
                    "field '" + field_ + "' declares " + std::to_string(length) +
                        " bytes but only " + std::to_string(remaining()) + " remain");
    }
    std::string value(data_.substr(offset_, static_cast<std::size_t>(length)));
    offset_ += static_cast<std::size_t>(length);
    return value;
  }

  template <typename Id>
  [[nodiscard]] Result<Id> identity(std::string_view name) {
    std::uint64_t raw{}; AF_TRY_ASSIGN(raw, u64());
    Id value{};
    AF_TRY_ASSIGN(value, strong_id_from_raw_checked<typename Id::tag_type>(raw, name));
    return value;
  }

  /// Only the reference fields documented as optional accept the null
  /// identity; every other identity must carry its declared domain.
  template <typename Id>
  [[nodiscard]] Result<Id> optional_identity(std::string_view name) {
    std::uint64_t raw{}; AF_TRY_ASSIGN(raw, u64());
    if (raw == 0) {
      return Id::null();
    }
    Id value{};
    AF_TRY_ASSIGN(value, strong_id_from_raw_checked<typename Id::tag_type>(raw, name));
    return value;
  }

  template <typename Tag>
  [[nodiscard]] Result<Generation<Tag>> generation() {
    std::uint32_t value{}; AF_TRY_ASSIGN(value, u32());
    return Generation<Tag>::from_value(value);
  }

  [[nodiscard]] Result<CoordinatorEpoch> epoch() {
    std::uint64_t value{}; AF_TRY_ASSIGN(value, u64());
    return CoordinatorEpoch::from_value(value);
  }

  /// Read an enum discriminant and reject any value outside its domain.
  [[nodiscard]] Result<std::uint8_t> enum_value(std::string_view name,
                                                std::uint8_t max_value) {
    std::uint8_t value{}; AF_TRY_ASSIGN(value, u8());
    if (value > max_value) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "field '" + std::string(name) + "' holds enum value " +
                        std::to_string(value) + " but the largest valid value is " +
                        std::to_string(max_value));
    }
    return value;
  }

  /// Read a collection count.
  ///
  /// Two independent ceilings apply before a single element is created: the
  /// declared count must fit inside the bytes that remain at the element's
  /// minimum encoded size, and it must be below the domain ceiling whenever the
  /// domain states one.
  [[nodiscard]] Result<std::uint32_t> count(std::string_view name,
                                           std::uint64_t min_element_bytes,
                                           std::uint32_t max_count = 0) {
    std::uint32_t value{}; AF_TRY_ASSIGN(value, u32());
    if (max_count != 0 && value > max_count) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "collection '" + std::string(name) + "' holds " +
                        std::to_string(value) + " entries, beyond the " +
                        std::to_string(max_count) + " entry ceiling");
    }
    if (min_element_bytes == 0 ||
        static_cast<std::uint64_t>(value) >
            static_cast<std::uint64_t>(remaining()) / min_element_bytes) {
      return Status(ErrorCode::MalformedEncoding,
                    "collection '" + std::string(name) + "' declares " +
                        std::to_string(value) + " entries but only " +
                        std::to_string(remaining()) + " bytes remain");
    }
    return value;
  }

  template <std::size_t N>
  [[nodiscard]] Result<std::array<std::uint32_t, N>> counters() {
    std::array<std::uint32_t, N> values{};
    for (std::size_t index = 0; index < N; ++index) {
      std::uint32_t value{}; AF_TRY_ASSIGN(value, u32());
      values[index] = value;
    }
    return values;
  }

  /// Fail unless every byte of this reader's slice has been consumed. Catches
  /// a body that carries more than its declared content.
  [[nodiscard]] Status require_exhausted() const {
    if (remaining() != 0) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "field '" + field_ + "' is followed by " +
                        std::to_string(remaining()) + " unread bytes");
    }
    return Status();
  }

 private:
  [[nodiscard]] Status short_buffer() const {
    return Status(ErrorCode::MalformedEncoding,
                  "field '" + field_ + "' ends after " + std::to_string(offset_) + " of " +
                      std::to_string(data_.size()) + " encoded bytes");
  }

  std::string_view data_;
  std::size_t offset_{0};
  std::string field_{"snapshot"};
};

// ---------------------------------------------------------------------------
// Bounds used by the decoders
// ---------------------------------------------------------------------------

// Record kinds. These values are part of the on disk contract and are never
// renumbered; the fixed order below is the serialization order.
constexpr std::uint8_t kRecordIdentityHeader = 1;
constexpr std::uint8_t kRecordStatistics = 2;
constexpr std::uint8_t kRecordTask = 3;
constexpr std::uint8_t kRecordPolicy = 4;
constexpr std::uint8_t kRecordPopulation = 5;
constexpr std::uint8_t kRecordCandidate = 6;
constexpr std::uint8_t kRecordWorker = 7;
constexpr std::uint8_t kRecordAttempt = 8;
constexpr std::uint8_t kRecordAssignment = 9;
constexpr std::uint8_t kRecordEvaluation = 10;
constexpr std::uint8_t kRecordSelection = 11;
constexpr std::uint8_t kRecordRetention = 12;
constexpr std::uint8_t kRecordPromotion = 13;
constexpr std::uint8_t kRecordLineageNode = 14;
constexpr std::uint8_t kRecordLedger = 15;

// Smallest number of bytes one element of each nested collection can occupy.
// Every element below carries at least one identity, one generation or one
// length prefix, so these are honest lower bounds and no legal document is
// rejected by them.
constexpr std::uint64_t kMinInputFileBytes = 12u;
constexpr std::uint64_t kMinRequirementBytes = 26u;
constexpr std::uint64_t kMinRankingFactorBytes = 14u;
constexpr std::uint64_t kMinArtifactRefBytes = 16u;
constexpr std::uint64_t kMinRankingValueBytes = 35u;
constexpr std::uint64_t kMinRankingEntryBytes = 40u;
constexpr std::uint64_t kMinExclusionEntryBytes = 18u;
constexpr std::uint64_t kMinRetentionEntryBytes = 25u;
constexpr std::uint64_t kMinEvidenceSummaryBytes = 31u;
constexpr std::uint64_t kMinIdentityBytes = 8u;

// Ceilings stated by the snapshot contract.
constexpr std::uint32_t kMaxClosureBlockers = 64;
constexpr std::uint32_t kMaxOutstandingRequirements = 64;

// AF_TRY_ASSIGN binds an existing target, so a fresh local needs its own
// spelling. These two expand to a single statement, declare the value in the
// enclosing scope, and propagate the failure with the original status.
#define AF_READ(type, name, expr)              \
  do {                                         \
    auto af_read_probe__ = (expr);             \
    if (!af_read_probe__.ok()) {               \
      return af_read_probe__.status();         \
    }                                          \
    type name = std::move(af_read_probe__).value(); \
  } while (false)

#define AF_READ_STR(name, expr)                \
  do {                                         \
    auto af_read_probe__ = (expr);             \
    if (!af_read_probe__.ok()) {               \
      return af_read_probe__.status();         \
    }                                          \
    std::string name = std::move(af_read_probe__).value(); \
  } while (false)

// ---------------------------------------------------------------------------
// Identity order without hashing
// ---------------------------------------------------------------------------

template <typename Id>
struct IdentityLess {
  [[nodiscard]] bool operator()(Id a, Id b) const noexcept { return a.raw() < b.raw(); }
};

template <typename Map>
[[nodiscard]] auto ascending_keys(const Map& map) {
  using Id = typename Map::key_type;
  std::vector<Id> keys;
  keys.reserve(map.size());
  for (const auto& entry : map) {
    keys.push_back(entry.first);
  }
  std::sort(keys.begin(), keys.end(), IdentityLess<Id>{});
  return keys;
}

/// Frame one record: kind, body length, body.
template <typename EncodeBody>
[[nodiscard]] std::string frame_record(std::uint8_t record_type, EncodeBody&& encode_body) {
  Writer writer;
  writer.u8(record_type);
  const std::size_t length_offset = writer.reserve_u32();
  encode_body(writer);
  std::string out = writer.take();
  patch_u32(out, length_offset, static_cast<std::uint32_t>(out.size() - length_offset - 4));
  return out;
}

/// Append every record of one map, in ascending identity order.
template <typename Map, typename WriteBody>
void append_map_records(std::string& payload, const Map& map, std::uint8_t record_type,
                        WriteBody&& write_body) {
  using Id = typename Map::key_type;
  for (const Id key : ascending_keys(map)) {
    const auto found = map.find(key);
    if (found == map.end()) {
      continue;
    }
    payload.append(
        frame_record(record_type, [&key, &found, &write_body](Writer& writer) {
          write_body(key, writer, found->second);
        }));
  }
}

// ---------------------------------------------------------------------------
// Record body encoders. Every field of every record is written in the order
// the field is declared in.
// ---------------------------------------------------------------------------

void write_budget_limits(Writer& w, const BudgetLimits& limits) {
  w.u32(limits.max_candidate_attempts);
  w.u32(limits.max_worker_assignments);
  w.u32(limits.max_evaluation_attempts);
  w.u32(limits.max_retained_candidates);
  w.u32(limits.max_retries);
  w.u32(limits.max_candidates);
  w.u32(limits.max_workers);
  w.u32(limits.max_active_attempts);
  w.u32(limits.max_evaluation_concurrency);
}

void write_input_file(Writer& w, const InputFile& file) {
  w.str(file.name);
  w.str(file.content);
  w.str(file.content_digest);
}

void write_requirement(Writer& w, const EvaluationRequirement& requirement) {
  w.u8(static_cast<std::uint8_t>(requirement.requirement_class));
  w.str(requirement.evaluator_key);
  w.str(requirement.description);
  w.boolean(requirement.has_score_floor);
  w.f64(requirement.score_floor);
  w.f64(requirement.ranking_weight);
}

void write_ranking_factor(Writer& w, const RankingFactor& factor) {
  w.u8(static_cast<std::uint8_t>(factor.kind));
  w.u8(static_cast<std::uint8_t>(factor.direction));
  w.f64(factor.weight);
  w.str(factor.key);
}

void write_selection_policy(Writer& w, const SelectionPolicy& selection) {
  w.u32(static_cast<std::uint32_t>(selection.factors.size()));
  for (const RankingFactor& factor : selection.factors) {
    write_ranking_factor(w, factor);
  }
  w.boolean(selection.require_complete_mandatory);
}

void write_retention_policy(Writer& w, const RetentionPolicy& retention) {
  w.u32(retention.retain_top_k);
  w.u32(retention.max_retained);
  w.u32(retention.max_per_lineage);
  w.boolean(retention.retain_selected);
}

void write_artifact_ref(Writer& w, const ArtifactRef& artifact) {
  w.str(artifact.name);
  w.u64(artifact.size_bytes);
  w.str(artifact.content_digest);
}

void write_task(TaskId /*key*/, Writer& w, const TaskSpec& task) {
  w.identity(task.id);
  w.generation(task.generation);
  w.str(task.name);
  w.str(task.objective);
  w.u32(static_cast<std::uint32_t>(task.inputs.size()));
  for (const InputFile& file : task.inputs) {
    write_input_file(w, file);
  }
  w.string_list(task.required_outputs);
  w.u32(static_cast<std::uint32_t>(task.requirements.size()));
  for (const EvaluationRequirement& requirement : task.requirements) {
    write_requirement(w, requirement);
  }
  w.string_list(task.hard_constraints);
  w.string_list(task.closure_criteria);
  w.identity(task.policy);
  w.generation(task.policy_generation);
  write_budget_limits(w, task.budgets);
  w.str(task.content_digest);
}

void write_policy(PolicyId /*key*/, Writer& w, const FoundryPolicy& policy) {
  w.identity(policy.id);
  w.generation(policy.generation);
  w.str(policy.name);
  write_selection_policy(w, policy.selection);
  write_retention_policy(w, policy.retention);
  write_budget_limits(w, policy.budgets);
  w.u32(policy.max_generation_depth);
  w.boolean(policy.carry_forward_elite);
  w.str(policy.content_digest);
}

void write_population(PopulationId /*key*/, Writer& w, const PopulationRecord& population) {
  w.identity(population.id);
  w.generation(population.generation);
  w.identity(population.run);
  w.str(population.name);
  w.identity(population.task);
  w.generation(population.task_generation);
  w.identity(population.policy);
  w.generation(population.policy_generation);
  w.u32(population.candidate_budget);
  w.u32(population.worker_budget);
  w.u32(population.population_index);
  w.identity(population.predecessor);
  w.identity(population.seed_candidate);
  w.generation(population.seed_candidate_generation);
  w.identity(population.seed_lineage);
  w.u8(static_cast<std::uint8_t>(population.state));
  w.epoch(population.created_epoch);
  w.epoch(population.state_epoch);
  w.generation(population.committed_selection);
  w.boolean(population.retention_committed);
  w.generation(population.retention_selection_generation);
  w.boolean(population.require_promotion_request);
  w.id_list(population.candidates);
  w.u64(population.promotion_requests);
  w.identity(population.committed_promotion_request);
  w.str(population.status_detail);
  w.string_list(population.closure_blockers);
}

void write_candidate(CandidateId /*key*/, Writer& w, const CandidateRecord& candidate) {
  w.identity(candidate.id);
  w.generation(candidate.generation);
  w.identity(candidate.population);
  w.generation(candidate.population_generation);
  w.identity(candidate.task);
  w.generation(candidate.task_generation);
  w.identity(candidate.lineage);
  w.id_list(candidate.parents);
  w.u32(candidate.depth);
  w.identity(candidate.producer_worker);
  w.identity(candidate.producer_boot);
  w.identity(candidate.attempt);
  w.generation(candidate.attempt_generation);
  w.identity(candidate.assignment);
  w.epoch(candidate.created_epoch);
  w.epoch(candidate.state_changed_epoch);
  w.u8(static_cast<std::uint8_t>(candidate.state));
  w.u32(static_cast<std::uint32_t>(candidate.artifacts.size()));
  for (const ArtifactRef& artifact : candidate.artifacts) {
    write_artifact_ref(w, artifact);
  }
  w.str(candidate.artifact_set_digest);
  w.u32(candidate.attempt_count);
  w.generation(candidate.evidence_generation);
  w.generation(candidate.evaluation_generation);
  w.generation(candidate.selection_generation);
  w.boolean(candidate.selected);
  w.boolean(candidate.retained);
  w.boolean(candidate.promotion_eligible);
  w.boolean(candidate.promotion_requested);
  w.str(candidate.diversity_key);
  w.str(candidate.failure_reason);
  w.str(candidate.superseded_by_reason);
}

void write_worker(WorkerId /*key*/, Writer& w, const WorkerRecord& worker) {
  w.identity(worker.id);
  w.identity(worker.boot);
  w.str(worker.label);
  w.u32(worker.process_id);
  w.u8(static_cast<std::uint8_t>(worker.state));
  w.epoch(worker.registered_epoch);
  w.epoch(worker.state_epoch);
  w.identity(worker.session);
  w.generation(worker.session_generation);
  w.str(worker.capability);
  w.u64(worker.assignments_accepted);
  w.u64(worker.assignments_completed);
  w.u64(worker.assignments_failed);
  w.u64(worker.stale_messages_rejected);
  w.identity(worker.active_attempt);
  w.generation(worker.active_attempt_generation);
  w.identity(worker.active_assignment);
  w.str(worker.last_diagnostic);
}

void write_attempt(AttemptId /*key*/, Writer& w, const AttemptRecord& attempt) {
  w.identity(attempt.id);
  w.generation(attempt.generation);
  w.identity(attempt.population);
  w.generation(attempt.population_generation);
  w.identity(attempt.task);
  w.generation(attempt.task_generation);
  w.identity(attempt.candidate);
  w.generation(attempt.candidate_generation);
  w.identity(attempt.worker);
  w.identity(attempt.worker_boot);
  w.identity(attempt.session);
  w.identity(attempt.assignment);
  w.epoch(attempt.authorized_epoch);
  w.epoch(attempt.dispatched_epoch);
  w.epoch(attempt.terminal_epoch);
  w.u8(static_cast<std::uint8_t>(attempt.state));
  w.u32(attempt.retry_index);
  w.identity(attempt.previous_attempt);
  w.str(attempt.failure_detail);
  w.boolean(attempt.holds_reservation);
}

void write_assignment(AssignmentId /*key*/, Writer& w, const AssignmentRecord& assignment) {
  w.identity(assignment.id);
  w.generation(assignment.generation);
  w.identity(assignment.attempt);
  w.generation(assignment.attempt_generation);
  w.identity(assignment.worker);
  w.identity(assignment.worker_boot);
  w.identity(assignment.session);
  w.generation(assignment.session_generation);
  w.epoch(assignment.issued_epoch);
  w.boolean(assignment.acknowledged);
  w.epoch(assignment.acknowledged_epoch);
  w.boolean(assignment.revoked);
  w.epoch(assignment.revoked_epoch);
}

void write_evaluation(EvaluationId /*key*/, Writer& w, const EvaluationRecord& evaluation) {
  w.identity(evaluation.id);
  w.generation(evaluation.generation);
  w.identity(evaluation.candidate);
  w.generation(evaluation.candidate_generation);
  w.identity(evaluation.task);
  w.generation(evaluation.task_generation);
  w.identity(evaluation.population);
  w.generation(evaluation.population_generation);
  w.identity(evaluation.evaluator);
  w.str(evaluation.evaluator_key);
  w.u8(static_cast<std::uint8_t>(evaluation.kind));
  w.u8(static_cast<std::uint8_t>(evaluation.requirement_class));
  w.u8(static_cast<std::uint8_t>(evaluation.outcome));
  w.boolean(evaluation.complete);
  w.boolean(evaluation.has_score);
  w.f64(evaluation.score);
  w.epoch(evaluation.decided_epoch);
  w.str(evaluation.diagnostics);
  w.str(evaluation.evidence_digest);
  w.u64(evaluation.duration_micros);
}

void write_ranking_value(Writer& w, const RankingFactorValue& value) {
  w.str(value.key);
  w.u8(static_cast<std::uint8_t>(value.kind));
  w.u8(static_cast<std::uint8_t>(value.direction));
  w.f64(value.weight);
  w.f64(value.raw_value);
  w.boolean(value.available);
  w.f64(value.contribution);
}

void write_ranking_entry(Writer& w, const RankingEntry& entry) {
  w.identity(entry.candidate);
  w.generation(entry.candidate_generation);
  w.u32(entry.rank);
  w.f64(entry.total_score);
  w.u32(static_cast<std::uint32_t>(entry.factors.size()));
  for (const RankingFactorValue& factor : entry.factors) {
    write_ranking_value(w, factor);
  }
  w.u64(entry.artifact_bytes);
  w.u32(entry.lineage_depth);
  w.str(entry.diversity_key);
}

void write_exclusion_entry(Writer& w, const ExclusionEntry& entry) {
  w.identity(entry.candidate);
  w.generation(entry.candidate_generation);
  w.u8(static_cast<std::uint8_t>(entry.stage));
  w.u8(static_cast<std::uint8_t>(entry.reason));
  w.str(entry.detail);
}

void write_selection(PopulationId /*key*/, Writer& w, const SelectionDecision& decision) {
  w.generation(decision.generation);
  w.identity(decision.population);
  w.generation(decision.population_generation);
  w.identity(decision.task);
  w.generation(decision.task_generation);
  w.identity(decision.policy);
  w.generation(decision.policy_generation);
  w.epoch(decision.coordinator_epoch);
  w.u8(static_cast<std::uint8_t>(decision.state));
  w.identity(decision.selected);
  w.generation(decision.selected_generation);
  w.u32(static_cast<std::uint32_t>(decision.ranking.size()));
  for (const RankingEntry& entry : decision.ranking) {
    write_ranking_entry(w, entry);
  }
  w.u32(static_cast<std::uint32_t>(decision.excluded.size()));
  for (const ExclusionEntry& entry : decision.excluded) {
    write_exclusion_entry(w, entry);
  }
  w.str(decision.canonical_state_digest);
  w.str(decision.rationale);
}

void write_retention_entry(Writer& w, const RetentionDecisionEntry& entry) {
  w.identity(entry.candidate);
  w.generation(entry.candidate_generation);
  w.u8(static_cast<std::uint8_t>(entry.outcome));
  w.u32(entry.rank);
  w.str(entry.lineage_key);
  w.str(entry.detail);
}

void write_retention(PopulationId /*key*/, Writer& w, const RetentionDecision& decision) {
  w.generation(decision.selection_generation);
  w.identity(decision.population);
  w.generation(decision.population_generation);
  w.identity(decision.policy);
  w.generation(decision.policy_generation);
  w.epoch(decision.coordinator_epoch);
  w.u32(static_cast<std::uint32_t>(decision.entries.size()));
  for (const RetentionDecisionEntry& entry : decision.entries) {
    write_retention_entry(w, entry);
  }
  w.id_list(decision.retained);
  w.id_list(decision.retired);
  w.str(decision.canonical_state_digest);
  w.boolean(decision.committed);
}

void write_evidence_summary(Writer& w, const PromotionEvidenceSummary& summary) {
  w.str(summary.evaluator_key);
  w.u8(static_cast<std::uint8_t>(summary.kind));
  w.u8(static_cast<std::uint8_t>(summary.outcome));
  w.u8(static_cast<std::uint8_t>(summary.requirement_class));
  w.boolean(summary.complete);
  w.boolean(summary.has_score);
  w.f64(summary.score);
  w.generation(summary.generation);
  w.str(summary.evidence_digest);
}

void write_promotion(PromotionRequestId /*key*/, Writer& w, const PromotionRequest& request) {
  w.identity(request.id);
  w.identity(request.foundry);
  w.identity(request.run);
  w.epoch(request.coordinator_epoch);
  w.identity(request.population);
  w.generation(request.population_generation);
  w.identity(request.candidate);
  w.generation(request.candidate_generation);
  w.identity(request.task);
  w.generation(request.task_generation);
  w.identity(request.lineage);
  w.generation(request.selection_generation);
  w.identity(request.policy);
  w.generation(request.policy_generation);
  w.generation(request.evidence_generation);
  w.u8(static_cast<std::uint8_t>(request.eligibility));
  w.string_list(request.outstanding_requirements);
  w.u32(static_cast<std::uint32_t>(request.artifacts.size()));
  for (const ArtifactRef& artifact : request.artifacts) {
    write_artifact_ref(w, artifact);
  }
  w.u64(request.total_artifact_bytes);
  w.str(request.artifact_set_digest);
  w.u32(static_cast<std::uint32_t>(request.mandatory_evidence.size()));
  for (const PromotionEvidenceSummary& summary : request.mandatory_evidence) {
    write_evidence_summary(w, summary);
  }
  w.id_list(request.ancestry);
  w.u32(request.lineage_depth);
  w.str(request.canonical_state_digest);
}

void write_lineage_node(CandidateId /*key*/, Writer& w, const LineageNode& node) {
  w.identity(node.candidate);
  w.generation(node.candidate_generation);
  w.identity(node.lineage);
  w.id_list(node.parents);
  w.u32(node.depth);
  w.epoch(node.created_epoch);
  w.boolean(node.retired);
  w.epoch(node.retired_epoch);
  w.str(node.retirement_reason);
}

void write_ledger(PopulationId key, Writer& w, const BudgetLedger& ledger) {
  w.identity(key);
  write_budget_limits(w, ledger.limits());
  for (const BudgetLedger::Counter& counter : ledger.counters()) {
    w.u64(counter.reserved);
    w.u64(counter.consumed);
    w.u64(counter.released);
  }
}

// ---------------------------------------------------------------------------
// Record body decoders. Every read is checked before it allocates, and every
// identity, generation and enum is validated against its domain.
// ---------------------------------------------------------------------------

Status read_budget_limits(Reader& reader, BudgetLimits& limits) {
  AF_TRY_ASSIGN(limits.max_candidate_attempts, reader.u32());
  AF_TRY_ASSIGN(limits.max_worker_assignments, reader.u32());
  AF_TRY_ASSIGN(limits.max_evaluation_attempts, reader.u32());
  AF_TRY_ASSIGN(limits.max_retained_candidates, reader.u32());
  AF_TRY_ASSIGN(limits.max_retries, reader.u32());
  AF_TRY_ASSIGN(limits.max_candidates, reader.u32());
  AF_TRY_ASSIGN(limits.max_workers, reader.u32());
  AF_TRY_ASSIGN(limits.max_active_attempts, reader.u32());
  AF_TRY_ASSIGN(limits.max_evaluation_concurrency, reader.u32());
  return Status();
}

Status read_strings(Reader& reader, std::string_view field, std::uint32_t max_count,
                    std::vector<std::string>& out) {
  reader.set_field(field);
  std::uint32_t length{}; AF_TRY_ASSIGN(length, (reader.count(field, 4u, max_count)));
  std::vector<std::string> values;
  values.reserve(length);
  for (std::uint32_t index = 0; index < length; ++index) {
    std::string value;
    AF_TRY_ASSIGN(value, reader.str());
    values.push_back(std::move(value));
  }
  out = std::move(values);
  return Status();
}

Status read_candidate_ids(Reader& reader, std::string_view field,
                          std::vector<CandidateId>& out) {
  reader.set_field(field);
  std::uint32_t length{}; AF_TRY_ASSIGN(length, (reader.count(field, kMinIdentityBytes, static_cast<std::uint32_t>(kMaxCandidatesPerPopulation))));
  std::vector<CandidateId> values;
  values.reserve(length);
  for (std::uint32_t index = 0; index < length; ++index) {
    CandidateId value;
    AF_TRY_ASSIGN(value, reader.identity<CandidateId>(field));
    values.push_back(value);
  }
  out = std::move(values);
  return Status();
}

Result<InputFile> read_input_file(Reader& reader) {
  InputFile file;
  AF_TRY_ASSIGN(file.name, reader.str());
  AF_TRY_ASSIGN(file.content, reader.str());
  AF_TRY_ASSIGN(file.content_digest, reader.str());
  return file;
}

Result<EvaluationRequirement> read_requirement(Reader& reader) {
  EvaluationRequirement requirement;
  std::uint8_t requirement_class{}; AF_TRY_ASSIGN(requirement_class, (reader.enum_value("EvaluationRequirement::requirement_class", 1u)));
  requirement.requirement_class = static_cast<RequirementClass>(requirement_class);
  AF_TRY_ASSIGN(requirement.evaluator_key, reader.str());
  AF_TRY_ASSIGN(requirement.description, reader.str());
  AF_TRY_ASSIGN(requirement.has_score_floor, reader.boolean());
  AF_TRY_ASSIGN(requirement.score_floor, reader.f64());
  AF_TRY_ASSIGN(requirement.ranking_weight, reader.f64());
  return requirement;
}

Result<RankingFactor> read_ranking_factor(Reader& reader) {
  RankingFactor factor;
  std::uint8_t kind{}; AF_TRY_ASSIGN(kind, (reader.enum_value("RankingFactor::kind", static_cast<std::uint8_t>(kRankingFactorKindCount - 1))));
  factor.kind = static_cast<RankingFactorKind>(kind);
  std::uint8_t direction{}; AF_TRY_ASSIGN(direction, (reader.enum_value("RankingFactor::direction", 1u)));
  factor.direction = static_cast<RankingDirection>(direction);
  AF_TRY_ASSIGN(factor.weight, reader.f64());
  AF_TRY_ASSIGN(factor.key, reader.str());
  return factor;
}

Status read_selection_policy(Reader& reader, SelectionPolicy& selection) {
  std::uint32_t factor_count{}; AF_TRY_ASSIGN(factor_count, (reader.count("SelectionPolicy::factors", kMinRankingFactorBytes, static_cast<std::uint32_t>(kMaxRankingFactors))));
  std::vector<RankingFactor> factors;
  factors.reserve(factor_count);
  for (std::uint32_t index = 0; index < factor_count; ++index) {
    RankingFactor factor;
    AF_TRY_ASSIGN(factor, read_ranking_factor(reader));
    factors.push_back(std::move(factor));
  }
  selection.factors = std::move(factors);
  AF_TRY_ASSIGN(selection.require_complete_mandatory, reader.boolean());
  return Status();
}

Status read_retention_policy(Reader& reader, RetentionPolicy& retention) {
  AF_TRY_ASSIGN(retention.retain_top_k, reader.u32());
  AF_TRY_ASSIGN(retention.max_retained, reader.u32());
  AF_TRY_ASSIGN(retention.max_per_lineage, reader.u32());
  AF_TRY_ASSIGN(retention.retain_selected, reader.boolean());
  return Status();
}

Result<ArtifactRef> read_artifact_ref(Reader& reader) {
  ArtifactRef artifact;
  AF_TRY_ASSIGN(artifact.name, reader.str());
  AF_TRY_ASSIGN(artifact.size_bytes, reader.u64());
  AF_TRY_ASSIGN(artifact.content_digest, reader.str());
  return artifact;
}

Result<TaskSpec> read_task(Reader& reader) {
  TaskSpec task;
  AF_TRY_ASSIGN(task.id, reader.identity<TaskId>("TaskSpec::id"));
  AF_TRY_ASSIGN(task.generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(task.name, reader.str());
  AF_TRY_ASSIGN(task.objective, reader.str());
  std::uint32_t input_count{}; AF_TRY_ASSIGN(input_count, (reader.count("TaskSpec::inputs", kMinInputFileBytes, static_cast<std::uint32_t>(kMaxInputFiles))));
  std::vector<InputFile> inputs;
  inputs.reserve(input_count);
  for (std::uint32_t index = 0; index < input_count; ++index) {
    InputFile file;
    AF_TRY_ASSIGN(file, read_input_file(reader));
    inputs.push_back(std::move(file));
  }
  task.inputs = std::move(inputs);
  AF_TRY(read_strings(reader, "TaskSpec::required_outputs",
                      static_cast<std::uint32_t>(kMaxRequiredOutputs),
                      task.required_outputs));
  std::uint32_t requirement_count{}; AF_TRY_ASSIGN(requirement_count, (reader.count("TaskSpec::requirements", kMinRequirementBytes, static_cast<std::uint32_t>(kMaxEvaluationRequirements))));
  std::vector<EvaluationRequirement> requirements;
  requirements.reserve(requirement_count);
  for (std::uint32_t index = 0; index < requirement_count; ++index) {
    EvaluationRequirement requirement;
    AF_TRY_ASSIGN(requirement, read_requirement(reader));
    requirements.push_back(std::move(requirement));
  }
  task.requirements = std::move(requirements);
  AF_TRY(read_strings(reader, "TaskSpec::hard_constraints",
                      static_cast<std::uint32_t>(kMaxConstraints),
                      task.hard_constraints));
  AF_TRY(read_strings(reader, "TaskSpec::closure_criteria",
                      static_cast<std::uint32_t>(kMaxConstraints),
                      task.closure_criteria));
  AF_TRY_ASSIGN(task.policy, reader.identity<PolicyId>("TaskSpec::policy"));
  AF_TRY_ASSIGN(task.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY(read_budget_limits(reader, task.budgets));
  AF_TRY_ASSIGN(task.content_digest, reader.str());
  return task;
}

Result<FoundryPolicy> read_policy(Reader& reader) {
  FoundryPolicy policy;
  AF_TRY_ASSIGN(policy.id, reader.identity<PolicyId>("FoundryPolicy::id"));
  AF_TRY_ASSIGN(policy.generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(policy.name, reader.str());
  AF_TRY(read_selection_policy(reader, policy.selection));
  AF_TRY(read_retention_policy(reader, policy.retention));
  AF_TRY(read_budget_limits(reader, policy.budgets));
  AF_TRY_ASSIGN(policy.max_generation_depth, reader.u32());
  AF_TRY_ASSIGN(policy.carry_forward_elite, reader.boolean());
  AF_TRY_ASSIGN(policy.content_digest, reader.str());
  return policy;
}

Result<PopulationRecord> read_population(Reader& reader) {
  PopulationRecord population;
  AF_TRY_ASSIGN(population.id, reader.identity<PopulationId>("PopulationRecord::id"));
  AF_TRY_ASSIGN(population.generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(population.run, reader.identity<FoundryRunId>("PopulationRecord::run"));
  AF_TRY_ASSIGN(population.name, reader.str());
  AF_TRY_ASSIGN(population.task, reader.identity<TaskId>("PopulationRecord::task"));
  AF_TRY_ASSIGN(population.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(population.policy, reader.identity<PolicyId>("PopulationRecord::policy"));
  AF_TRY_ASSIGN(population.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(population.candidate_budget, reader.u32());
  AF_TRY_ASSIGN(population.worker_budget, reader.u32());
  AF_TRY_ASSIGN(population.population_index, reader.u32());
  AF_TRY_ASSIGN(population.predecessor,
                reader.optional_identity<PopulationId>("PopulationRecord::predecessor"));
  AF_TRY_ASSIGN(population.seed_candidate,
                reader.optional_identity<CandidateId>("PopulationRecord::seed_candidate"));
  AF_TRY_ASSIGN(population.seed_candidate_generation,
                reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(population.seed_lineage,
                reader.optional_identity<LineageId>("PopulationRecord::seed_lineage"));
  std::uint8_t state{}; AF_TRY_ASSIGN(state, (reader.enum_value("PopulationRecord::state", static_cast<std::uint8_t>(kPopulationStateCount - 1))));
  population.state = static_cast<PopulationState>(state);
  AF_TRY_ASSIGN(population.created_epoch, reader.epoch());
  AF_TRY_ASSIGN(population.state_epoch, reader.epoch());
  AF_TRY_ASSIGN(population.committed_selection, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(population.retention_committed, reader.boolean());
  AF_TRY_ASSIGN(population.retention_selection_generation,
                reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(population.require_promotion_request, reader.boolean());
  AF_TRY(read_candidate_ids(reader, "PopulationRecord::candidates",
                           population.candidates));
  AF_TRY_ASSIGN(population.promotion_requests, reader.u64());
  AF_TRY_ASSIGN(population.committed_promotion_request,
                reader.optional_identity<PromotionRequestId>(
                    "PopulationRecord::committed_promotion_request"));
  AF_TRY_ASSIGN(population.status_detail, reader.str());
  AF_TRY(read_strings(reader, "PopulationRecord::closure_blockers",
                      kMaxClosureBlockers, population.closure_blockers));
  return population;
}

Result<CandidateRecord> read_candidate(Reader& reader) {
  CandidateRecord candidate;
  AF_TRY_ASSIGN(candidate.id, reader.identity<CandidateId>("CandidateRecord::id"));
  AF_TRY_ASSIGN(candidate.generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(candidate.population,
                reader.identity<PopulationId>("CandidateRecord::population"));
  AF_TRY_ASSIGN(candidate.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(candidate.task, reader.identity<TaskId>("CandidateRecord::task"));
  AF_TRY_ASSIGN(candidate.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(candidate.lineage, reader.identity<LineageId>("CandidateRecord::lineage"));
  AF_TRY(read_candidate_ids(reader, "CandidateRecord::parents", candidate.parents));
  AF_TRY_ASSIGN(candidate.depth, reader.u32());
  AF_TRY_ASSIGN(candidate.producer_worker,
                reader.optional_identity<WorkerId>("CandidateRecord::producer_worker"));
  AF_TRY_ASSIGN(candidate.producer_boot,
                reader.optional_identity<WorkerBootId>("CandidateRecord::producer_boot"));
  AF_TRY_ASSIGN(candidate.attempt, reader.optional_identity<AttemptId>("CandidateRecord::attempt"));
  AF_TRY_ASSIGN(candidate.attempt_generation, reader.generation<AttemptIdTag>());
  AF_TRY_ASSIGN(candidate.assignment,
                reader.optional_identity<AssignmentId>("CandidateRecord::assignment"));
  AF_TRY_ASSIGN(candidate.created_epoch, reader.epoch());
  AF_TRY_ASSIGN(candidate.state_changed_epoch, reader.epoch());
  std::uint8_t state{}; AF_TRY_ASSIGN(state, (reader.enum_value("CandidateRecord::state", static_cast<std::uint8_t>(kCandidateStateCount - 1))));
  candidate.state = static_cast<CandidateState>(state);
  std::uint32_t artifact_count{}; AF_TRY_ASSIGN(artifact_count, (reader.count("CandidateRecord::artifacts", kMinArtifactRefBytes, static_cast<std::uint32_t>(kMaxArtifactsPerCandidate))));
  std::vector<ArtifactRef> artifacts;
  artifacts.reserve(artifact_count);
  for (std::uint32_t index = 0; index < artifact_count; ++index) {
    ArtifactRef artifact;
    AF_TRY_ASSIGN(artifact, read_artifact_ref(reader));
    artifacts.push_back(std::move(artifact));
  }
  candidate.artifacts = std::move(artifacts);
  AF_TRY_ASSIGN(candidate.artifact_set_digest, reader.str());
  AF_TRY_ASSIGN(candidate.attempt_count, reader.u32());
  AF_TRY_ASSIGN(candidate.evidence_generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(candidate.evaluation_generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(candidate.selection_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(candidate.selected, reader.boolean());
  AF_TRY_ASSIGN(candidate.retained, reader.boolean());
  AF_TRY_ASSIGN(candidate.promotion_eligible, reader.boolean());
  AF_TRY_ASSIGN(candidate.promotion_requested, reader.boolean());
  AF_TRY_ASSIGN(candidate.diversity_key, reader.str());
  AF_TRY_ASSIGN(candidate.failure_reason, reader.str());
  AF_TRY_ASSIGN(candidate.superseded_by_reason, reader.str());
  return candidate;
}

Result<WorkerRecord> read_worker(Reader& reader) {
  WorkerRecord worker;
  AF_TRY_ASSIGN(worker.id, reader.identity<WorkerId>("WorkerRecord::id"));
  AF_TRY_ASSIGN(worker.boot, reader.identity<WorkerBootId>("WorkerRecord::boot"));
  AF_TRY_ASSIGN(worker.label, reader.str());
  AF_TRY_ASSIGN(worker.process_id, reader.u32());
  std::uint8_t state{}; AF_TRY_ASSIGN(state, (reader.enum_value("WorkerRecord::state", static_cast<std::uint8_t>(kWorkerStateCount - 1))));
  worker.state = static_cast<WorkerState>(state);
  AF_TRY_ASSIGN(worker.registered_epoch, reader.epoch());
  AF_TRY_ASSIGN(worker.state_epoch, reader.epoch());
  AF_TRY_ASSIGN(worker.session, reader.identity<SessionId>("WorkerRecord::session"));
  AF_TRY_ASSIGN(worker.session_generation, reader.generation<SessionIdTag>());
  AF_TRY_ASSIGN(worker.capability, reader.str());
  AF_TRY_ASSIGN(worker.assignments_accepted, reader.u64());
  AF_TRY_ASSIGN(worker.assignments_completed, reader.u64());
  AF_TRY_ASSIGN(worker.assignments_failed, reader.u64());
  AF_TRY_ASSIGN(worker.stale_messages_rejected, reader.u64());
  AF_TRY_ASSIGN(worker.active_attempt,
                reader.optional_identity<AttemptId>("WorkerRecord::active_attempt"));
  AF_TRY_ASSIGN(worker.active_attempt_generation, reader.generation<AttemptIdTag>());
  AF_TRY_ASSIGN(worker.active_assignment,
                reader.optional_identity<AssignmentId>("WorkerRecord::active_assignment"));
  AF_TRY_ASSIGN(worker.last_diagnostic, reader.str());
  return worker;
}

Result<AttemptRecord> read_attempt(Reader& reader) {
  AttemptRecord attempt;
  AF_TRY_ASSIGN(attempt.id, reader.identity<AttemptId>("AttemptRecord::id"));
  AF_TRY_ASSIGN(attempt.generation, reader.generation<AttemptIdTag>());
  AF_TRY_ASSIGN(attempt.population, reader.identity<PopulationId>("AttemptRecord::population"));
  AF_TRY_ASSIGN(attempt.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(attempt.task, reader.identity<TaskId>("AttemptRecord::task"));
  AF_TRY_ASSIGN(attempt.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(attempt.candidate, reader.identity<CandidateId>("AttemptRecord::candidate"));
  AF_TRY_ASSIGN(attempt.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(attempt.worker, reader.identity<WorkerId>("AttemptRecord::worker"));
  AF_TRY_ASSIGN(attempt.worker_boot, reader.identity<WorkerBootId>("AttemptRecord::worker_boot"));
  AF_TRY_ASSIGN(attempt.session, reader.identity<SessionId>("AttemptRecord::session"));
  AF_TRY_ASSIGN(attempt.assignment, reader.identity<AssignmentId>("AttemptRecord::assignment"));
  AF_TRY_ASSIGN(attempt.authorized_epoch, reader.epoch());
  AF_TRY_ASSIGN(attempt.dispatched_epoch, reader.epoch());
  AF_TRY_ASSIGN(attempt.terminal_epoch, reader.epoch());
  std::uint8_t state{}; AF_TRY_ASSIGN(state, (reader.enum_value("AttemptRecord::state", static_cast<std::uint8_t>(kAttemptStateCount - 1))));
  attempt.state = static_cast<AttemptState>(state);
  AF_TRY_ASSIGN(attempt.retry_index, reader.u32());
  AF_TRY_ASSIGN(attempt.previous_attempt,
                reader.optional_identity<AttemptId>("AttemptRecord::previous_attempt"));
  AF_TRY_ASSIGN(attempt.failure_detail, reader.str());
  AF_TRY_ASSIGN(attempt.holds_reservation, reader.boolean());
  return attempt;
}

Result<AssignmentRecord> read_assignment(Reader& reader) {
  AssignmentRecord assignment;
  AF_TRY_ASSIGN(assignment.id, reader.identity<AssignmentId>("AssignmentRecord::id"));
  AF_TRY_ASSIGN(assignment.generation, reader.generation<AssignmentIdTag>());
  AF_TRY_ASSIGN(assignment.attempt, reader.identity<AttemptId>("AssignmentRecord::attempt"));
  AF_TRY_ASSIGN(assignment.attempt_generation, reader.generation<AttemptIdTag>());
  AF_TRY_ASSIGN(assignment.worker, reader.identity<WorkerId>("AssignmentRecord::worker"));
  AF_TRY_ASSIGN(assignment.worker_boot,
                reader.identity<WorkerBootId>("AssignmentRecord::worker_boot"));
  AF_TRY_ASSIGN(assignment.session, reader.identity<SessionId>("AssignmentRecord::session"));
  AF_TRY_ASSIGN(assignment.session_generation, reader.generation<SessionIdTag>());
  AF_TRY_ASSIGN(assignment.issued_epoch, reader.epoch());
  AF_TRY_ASSIGN(assignment.acknowledged, reader.boolean());
  AF_TRY_ASSIGN(assignment.acknowledged_epoch, reader.epoch());
  AF_TRY_ASSIGN(assignment.revoked, reader.boolean());
  AF_TRY_ASSIGN(assignment.revoked_epoch, reader.epoch());
  return assignment;
}

Result<EvaluationRecord> read_evaluation(Reader& reader) {
  EvaluationRecord evaluation;
  AF_TRY_ASSIGN(evaluation.id, reader.identity<EvaluationId>("EvaluationRecord::id"));
  AF_TRY_ASSIGN(evaluation.generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(evaluation.candidate,
                reader.identity<CandidateId>("EvaluationRecord::candidate"));
  AF_TRY_ASSIGN(evaluation.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(evaluation.task, reader.identity<TaskId>("EvaluationRecord::task"));
  AF_TRY_ASSIGN(evaluation.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(evaluation.population,
                reader.identity<PopulationId>("EvaluationRecord::population"));
  AF_TRY_ASSIGN(evaluation.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(evaluation.evaluator,
                reader.identity<EvaluatorId>("EvaluationRecord::evaluator"));
  AF_TRY_ASSIGN(evaluation.evaluator_key, reader.str());
  std::uint8_t kind{}; AF_TRY_ASSIGN(kind, (reader.enum_value("EvaluationRecord::kind", static_cast<std::uint8_t>(kEvaluatorKindCount - 1))));
  evaluation.kind = static_cast<EvaluatorKind>(kind);
  std::uint8_t requirement_class{}; AF_TRY_ASSIGN(requirement_class, (reader.enum_value("EvaluationRecord::requirement_class", 1u)));
  evaluation.requirement_class = static_cast<RequirementClass>(requirement_class);
  std::uint8_t outcome{}; AF_TRY_ASSIGN(outcome, (reader.enum_value("EvaluationRecord::outcome", static_cast<std::uint8_t>(kEvaluationOutcomeCount - 1))));
  evaluation.outcome = static_cast<EvaluationOutcome>(outcome);
  AF_TRY_ASSIGN(evaluation.complete, reader.boolean());
  AF_TRY_ASSIGN(evaluation.has_score, reader.boolean());
  AF_TRY_ASSIGN(evaluation.score, reader.f64());
  AF_TRY_ASSIGN(evaluation.decided_epoch, reader.epoch());
  AF_TRY_ASSIGN(evaluation.diagnostics, reader.str());
  AF_TRY_ASSIGN(evaluation.evidence_digest, reader.str());
  AF_TRY_ASSIGN(evaluation.duration_micros, reader.u64());
  return evaluation;
}

Result<RankingFactorValue> read_ranking_value(Reader& reader) {
  RankingFactorValue value;
  AF_TRY_ASSIGN(value.key, reader.str());
  std::uint8_t kind{}; AF_TRY_ASSIGN(kind, (reader.enum_value("RankingFactorValue::kind", static_cast<std::uint8_t>(kRankingFactorKindCount - 1))));
  value.kind = static_cast<RankingFactorKind>(kind);
  std::uint8_t direction{}; AF_TRY_ASSIGN(direction, (reader.enum_value("RankingFactorValue::direction", 1u)));
  value.direction = static_cast<RankingDirection>(direction);
  AF_TRY_ASSIGN(value.weight, reader.f64());
  AF_TRY_ASSIGN(value.raw_value, reader.f64());
  AF_TRY_ASSIGN(value.available, reader.boolean());
  AF_TRY_ASSIGN(value.contribution, reader.f64());
  return value;
}

Result<RankingEntry> read_ranking_entry(Reader& reader) {
  RankingEntry entry;
  AF_TRY_ASSIGN(entry.candidate, reader.identity<CandidateId>("RankingEntry::candidate"));
  AF_TRY_ASSIGN(entry.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(entry.rank, reader.u32());
  AF_TRY_ASSIGN(entry.total_score, reader.f64());
  std::uint32_t factor_count{};
  AF_TRY_ASSIGN(factor_count, (reader.count("RankingEntry::factors", kMinRankingValueBytes, static_cast<std::uint32_t>(kMaxRankingFactors))));
  std::vector<RankingFactorValue> factors;
  factors.reserve(factor_count);
  for (std::uint32_t index = 0; index < factor_count; ++index) {
    RankingFactorValue factor;
    AF_TRY_ASSIGN(factor, read_ranking_value(reader));
    factors.push_back(std::move(factor));
  }
  entry.factors = std::move(factors);
  AF_TRY_ASSIGN(entry.artifact_bytes, reader.u64());
  AF_TRY_ASSIGN(entry.lineage_depth, reader.u32());
  AF_TRY_ASSIGN(entry.diversity_key, reader.str());
  return entry;
}

Result<ExclusionEntry> read_exclusion_entry(Reader& reader) {
  ExclusionEntry entry;
  AF_TRY_ASSIGN(entry.candidate, reader.identity<CandidateId>("ExclusionEntry::candidate"));
  AF_TRY_ASSIGN(entry.candidate_generation, reader.generation<CandidateIdTag>());
  std::uint8_t stage{};
  AF_TRY_ASSIGN(stage, (reader.enum_value("ExclusionEntry::stage",
                                          static_cast<std::uint8_t>(kSelectionStageCount - 1))));
  entry.stage = static_cast<SelectionStage>(stage);
  std::uint8_t reason{};
  AF_TRY_ASSIGN(reason, (reader.enum_value("ExclusionEntry::reason",
                                           static_cast<std::uint8_t>(kExclusionReasonCount - 1))));
  entry.reason = static_cast<ExclusionReason>(reason);
  AF_TRY_ASSIGN(entry.detail, reader.str());
  return entry;
}

Result<SelectionDecision> read_selection(Reader& reader) {
  SelectionDecision decision;
  AF_TRY_ASSIGN(decision.generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(decision.population,
                reader.identity<PopulationId>("SelectionDecision::population"));
  AF_TRY_ASSIGN(decision.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(decision.task, reader.identity<TaskId>("SelectionDecision::task"));
  AF_TRY_ASSIGN(decision.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(decision.policy, reader.identity<PolicyId>("SelectionDecision::policy"));
  AF_TRY_ASSIGN(decision.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(decision.coordinator_epoch, reader.epoch());
  std::uint8_t state{};
  AF_TRY_ASSIGN(state, (reader.enum_value("SelectionDecision::state", 3u)));
  decision.state = static_cast<SelectionDecisionState>(state);
  AF_TRY_ASSIGN(decision.selected,
                reader.optional_identity<CandidateId>("SelectionDecision::selected"));
  AF_TRY_ASSIGN(decision.selected_generation, reader.generation<CandidateIdTag>());
  std::uint32_t ranking_count{};
  AF_TRY_ASSIGN(ranking_count,
                (reader.count("SelectionDecision::ranking", kMinRankingEntryBytes,
                              kMaxCandidatesPerPopulation)));
  std::vector<RankingEntry> ranking;
  ranking.reserve(ranking_count);
  for (std::uint32_t index = 0; index < ranking_count; ++index) {
    RankingEntry entry;
    AF_TRY_ASSIGN(entry, read_ranking_entry(reader));
    ranking.push_back(std::move(entry));
  }
  decision.ranking = std::move(ranking);
  std::uint32_t excluded_count{};
  AF_TRY_ASSIGN(excluded_count,
                (reader.count("SelectionDecision::excluded", kMinExclusionEntryBytes,
                              kMaxCandidatesPerPopulation)));
  std::vector<ExclusionEntry> excluded;
  excluded.reserve(excluded_count);
  for (std::uint32_t index = 0; index < excluded_count; ++index) {
    ExclusionEntry entry;
    AF_TRY_ASSIGN(entry, read_exclusion_entry(reader));
    excluded.push_back(std::move(entry));
  }
  decision.excluded = std::move(excluded);
  AF_TRY_ASSIGN(decision.canonical_state_digest, reader.str());
  AF_TRY_ASSIGN(decision.rationale, reader.str());
  return decision;
}

Result<RetentionDecisionEntry> read_retention_entry(Reader& reader) {
  RetentionDecisionEntry entry;
  AF_TRY_ASSIGN(entry.candidate,
                reader.identity<CandidateId>("RetentionDecisionEntry::candidate"));
  AF_TRY_ASSIGN(entry.candidate_generation, reader.generation<CandidateIdTag>());
  std::uint8_t outcome{};
  AF_TRY_ASSIGN(outcome, (reader.enum_value("RetentionDecisionEntry::outcome", 6u)));
  entry.outcome = static_cast<RetentionOutcome>(outcome);
  AF_TRY_ASSIGN(entry.rank, reader.u32());
  AF_TRY_ASSIGN(entry.lineage_key, reader.str());
  AF_TRY_ASSIGN(entry.detail, reader.str());
  return entry;
}

Result<RetentionDecision> read_retention(Reader& reader) {
  RetentionDecision decision;
  AF_TRY_ASSIGN(decision.selection_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(decision.population,
                reader.identity<PopulationId>("RetentionDecision::population"));
  AF_TRY_ASSIGN(decision.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(decision.policy, reader.identity<PolicyId>("RetentionDecision::policy"));
  AF_TRY_ASSIGN(decision.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(decision.coordinator_epoch, reader.epoch());
  std::uint32_t entry_count{};
  AF_TRY_ASSIGN(entry_count,
                (reader.count("RetentionDecision::entries", kMinRetentionEntryBytes,
                              kMaxCandidatesPerPopulation)));
  std::vector<RetentionDecisionEntry> entries;
  entries.reserve(entry_count);
  for (std::uint32_t index = 0; index < entry_count; ++index) {
    RetentionDecisionEntry entry;
    AF_TRY_ASSIGN(entry, read_retention_entry(reader));
    entries.push_back(std::move(entry));
  }
  decision.entries = std::move(entries);
  AF_TRY(read_candidate_ids(reader, "RetentionDecision::retained", decision.retained));
  AF_TRY(read_candidate_ids(reader, "RetentionDecision::retired", decision.retired));
  AF_TRY_ASSIGN(decision.canonical_state_digest, reader.str());
  AF_TRY_ASSIGN(decision.committed, reader.boolean());
  return decision;
}

Result<PromotionEvidenceSummary> read_evidence_summary(Reader& reader) {
  PromotionEvidenceSummary summary;
  AF_TRY_ASSIGN(summary.evaluator_key, reader.str());
  std::uint8_t kind{};
  AF_TRY_ASSIGN(kind, (reader.enum_value("PromotionEvidenceSummary::kind",
                                         static_cast<std::uint8_t>(kEvaluatorKindCount - 1))));
  summary.kind = static_cast<EvaluatorKind>(kind);
  std::uint8_t outcome{};
  AF_TRY_ASSIGN(outcome,
                (reader.enum_value("PromotionEvidenceSummary::outcome",
                                   static_cast<std::uint8_t>(kEvaluationOutcomeCount - 1))));
  summary.outcome = static_cast<EvaluationOutcome>(outcome);
  std::uint8_t requirement_class{};
  AF_TRY_ASSIGN(requirement_class,
                (reader.enum_value("PromotionEvidenceSummary::requirement_class", 1u)));
  summary.requirement_class = static_cast<RequirementClass>(requirement_class);
  AF_TRY_ASSIGN(summary.complete, reader.boolean());
  AF_TRY_ASSIGN(summary.has_score, reader.boolean());
  AF_TRY_ASSIGN(summary.score, reader.f64());
  AF_TRY_ASSIGN(summary.generation, reader.generation<EvaluationIdTag>());
  AF_TRY_ASSIGN(summary.evidence_digest, reader.str());
  return summary;
}

Result<PromotionRequest> read_promotion(Reader& reader) {
  PromotionRequest request;
  AF_TRY_ASSIGN(request.id, reader.identity<PromotionRequestId>("PromotionRequest::id"));
  AF_TRY_ASSIGN(request.foundry, reader.identity<FoundryId>("PromotionRequest::foundry"));
  AF_TRY_ASSIGN(request.run, reader.identity<FoundryRunId>("PromotionRequest::run"));
  AF_TRY_ASSIGN(request.coordinator_epoch, reader.epoch());
  AF_TRY_ASSIGN(request.population,
                reader.identity<PopulationId>("PromotionRequest::population"));
  AF_TRY_ASSIGN(request.population_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(request.candidate,
                reader.identity<CandidateId>("PromotionRequest::candidate"));
  AF_TRY_ASSIGN(request.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(request.task, reader.identity<TaskId>("PromotionRequest::task"));
  AF_TRY_ASSIGN(request.task_generation, reader.generation<TaskIdTag>());
  AF_TRY_ASSIGN(request.lineage, reader.identity<LineageId>("PromotionRequest::lineage"));
  AF_TRY_ASSIGN(request.selection_generation, reader.generation<PopulationIdTag>());
  AF_TRY_ASSIGN(request.policy, reader.identity<PolicyId>("PromotionRequest::policy"));
  AF_TRY_ASSIGN(request.policy_generation, reader.generation<PolicyIdTag>());
  AF_TRY_ASSIGN(request.evidence_generation, reader.generation<EvaluationIdTag>());
  std::uint8_t eligibility{};
  AF_TRY_ASSIGN(eligibility, (reader.enum_value("PromotionRequest::eligibility", 3u)));
  request.eligibility = static_cast<PromotionEligibilityState>(eligibility);
  AF_TRY(read_strings(reader, "PromotionRequest::outstanding_requirements",
                      kMaxOutstandingRequirements, request.outstanding_requirements));
  std::uint32_t artifact_count{};
  AF_TRY_ASSIGN(artifact_count,
                (reader.count("PromotionRequest::artifacts", kMinArtifactRefBytes,
                              static_cast<std::uint32_t>(kMaxArtifactsPerCandidate))));
  std::vector<ArtifactRef> artifacts;
  artifacts.reserve(artifact_count);
  for (std::uint32_t index = 0; index < artifact_count; ++index) {
    ArtifactRef artifact;
    AF_TRY_ASSIGN(artifact, read_artifact_ref(reader));
    artifacts.push_back(std::move(artifact));
  }
  request.artifacts = std::move(artifacts);
  AF_TRY_ASSIGN(request.total_artifact_bytes, reader.u64());
  AF_TRY_ASSIGN(request.artifact_set_digest, reader.str());
  std::uint32_t evidence_count{};
  AF_TRY_ASSIGN(evidence_count,
                (reader.count("PromotionRequest::mandatory_evidence",
                              kMinEvidenceSummaryBytes,
                              static_cast<std::uint32_t>(kMaxEvaluationRequirements))));
  std::vector<PromotionEvidenceSummary> evidence;
  evidence.reserve(evidence_count);
  for (std::uint32_t index = 0; index < evidence_count; ++index) {
    PromotionEvidenceSummary summary;
    AF_TRY_ASSIGN(summary, read_evidence_summary(reader));
    evidence.push_back(std::move(summary));
  }
  request.mandatory_evidence = std::move(evidence);
  AF_TRY(read_candidate_ids(reader, "PromotionRequest::ancestry", request.ancestry));
  AF_TRY_ASSIGN(request.lineage_depth, reader.u32());
  AF_TRY_ASSIGN(request.canonical_state_digest, reader.str());
  return request;
}

Result<LineageNode> read_lineage_node(Reader& reader) {
  LineageNode node;
  AF_TRY_ASSIGN(node.candidate, reader.identity<CandidateId>("LineageNode::candidate"));
  AF_TRY_ASSIGN(node.candidate_generation, reader.generation<CandidateIdTag>());
  AF_TRY_ASSIGN(node.lineage, reader.identity<LineageId>("LineageNode::lineage"));
  AF_TRY(read_candidate_ids(reader, "LineageNode::parents", node.parents));
  AF_TRY_ASSIGN(node.depth, reader.u32());
  AF_TRY_ASSIGN(node.created_epoch, reader.epoch());
  AF_TRY_ASSIGN(node.retired, reader.boolean());
  AF_TRY_ASSIGN(node.retired_epoch, reader.epoch());
  AF_TRY_ASSIGN(node.retirement_reason, reader.str());
  return node;
}

Result<BudgetLedger> read_ledger(PopulationId key, Reader& reader) {
  (void)key;
  BudgetLimits limits;
  AF_TRY(read_budget_limits(reader, limits));
  std::array<BudgetLedger::Counter, kBudgetKindCount> counters{};
  for (BudgetLedger::Counter& counter : counters) {
    AF_TRY_ASSIGN(counter.reserved, reader.u64());
    AF_TRY_ASSIGN(counter.consumed, reader.u64());
    AF_TRY_ASSIGN(counter.released, reader.u64());
  }
  BudgetLedger ledger(limits);
  ledger.restore(counters);
  return ledger;
}

// ---------------------------------------------------------------------------
// Payload assembly
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Payload assembly
// ---------------------------------------------------------------------------

void write_identity_header(Writer& w, const FoundrySnapshot& snapshot) {
  w.identity(snapshot.foundry);
  w.identity(snapshot.run);
  w.epoch(snapshot.epoch);
  w.u64(snapshot.revision);
  w.u32(snapshot.id_salt);
  w.counters(snapshot.id_counters);
}

void write_statistics(Writer& w, const FoundryStatistics& statistics) {
  w.u64(statistics.task_revisions);
  w.u64(statistics.policy_revisions);
  w.u64(statistics.populations_created);
  w.u64(statistics.populations_closed);
  w.u64(statistics.candidate_slots_created);
  w.u64(statistics.candidates_published);
  w.u64(statistics.attempts_authorized);
  w.u64(statistics.attempts_completed);
  w.u64(statistics.attempts_failed);
  w.u64(statistics.attempts_cancelled);
  w.u64(statistics.attempts_outcome_unknown);
  w.u64(statistics.evaluations_recorded);
  w.u64(statistics.selections_prepared);
  w.u64(statistics.selections_committed);
  w.u64(statistics.retentions_committed);
  w.u64(statistics.promotion_requests);
  w.u64(statistics.stale_authority_rejections);
  w.u64(statistics.duplicate_rejections);
  w.u64(statistics.late_rejections);
  w.u64(statistics.illegal_transition_rejections);
}

/// A lowercase hexadecimal rendering of a 32 bit value, for diagnostics.
[[nodiscard]] std::string hex32(std::uint32_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(10);
  text.append("0x");
  for (int shift = 28; shift >= 0; shift -= 4) {
    text.push_back(kHex[(value >> shift) & 0x0Fu]);
  }
  return text;
}

// ---------------------------------------------------------------------------
// File image header
// ---------------------------------------------------------------------------

[[nodiscard]] Status header_bytes(std::string_view image, const SnapshotHeader& header,
                                  std::size_t& total) {
  const std::uint64_t declared =
      static_cast<std::uint64_t>(kSnapshotHeaderBytes) + header.payload_length;
  if (declared > std::numeric_limits<std::size_t>::max()) {
    return Status(ErrorCode::PersistenceTruncated,
                  "snapshot declares a payload of " + std::to_string(header.payload_length) +
                      " bytes, which no file on this platform can hold");
  }
  total = static_cast<std::size_t>(declared);
  if (declared > static_cast<std::uint64_t>(image.size())) {
    return Status(ErrorCode::PersistenceTruncated,
                  "snapshot declares " + std::to_string(declared) +
                      " bytes but the image holds " + std::to_string(image.size()));
  }
  if (total < image.size()) {
    return Status(ErrorCode::PersistenceTrailingGarbage,
                  "snapshot declares " + std::to_string(declared) +
                      " bytes but the image holds " + std::to_string(image.size()));
  }
  if (total > static_cast<std::size_t>(kSnapshotHeaderBytes) &&
      header.payload_length > kMaxSnapshotPayloadBytes) {
    return Status(ErrorCode::PersistenceTruncated,
                  "snapshot declares a payload of " + std::to_string(header.payload_length) +
                      " bytes, beyond the " + std::to_string(kMaxSnapshotPayloadBytes) +
                      " byte snapshot payload ceiling");
  }
  return Status();
}

/// Steps one to five of the documented header validation order. Nothing is
/// allocated from the payload and no body is touched.
[[nodiscard]] Status validate_header(std::string_view image, SnapshotHeader& header,
                                     std::size_t& total) {
  if (image.size() < kSnapshotHeaderBytes) {
    return Status(ErrorCode::PersistenceTruncated,
                  "snapshot image holds " + std::to_string(image.size()) +
                      " bytes, fewer than the " + std::to_string(kSnapshotHeaderBytes) +
                      " byte header");
  }

  const std::string_view magic = image.substr(0, kSnapshotMagic.size());
  if (magic != kSnapshotMagic) {
    std::string seen;
    for (const char byte : magic) {
      const auto value = static_cast<unsigned char>(byte);
      if (value >= 0x20u && value < 0x7Fu) {
        seen.push_back(static_cast<char>(value));
      } else {
        static constexpr char kHex[] = "0123456789abcdef";
        seen.append("\\x");
        seen.push_back(kHex[(value >> 4) & 0x0Fu]);
        seen.push_back(kHex[value & 0x0Fu]);
      }
    }
    return Status(ErrorCode::PersistenceCorrupt,
                  "snapshot magic is '" + seen + "' but '" + std::string(kSnapshotMagic) +
                      "' was expected");
  }

  const auto* bytes = reinterpret_cast<const unsigned char*>(image.data());
  header.format_version = static_cast<std::uint32_t>(bytes[4]) |
                          (static_cast<std::uint32_t>(bytes[5]) << 8) |
                          (static_cast<std::uint32_t>(bytes[6]) << 16) |
                          (static_cast<std::uint32_t>(bytes[7]) << 24);
  std::uint64_t payload_length = 0;
  for (unsigned index = 0; index < 8; ++index) {
    payload_length |= static_cast<std::uint64_t>(bytes[8 + index]) << (8 * index);
  }
  header.payload_length = payload_length;
  header.payload_crc32c = static_cast<std::uint32_t>(bytes[16]) |
                          (static_cast<std::uint32_t>(bytes[17]) << 8) |
                          (static_cast<std::uint32_t>(bytes[18]) << 16) |
                          (static_cast<std::uint32_t>(bytes[19]) << 24);

  std::uint32_t reserved = 0;
  for (unsigned index = 0; index < 4; ++index) {
    reserved |= static_cast<std::uint32_t>(bytes[20 + index]) << (8 * index);
  }
  if (reserved != 0) {
    return Status(ErrorCode::PersistenceCorrupt,
                  "snapshot reserved header field holds " + std::to_string(reserved) +
                      " but it must be zero");
  }

  if (header.format_version != kSnapshotFormatVersion) {
    return Status(ErrorCode::PersistenceVersionUnsupported,
                  "snapshot format version " + std::to_string(header.format_version) +
                      " is not supported by this build, which writes and reads version " +
                      std::to_string(kSnapshotFormatVersion));
  }

  return header_bytes(image, header, total);
}

// Declared here so that the header path above it can complete the chain.
// The payload reader is defined below the header path that calls it.
[[nodiscard]] Result<FoundrySnapshot> parse_payload(std::string_view payload);

/// Step six, the CRC-32C over the payload, followed by the payload parse.
[[nodiscard]] Result<FoundrySnapshot> parse_image(std::string_view image) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(image.data());
  std::uint64_t payload_length = 0;
  for (unsigned index = 0; index < 8; ++index) {
    payload_length |= static_cast<std::uint64_t>(bytes[8 + index]) << (8 * index);
  }
  std::uint32_t expected_crc = 0;
  for (unsigned index = 0; index < 4; ++index) {
    expected_crc |= static_cast<std::uint32_t>(bytes[16 + index]) << (8 * index);
  }
  const std::string_view payload =
      image.substr(kSnapshotHeaderBytes, static_cast<std::size_t>(payload_length));
  const std::uint32_t actual_crc = crc32c(payload);
  if (actual_crc != expected_crc) {
    return Status(ErrorCode::PersistenceIntegrityMismatch,
                  "snapshot payload CRC-32C is " + hex32(actual_crc) +
                      " but the header declares " + hex32(expected_crc));
  }
  return parse_payload(payload);
}

// ---------------------------------------------------------------------------
// Semantic validation of a freshly parsed snapshot
// ---------------------------------------------------------------------------

/// One labelled value in a per-domain table. Longer values are truncated so a
/// diagnostic can never be dominated by one hostile string.
struct DiagnosticValue {
  const char* label;
  std::string value;
};

constexpr std::size_t kMaxDiagnosticValueLength = 256;

[[nodiscard]] std::string truncate_value(std::string value) {
  if (value.size() <= kMaxDiagnosticValueLength) {
    return value;
  }
  value.resize(kMaxDiagnosticValueLength);
  value.append("... (truncated)");
  return value;
}

void bump_identity_counter(std::array<std::uint32_t, kIdKindCount>& observed, IdKind kind,
                           std::uint64_t raw) {
  const std::size_t index = static_cast<std::size_t>(kind);
  if (index == 0 || index >= observed.size()) {
    return;
  }
  const auto counter = static_cast<std::uint32_t>(raw & 0xFFFFFFFFull);
  if (counter > observed[index]) {
    observed[index] = counter;
  }
}

template <typename Map>
Status require_no_null_identity(const Map& map, const char* domain) {
  for (const auto& entry : map) {
    if (!entry.first.valid()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    std::string(domain) + " holds the null identity");
    }
  }
  return Status();
}

std::array<std::uint32_t, kIdKindCount> collect_observed_identity_counters(
    const FoundrySnapshot& snapshot) {
  std::array<std::uint32_t, kIdKindCount> observed{};
  auto note_keys = [&observed](const auto& map) {
    for (const auto& entry : map) {
      bump_identity_counter(observed, std::remove_reference_t<decltype(map)>::key_type::tag_type::kind,
                            entry.first.raw());
    }
  };
  note_keys(snapshot.tasks);
  note_keys(snapshot.policies);
  note_keys(snapshot.populations);
  note_keys(snapshot.candidates);
  note_keys(snapshot.workers);
  note_keys(snapshot.attempts);
  note_keys(snapshot.assignments);
  note_keys(snapshot.evaluations);
  note_keys(snapshot.selections);
  note_keys(snapshot.retentions);
  note_keys(snapshot.promotion_requests);
  note_keys(snapshot.ledgers);
  note_keys(snapshot.lineage.nodes());
  // Identities that live inside records rather than in a table key: a worker
  // incarnation's boot identity and the producer of a candidate are minted by
  // the peer, so they are only visible here.
  for (const auto& entry : snapshot.workers) {
    bump_identity_counter(observed, IdKind::WorkerBoot, entry.second.boot.raw());
  }
  for (const auto& entry : snapshot.candidates) {
    bump_identity_counter(observed, IdKind::Worker, entry.second.producer_worker.raw());
    bump_identity_counter(observed, IdKind::WorkerBoot, entry.second.producer_boot.raw());
  }
  return observed;
}

Status require_population_ordering(const std::vector<CandidateId>& candidates,
                                   PopulationId population) {
  if (candidates.size() > static_cast<std::size_t>(kMaxCandidatesPerPopulation)) {
    return Status(ErrorCode::PersistenceRejectedContent,
                  "population " + population.to_string() + " lists " +
                      std::to_string(candidates.size()) + " candidates, beyond the " +
                      std::to_string(kMaxCandidatesPerPopulation) + " candidate ceiling");
  }
  for (std::size_t index = 1; index < candidates.size(); ++index) {
    if (!(candidates[index - 1] < candidates[index])) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "population " + population.to_string() +
                        " candidate list is not strictly ascending at position " +
                        std::to_string(index) + ": " + candidates[index - 1].to_string() +
                        " is followed by " + candidates[index].to_string());
    }
  }
  return Status();
}

Status require_all_equal(const char* domain, const char* key, std::string key_value,
                         const std::vector<DiagnosticValue>& values) {
  for (std::size_t index = 1; index < values.size(); ++index) {
    if (values[index].value != values[0].value) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    std::string(domain) + " " + std::string(key) + " " + key_value +
                        " disagrees with itself between two records: field '" +
                        values[0].label + "' holds '" + truncate_value(values[0].value) +
                        "' but field '" + values[index].label + "' holds '" +
                        truncate_value(values[index].value) + "'");
    }
  }
  return Status();
}

Status validate_semantics(const FoundrySnapshot& snapshot) {
  for (const auto& entry : snapshot.tasks) {
    AF_TRY(validate_task(entry.second));
  }
  for (const auto& entry : snapshot.policies) {
    AF_TRY(validate_policy(entry.second));
  }
  for (const auto& entry : snapshot.candidates) {
    AF_TRY(validate_candidate_record(entry.second));
  }
  for (const auto& entry : snapshot.evaluations) {
    AF_TRY(validate_evaluation_record(entry.second));
  }

  for (const auto& entry : snapshot.evaluations) {
    const EvaluationRecord& evaluation = entry.second;
    if (snapshot.candidates.find(evaluation.candidate) == snapshot.candidates.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "evaluation " + evaluation.id.to_string() + " names candidate " +
                        evaluation.candidate.to_string() +
                        ", which the snapshot does not hold");
    }
    if (snapshot.tasks.find(evaluation.task) == snapshot.tasks.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "evaluation " + evaluation.id.to_string() + " names task " +
                        evaluation.task.to_string() + ", which the snapshot does not hold");
    }
  }

  for (const auto& entry : snapshot.attempts) {
    const AttemptRecord& attempt = entry.second;
    if (snapshot.candidates.find(attempt.candidate) == snapshot.candidates.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "attempt " + attempt.id.to_string() + " names candidate " +
                        attempt.candidate.to_string() + ", which the snapshot does not hold");
    }
    if (snapshot.populations.find(attempt.population) == snapshot.populations.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "attempt " + attempt.id.to_string() + " names population " +
                        attempt.population.to_string() +
                        ", which the snapshot does not hold");
    }
    if (snapshot.tasks.find(attempt.task) == snapshot.tasks.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "attempt " + attempt.id.to_string() + " names task " +
                        attempt.task.to_string() + ", which the snapshot does not hold");
    }
  }

  for (const auto& entry : snapshot.assignments) {
    const AssignmentRecord& assignment = entry.second;
    if (snapshot.attempts.find(assignment.attempt) == snapshot.attempts.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "assignment " + assignment.id.to_string() + " names attempt " +
                        assignment.attempt.to_string() +
                        ", which the snapshot does not hold");
    }
  }

  for (const auto& entry : snapshot.selections) {
    if (snapshot.populations.find(entry.first) == snapshot.populations.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "selection decision names population " + entry.first.to_string() +
                        ", which the snapshot does not hold");
    }
    const SelectionDecision& decision = entry.second;
    if (decision.selected.valid() &&
        snapshot.candidates.find(decision.selected) == snapshot.candidates.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "selection decision for population " + entry.first.to_string() +
                        " selects candidate " + decision.selected.to_string() +
                        ", which the snapshot does not hold");
    }
  }

  for (const auto& entry : snapshot.retentions) {
    if (snapshot.populations.find(entry.first) == snapshot.populations.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "retention decision names population " + entry.first.to_string() +
                        ", which the snapshot does not hold");
    }
  }

  for (const auto& entry : snapshot.promotion_requests) {
    const PromotionRequest& request = entry.second;
    if (snapshot.candidates.find(request.candidate) == snapshot.candidates.end()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "promotion request " + request.id.to_string() + " names candidate " +
                        request.candidate.to_string() +
                        ", which the snapshot does not hold");
    }
  }

  for (const auto& entry : snapshot.populations) {
    AF_TRY(require_population_ordering(entry.second.candidates, entry.first));
  }

  const Status lineage_status = snapshot.lineage.validate();
  if (!lineage_status.ok()) {
    return Status(ErrorCode::PersistenceRejectedContent,
                  "restored lineage graph is not a valid DAG: " + lineage_status.message());
  }

  AF_TRY(require_no_null_identity(snapshot.tasks, "task table"));
  AF_TRY(require_no_null_identity(snapshot.policies, "policy table"));
  AF_TRY(require_no_null_identity(snapshot.populations, "population table"));
  AF_TRY(require_no_null_identity(snapshot.candidates, "candidate table"));
  AF_TRY(require_no_null_identity(snapshot.workers, "worker table"));
  AF_TRY(require_no_null_identity(snapshot.attempts, "attempt table"));
  AF_TRY(require_no_null_identity(snapshot.assignments, "assignment table"));
  AF_TRY(require_no_null_identity(snapshot.evaluations, "evaluation table"));
  AF_TRY(require_no_null_identity(snapshot.selections, "selection table"));
  AF_TRY(require_no_null_identity(snapshot.retentions, "retention table"));
  AF_TRY(require_no_null_identity(snapshot.promotion_requests, "promotion request table"));
  AF_TRY(require_no_null_identity(snapshot.ledgers, "ledger table"));
  AF_TRY(require_no_null_identity(snapshot.lineage.nodes(), "lineage table"));

  const std::array<std::uint32_t, kIdKindCount> observed =
      collect_observed_identity_counters(snapshot);

  for (std::size_t index = 0; index < observed.size(); ++index) {
    // Two classes of identity counter exist and they are validated differently.
    //
    // Coordinator-minted domains are allocated from the restored counter, so a
    // declared counter below the highest counter already present in the loaded
    // state would re-issue an identity that exists: that image is refused.
    //
    // Worker and worker-boot identities are minted by the peer, not by the
    // coordinator allocator, so id_counters[Worker] is legitimately zero while
    // the tables hold live peer-issued values, and comparing the two would
    // refuse every snapshot that contains a registered worker. Restore instead
    // seeds the allocator with the highest counter actually present in those
    // domains (see FoundryCore::recover), which prevents a future allocation
    // from colliding without rejecting a legal historical image.
    const IdKind counter_kind = static_cast<IdKind>(index);
    if (counter_kind == IdKind::Worker || counter_kind == IdKind::WorkerBoot) {
      continue;
    }
    if (observed[index] > snapshot.id_counters[index]) {
      const auto kind = static_cast<IdKind>(index);
      return Status(ErrorCode::PersistenceRejectedContent,
                    "snapshot identity counter for domain '" +
                        std::string(id_kind_name(kind)) + "' is " +
                        std::to_string(snapshot.id_counters[index]) +
                        " but loaded state already holds counter " +
                        std::to_string(observed[index]) +
                        ", so restoring it would re-issue an existing identity");
    }
  }

  return Status();
}

/// The payload record stream, in the fixed serialization order.
[[nodiscard]] std::string build_payload(const FoundrySnapshot& snapshot) {
  std::uint64_t record_count = 2;
  record_count += snapshot.tasks.size();
  record_count += snapshot.policies.size();
  record_count += snapshot.populations.size();
  record_count += snapshot.candidates.size();
  record_count += snapshot.workers.size();
  record_count += snapshot.attempts.size();
  record_count += snapshot.assignments.size();
  record_count += snapshot.evaluations.size();
  record_count += snapshot.selections.size();
  record_count += snapshot.retentions.size();
  record_count += snapshot.promotion_requests.size();
  record_count += snapshot.lineage.nodes().size();
  record_count += snapshot.ledgers.size();

  std::string payload;
  append_u32(payload, static_cast<std::uint32_t>(record_count));
  payload.append(frame_record(
      kRecordIdentityHeader, [&snapshot](Writer& w) { write_identity_header(w, snapshot); }));
  payload.append(frame_record(
      kRecordStatistics, [&snapshot](Writer& w) { write_statistics(w, snapshot.statistics); }));
  append_map_records(payload, snapshot.tasks, kRecordTask, write_task);
  append_map_records(payload, snapshot.policies, kRecordPolicy, write_policy);
  append_map_records(payload, snapshot.populations, kRecordPopulation, write_population);
  append_map_records(payload, snapshot.candidates, kRecordCandidate, write_candidate);
  append_map_records(payload, snapshot.workers, kRecordWorker, write_worker);
  append_map_records(payload, snapshot.attempts, kRecordAttempt, write_attempt);
  append_map_records(payload, snapshot.assignments, kRecordAssignment, write_assignment);
  append_map_records(payload, snapshot.evaluations, kRecordEvaluation, write_evaluation);
  append_map_records(payload, snapshot.selections, kRecordSelection, write_selection);
  append_map_records(payload, snapshot.retentions, kRecordRetention, write_retention);
  append_map_records(payload, snapshot.promotion_requests, kRecordPromotion, write_promotion);
  append_map_records(payload, snapshot.lineage.nodes(), kRecordLineageNode, write_lineage_node);
  append_map_records(payload, snapshot.ledgers, kRecordLedger, write_ledger);
  return payload;
}

// The payload reader is defined after the semantic validator it calls, but the
// file image reader above it needs the declaration.
// -- payload decoding -------------------------------------------------------

/// Insert one decoded record, refusing a duplicate identity inside the map.
template <typename Map, typename Record, typename Id>
Status store_record(Map& map, Record value, Id key, const char* domain) {
  const auto inserted = map.emplace(key, std::move(value));
  if (!inserted.second) {
    return Status(ErrorCode::PersistenceRejectedContent,
                  std::string("snapshot holds two ") + domain + " records for " +
                      key.to_string());
  }
  return Status();
}

struct TaskKey {
  using key_type = TaskId;
  [[nodiscard]] TaskId operator()(const TaskSpec& value) const noexcept { return value.id; }
};

struct PolicyKey {
  using key_type = PolicyId;
  [[nodiscard]] PolicyId operator()(const FoundryPolicy& value) const noexcept { return value.id; }
};

struct PopulationKey {
  using key_type = PopulationId;
  [[nodiscard]] PopulationId operator()(const PopulationRecord& value) const noexcept {
    return value.id;
  }
};

struct CandidateKey {
  using key_type = CandidateId;
  [[nodiscard]] CandidateId operator()(const CandidateRecord& value) const noexcept {
    return value.id;
  }
};

struct WorkerKey {
  using key_type = WorkerId;
  [[nodiscard]] WorkerId operator()(const WorkerRecord& value) const noexcept { return value.id; }
};

struct AttemptKey {
  using key_type = AttemptId;
  [[nodiscard]] AttemptId operator()(const AttemptRecord& value) const noexcept { return value.id; }
};

struct AssignmentKey {
  using key_type = AssignmentId;
  [[nodiscard]] AssignmentId operator()(const AssignmentRecord& value) const noexcept {
    return value.id;
  }
};

struct EvaluationKey {
  using key_type = EvaluationId;
  [[nodiscard]] EvaluationId operator()(const EvaluationRecord& value) const noexcept {
    return value.id;
  }
};

struct SelectionKey {
  using key_type = PopulationId;
  [[nodiscard]] PopulationId operator()(const SelectionDecision& value) const noexcept {
    return value.population;
  }
};

struct RetentionKey {
  using key_type = PopulationId;
  [[nodiscard]] PopulationId operator()(const RetentionDecision& value) const noexcept {
    return value.population;
  }
};

struct PromotionKey {
  using key_type = PromotionRequestId;
  [[nodiscard]] PromotionRequestId operator()(const PromotionRequest& value) const noexcept {
    return value.id;
  }
};

struct LineageKey {
  using key_type = CandidateId;
  [[nodiscard]] CandidateId operator()(const LineageNode& value) const noexcept {
    return value.candidate;
  }
};

[[nodiscard]] Result<FoundrySnapshot> parse_payload(std::string_view payload) {
  if (static_cast<std::uint64_t>(payload.size()) > kMaxSnapshotPayloadBytes) {
    return Status(ErrorCode::PersistenceTruncated,
                  "snapshot payload holds " + std::to_string(payload.size()) +
                      " bytes, beyond the " + std::to_string(kMaxSnapshotPayloadBytes) +
                      " byte snapshot payload ceiling");
  }

  Reader reader(payload);
  reader.set_field("payload");
  std::uint32_t record_count{}; AF_TRY_ASSIGN(record_count, (reader.count("record_count", 1u)));
  if (static_cast<std::uint64_t>(record_count) >
      static_cast<std::uint64_t>(kMaxSnapshotRecordsPerKind) * 16ull) {
    return Status(ErrorCode::PersistenceTruncated,
                  "snapshot declares " + std::to_string(record_count) +
                      " records, beyond the " +
                      std::to_string(static_cast<std::uint64_t>(kMaxSnapshotRecordsPerKind) * 16ull) +
                      " record ceiling");
  }

  FoundrySnapshot snapshot;
  std::unordered_map<CandidateId, LineageNode> lineage_nodes;

  for (std::uint32_t index = 0; index < record_count; ++index) {
    std::uint8_t record_type{}; AF_TRY_ASSIGN(record_type, reader.u8());
    std::uint32_t body_length{}; AF_TRY_ASSIGN(body_length, reader.u32());
    if (reader.remaining() < body_length) {
      return Status(ErrorCode::PersistenceTruncated,
                    "record " + std::to_string(index) + " of kind " +
                        std::to_string(record_type) + " declares " +
                        std::to_string(body_length) + " body bytes but only " +
                        std::to_string(reader.remaining()) + " remain");
    }
    std::string body{}; AF_TRY_ASSIGN(body, (reader.take_bytes(body_length, "record body")));
    Reader record(body);
    record.set_field("record " + std::to_string(index));

    switch (record_type) {
      case kRecordIdentityHeader: {
        AF_TRY_ASSIGN(snapshot.foundry, record.identity<FoundryId>("snapshot::foundry"));
        AF_TRY_ASSIGN(snapshot.run, record.identity<FoundryRunId>("snapshot::run"));
        AF_TRY_ASSIGN(snapshot.epoch, record.epoch());
        AF_TRY_ASSIGN(snapshot.revision, record.u64());
        AF_TRY_ASSIGN(snapshot.id_salt, record.u32());
        AF_TRY_ASSIGN(snapshot.id_counters, record.counters<kIdKindCount>());
        break;
      }
      case kRecordStatistics: {
        FoundryStatistics statistics;
        AF_TRY_ASSIGN(statistics.task_revisions, record.u64());
        AF_TRY_ASSIGN(statistics.policy_revisions, record.u64());
        AF_TRY_ASSIGN(statistics.populations_created, record.u64());
        AF_TRY_ASSIGN(statistics.populations_closed, record.u64());
        AF_TRY_ASSIGN(statistics.candidate_slots_created, record.u64());
        AF_TRY_ASSIGN(statistics.candidates_published, record.u64());
        AF_TRY_ASSIGN(statistics.attempts_authorized, record.u64());
        AF_TRY_ASSIGN(statistics.attempts_completed, record.u64());
        AF_TRY_ASSIGN(statistics.attempts_failed, record.u64());
        AF_TRY_ASSIGN(statistics.attempts_cancelled, record.u64());
        AF_TRY_ASSIGN(statistics.attempts_outcome_unknown, record.u64());
        AF_TRY_ASSIGN(statistics.evaluations_recorded, record.u64());
        AF_TRY_ASSIGN(statistics.selections_prepared, record.u64());
        AF_TRY_ASSIGN(statistics.selections_committed, record.u64());
        AF_TRY_ASSIGN(statistics.retentions_committed, record.u64());
        AF_TRY_ASSIGN(statistics.promotion_requests, record.u64());
        AF_TRY_ASSIGN(statistics.stale_authority_rejections, record.u64());
        AF_TRY_ASSIGN(statistics.duplicate_rejections, record.u64());
        AF_TRY_ASSIGN(statistics.late_rejections, record.u64());
        AF_TRY_ASSIGN(statistics.illegal_transition_rejections, record.u64());
        snapshot.statistics = statistics;
        break;
      }
      case kRecordTask: {
        TaskSpec value;
        AF_TRY_ASSIGN(value, read_task(record));
        const TaskId key = value.id;
        AF_TRY(store_record(snapshot.tasks, std::move(value), key, "task"));
        break;
      }
      case kRecordPolicy: {
        FoundryPolicy value;
        AF_TRY_ASSIGN(value, read_policy(record));
        const PolicyId key = value.id;
        AF_TRY(store_record(snapshot.policies, std::move(value), key, "policy"));
        break;
      }
      case kRecordPopulation: {
        PopulationRecord value;
        AF_TRY_ASSIGN(value, read_population(record));
        const PopulationId key = value.id;
        AF_TRY(store_record(snapshot.populations, std::move(value), key, "population"));
        break;
      }
      case kRecordCandidate: {
        CandidateRecord value;
        AF_TRY_ASSIGN(value, read_candidate(record));
        const CandidateId key = value.id;
        AF_TRY(store_record(snapshot.candidates, std::move(value), key, "candidate"));
        break;
      }
      case kRecordWorker: {
        WorkerRecord value;
        AF_TRY_ASSIGN(value, read_worker(record));
        const WorkerId key = value.id;
        AF_TRY(store_record(snapshot.workers, std::move(value), key, "worker"));
        break;
      }
      case kRecordAttempt: {
        AttemptRecord value;
        AF_TRY_ASSIGN(value, read_attempt(record));
        const AttemptId key = value.id;
        AF_TRY(store_record(snapshot.attempts, std::move(value), key, "attempt"));
        break;
      }
      case kRecordAssignment: {
        AssignmentRecord value;
        AF_TRY_ASSIGN(value, read_assignment(record));
        const AssignmentId key = value.id;
        AF_TRY(store_record(snapshot.assignments, std::move(value), key, "assignment"));
        break;
      }
      case kRecordEvaluation: {
        EvaluationRecord value;
        AF_TRY_ASSIGN(value, read_evaluation(record));
        const EvaluationId key = value.id;
        AF_TRY(store_record(snapshot.evaluations, std::move(value), key, "evaluation"));
        break;
      }
      case kRecordSelection: {
        SelectionDecision value;
        AF_TRY_ASSIGN(value, read_selection(record));
        const PopulationId key = value.population;
        AF_TRY(store_record(snapshot.selections, std::move(value), key, "selection decision"));
        break;
      }
      case kRecordRetention: {
        RetentionDecision value;
        AF_TRY_ASSIGN(value, read_retention(record));
        const PopulationId key = value.population;
        AF_TRY(store_record(snapshot.retentions, std::move(value), key, "retention decision"));
        break;
      }
      case kRecordPromotion: {
        PromotionRequest value;
        AF_TRY_ASSIGN(value, read_promotion(record));
        const PromotionRequestId key = value.id;
        AF_TRY(store_record(snapshot.promotion_requests, std::move(value), key, "promotion request"));
        break;
      }
      case kRecordLineageNode: {
        LineageNode value;
        AF_TRY_ASSIGN(value, read_lineage_node(record));
        const CandidateId key = value.candidate;
        AF_TRY(store_record(lineage_nodes, std::move(value), key, "lineage node"));
        break;
      }
      case kRecordLedger: {
        PopulationId ledger_key{};
        AF_TRY_ASSIGN(ledger_key,
                      record.identity<PopulationId>("BudgetLedger::population"));

        BudgetLedger value;
        AF_TRY_ASSIGN(value, read_ledger(ledger_key, record));
        AF_TRY(store_record(snapshot.ledgers, std::move(value), ledger_key, "ledger"));
        break;
      }
      default:
        return Status(ErrorCode::PersistenceRejectedContent,
                      "record " + std::to_string(index) + " carries unknown record type " +
                          std::to_string(record_type));
    }

    const Status consumed = record.require_exhausted();
    if (!consumed.ok()) {
      return Status(ErrorCode::PersistenceRejectedContent,
                    "record " + std::to_string(index) + " of kind " +
                        std::to_string(record_type) + " carries undeclared extra bytes: " +
                        consumed.message());
    }
  }

  const Status complete = reader.require_exhausted();
  if (!complete.ok()) {
    return Status(ErrorCode::PersistenceRejectedContent,
                  "snapshot payload carries bytes after its last record: " +
                      complete.message());
  }

  snapshot.lineage.restore(std::move(lineage_nodes));
  AF_TRY(validate_semantics(snapshot));
  return snapshot;
}

// ---------------------------------------------------------------------------
// Deep comparison
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxDifferenceValueLength = 200;

[[nodiscard]] std::string truncate_difference(std::string value) {
  if (value.size() <= kMaxDifferenceValueLength) {
    return value;
  }
  value.resize(kMaxDifferenceValueLength);
  value.append("...");
  return value;
}

/// Walk two maps in ascending identity order and report the first difference.
template <typename Map, typename RowEquals>
[[nodiscard]] std::optional<std::string> compare_map_entries(const Map& a, const Map& b,
                                                             const char* name,
                                                             RowEquals&& row_equals) {
  if (a.size() != b.size()) {
    return std::string(name) + " holds " + std::to_string(a.size()) +
           " records on the left but " + std::to_string(b.size()) + " on the right";
  }
  const auto keys = ascending_keys(a);
  for (const auto key : keys) {
    const auto left = a.find(key);
    const auto right = b.find(key);
    if (left == a.end() || right == b.end()) {
      return std::string(name) + " holds no record for " + key.to_string() + " on the right";
    }
    if (!row_equals(left->second, right->second)) {
      return std::string(name) + " record " + key.to_string() + " differs";
    }
  }
  return std::nullopt;
}

/// Element by element comparison of one vector of durable records.
template <typename Value>
[[nodiscard]] std::optional<std::string> compare_value_vectors(const std::vector<Value>& a,
                                                               const std::vector<Value>& b,
                                                               const char* name) {
  if (a.size() != b.size()) {
    return std::string(name) + " holds " + std::to_string(a.size()) +
           " entries on the left but " + std::to_string(b.size()) + " on the right";
  }
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (!(a[index] == b[index])) {
      return std::string(name) + " differs at position " + std::to_string(index);
    }
  }
  return std::nullopt;
}

template <typename Value>
[[nodiscard]] std::optional<std::string> compare_string_vectors(const std::vector<Value>& a,
                                                                const std::vector<Value>& b,
                                                                const char* name) {
  return compare_value_vectors(a, b, name);
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared with recovery
// ---------------------------------------------------------------------------

std::array<std::uint32_t, kIdKindCount> observed_identity_counters(
    const FoundrySnapshot& snapshot) {
  return collect_observed_identity_counters(snapshot);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<std::string> serialize_snapshot(const FoundrySnapshot& snapshot) {
  const std::string payload = build_payload(snapshot);
  const std::uint64_t payload_length = static_cast<std::uint64_t>(payload.size());
  if (payload_length > kMaxSnapshotPayloadBytes) {
    return Status(ErrorCode::LengthOutOfRange,
                  "snapshot payload encodes to " + std::to_string(payload_length) +
                      " bytes, beyond the " + std::to_string(kMaxSnapshotPayloadBytes) +
                      " byte snapshot payload ceiling");
  }
  const std::uint32_t payload_crc = crc32c(payload);

  std::string image;
  image.reserve(kSnapshotHeaderBytes + payload.size());
  image.append(kSnapshotMagic.data(), kSnapshotMagic.size());
  append_u32(image, kSnapshotFormatVersion);
  append_u64(image, payload_length);
  append_u32(image, payload_crc);
  append_u32(image, 0u);
  image.append(payload);
  return image;
}

Result<FoundrySnapshot> deserialize_snapshot(std::string_view image) {
  SnapshotHeader header;
  std::size_t total = 0;
  AF_TRY(validate_header(image, header, total));
  return parse_image(image);
}

Result<SnapshotHeader> parse_snapshot_header(std::string_view image) {
  SnapshotHeader header;
  std::size_t total = 0;
  AF_TRY(validate_header(image, header, total));
  return header;
}

SnapshotStore::SnapshotStore(std::filesystem::path path) : path_(std::move(path)) {}

bool SnapshotStore::exists() const {
  std::error_code error;
  return std::filesystem::exists(path_, error) && !error;
}

Status SnapshotStore::save(const FoundrySnapshot& snapshot) {
  std::string image;
  AF_TRY_ASSIGN(image, serialize_snapshot(snapshot));
  const Status written = atomic_write_file(path_, image);
  if (!written.ok()) {
    return written.with_context("snapshot store could not write '" + path_to_utf8(path_) + "'");
  }
  save_count_ += 1;
  return Status();
}

Result<FoundrySnapshot> SnapshotStore::load() const {
  std::string image{}; AF_TRY_ASSIGN(image, (read_file_bounded(path_, static_cast<std::uint64_t>(kSnapshotHeaderBytes) + kMaxSnapshotPayloadBytes)));
  return deserialize_snapshot(image);
}

Status SnapshotStore::save_and_verify(const FoundrySnapshot& snapshot) {
  AF_TRY(save(snapshot));
  FoundrySnapshot reloaded;
  AF_TRY_ASSIGN(reloaded, load());
  if (!snapshots_are_equal(snapshot, reloaded)) {
    return Status(ErrorCode::PersistenceRejectedContent,
                  "snapshot did not survive a save and reload round trip: " +
                      first_snapshot_difference(snapshot, reloaded));
  }
  return Status();
}

bool snapshots_are_equal(const FoundrySnapshot& a, const FoundrySnapshot& b) {
  if (!(a.foundry == b.foundry) || !(a.run == b.run) || !(a.epoch == b.epoch) ||
      a.revision != b.revision || a.id_salt != b.id_salt || a.id_counters != b.id_counters ||
      !(a.statistics == b.statistics)) {
    return false;
  }
  if (a.tasks != b.tasks || a.policies != b.policies || a.populations != b.populations ||
      a.candidates != b.candidates || a.workers != b.workers || a.attempts != b.attempts ||
      a.assignments != b.assignments || a.evaluations != b.evaluations ||
      a.selections != b.selections || a.retentions != b.retentions ||
      a.promotion_requests != b.promotion_requests) {
    return false;
  }
  for (const auto& entry : a.ledgers) {
    const BudgetLedger& other = b.ledgers.at(entry.first);
    if (!(entry.second.limits() == other.limits()) ||
        !(entry.second.counters() == other.counters())) {
      return false;
    }
  }
  return a.lineage.nodes() == b.lineage.nodes();
}

std::string first_snapshot_difference(const FoundrySnapshot& a, const FoundrySnapshot& b) {
  if (!(a.foundry == b.foundry)) {
    return "foundry identity differs";
  }
  if (!(a.run == b.run)) {
    return "foundry run identity differs";
  }
  if (!(a.epoch == b.epoch)) {
    return "coordinator epoch differs";
  }
  if (a.revision != b.revision) {
    return "revision differs";
  }
  if (a.id_salt != b.id_salt) {
    return "identity salt differs";
  }
  if (a.id_counters != b.id_counters) {
    for (std::size_t index = 0; index < a.id_counters.size(); ++index) {
      if (a.id_counters[index] != b.id_counters[index]) {
        return "identity counter for domain '" +
               std::string(id_kind_name(static_cast<IdKind>(index))) + "' is " +
               std::to_string(a.id_counters[index]) + " but " +
               std::to_string(b.id_counters[index]);
      }
    }
    return "identity counters differ";
  }
  if (!(a.statistics == b.statistics)) {
    return "foundry statistics differ";
  }

  const auto row_equal = [](const auto& left, const auto& right) { return left == right; };
  std::optional<std::string> difference =
      compare_map_entries(a.tasks, b.tasks, "task table", row_equal);
  if (!difference.has_value()) {
    difference = compare_map_entries(a.policies, b.policies, "policy table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.populations, b.populations, "population table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.candidates, b.candidates, "candidate table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.workers, b.workers, "worker table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.attempts, b.attempts, "attempt table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.assignments, b.assignments, "assignment table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.evaluations, b.evaluations, "evaluation table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.selections, b.selections, "selection table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.retentions, b.retentions, "retention table", row_equal);
  }
  if (!difference.has_value()) {
    difference = compare_map_entries(a.promotion_requests, b.promotion_requests,
                                     "promotion request table", row_equal);
  }
  if (!difference.has_value()) {
    difference =
        compare_map_entries(a.lineage.nodes(), b.lineage.nodes(), "lineage table", row_equal);
  }
  if (!difference.has_value()) {
  }
  if (difference.has_value()) {
    return *difference;
  }

  for (const auto& entry : a.tasks) {
    const TaskSpec& other = b.tasks.at(entry.first);
    difference = compare_map_entries(a.tasks, b.tasks, "task table", row_equal);
    const std::optional<std::string> inputs =
        compare_value_vectors(entry.second.inputs, other.inputs, "task inputs");
    if (inputs.has_value()) {
      return *inputs;
    }
    const std::optional<std::string> outputs = compare_string_vectors(
        entry.second.required_outputs, other.required_outputs, "task required outputs");
    if (outputs.has_value()) {
      return *outputs;
    }
    const std::optional<std::string> requirements = compare_value_vectors(
        entry.second.requirements, other.requirements, "task evaluation requirements");
    if (requirements.has_value()) {
      return *requirements;
    }
    const std::optional<std::string> constraints = compare_string_vectors(
        entry.second.hard_constraints, other.hard_constraints, "task hard constraints");
    if (constraints.has_value()) {
      return *constraints;
    }
    const std::optional<std::string> criteria = compare_string_vectors(
        entry.second.closure_criteria, other.closure_criteria, "task closure criteria");
    if (criteria.has_value()) {
      return *criteria;
    }
  }

  for (const auto& entry : a.policies) {
    const std::optional<std::string> factors =
        compare_value_vectors(entry.second.selection.factors,
                              b.policies.at(entry.first).selection.factors,
                              "policy ranking factors");
    if (factors.has_value()) {
      return *factors;
    }
  }

  for (const auto& entry : a.populations) {
    const PopulationRecord& other = b.populations.at(entry.first);
    const std::optional<std::string> candidates =
        compare_value_vectors(entry.second.candidates, other.candidates, "population candidates");
    if (candidates.has_value()) {
      return *candidates;
    }
    const std::optional<std::string> blockers = compare_string_vectors(
        entry.second.closure_blockers, other.closure_blockers, "population closure blockers");
    if (blockers.has_value()) {
      return *blockers;
    }
  }

  for (const auto& entry : a.candidates) {
    const CandidateRecord& other = b.candidates.at(entry.first);
    const std::optional<std::string> parents =
        compare_value_vectors(entry.second.parents, other.parents, "candidate parents");
    if (parents.has_value()) {
      return *parents;
    }
    const std::optional<std::string> artifacts =
        compare_value_vectors(entry.second.artifacts, other.artifacts, "candidate artifacts");
    if (artifacts.has_value()) {
      return *artifacts;
    }
  }

  for (const auto& entry : a.selections) {
    const SelectionDecision& other = b.selections.at(entry.first);
    const std::optional<std::string> ranking =
        compare_value_vectors(entry.second.ranking, other.ranking, "selection ranking");
    if (ranking.has_value()) {
      return *ranking;
    }
    const std::optional<std::string> excluded =
        compare_value_vectors(entry.second.excluded, other.excluded, "selection exclusions");
    if (excluded.has_value()) {
      return *excluded;
    }
    for (std::size_t index = 0; index < entry.second.ranking.size(); ++index) {
      const std::optional<std::string> factors =
          compare_value_vectors(entry.second.ranking[index].factors,
                                other.ranking[index].factors, "ranking entry factors");
      if (factors.has_value()) {
        return *factors;
      }
    }
  }

  for (const auto& entry : a.retentions) {
    const RetentionDecision& other = b.retentions.at(entry.first);
    const std::optional<std::string> entries =
        compare_value_vectors(entry.second.entries, other.entries, "retention entries");
    if (entries.has_value()) {
      return *entries;
    }
    const std::optional<std::string> retained =
        compare_value_vectors(entry.second.retained, other.retained, "retention retained list");
    if (retained.has_value()) {
      return *retained;
    }
    const std::optional<std::string> retired =
        compare_value_vectors(entry.second.retired, other.retired, "retention retired list");
    if (retired.has_value()) {
      return *retired;
    }
  }

  for (const auto& entry : a.promotion_requests) {
    const PromotionRequest& other = b.promotion_requests.at(entry.first);
    const std::optional<std::string> outstanding =
        compare_string_vectors(entry.second.outstanding_requirements,
                               other.outstanding_requirements,
                               "promotion outstanding requirements");
    if (outstanding.has_value()) {
      return *outstanding;
    }
    const std::optional<std::string> artifacts =
        compare_value_vectors(entry.second.artifacts, other.artifacts, "promotion artifacts");
    if (artifacts.has_value()) {
      return *artifacts;
    }
    const std::optional<std::string> evidence = compare_value_vectors(
        entry.second.mandatory_evidence, other.mandatory_evidence, "promotion mandatory evidence");
    if (evidence.has_value()) {
      return *evidence;
    }
    const std::optional<std::string> ancestry =
        compare_value_vectors(entry.second.ancestry, other.ancestry, "promotion ancestry");
    if (ancestry.has_value()) {
      return *ancestry;
    }
  }

  for (const auto& entry : a.lineage.nodes()) {
    const std::optional<std::string> parents =
        compare_value_vectors(entry.second.parents,
                              b.lineage.nodes().at(entry.first).parents, "lineage node parents");
    if (parents.has_value()) {
      return *parents;
    }
  }

  for (const auto& entry : a.ledgers) {
    const BudgetLedger& other = b.ledgers.at(entry.first);
    for (std::size_t index = 0; index < kBudgetKindCount; ++index) {
      const auto kind = static_cast<BudgetKind>(index);
      if (!(entry.second.counter(kind) == other.counter(kind))) {
        return "ledger for population " + entry.first.to_string() +
               " differs on budget kind '" + std::string(budget_kind_name(kind)) +
               "': reserved/consumed/released " +
               std::to_string(entry.second.counter(kind).reserved) + "/" +
               std::to_string(entry.second.counter(kind).consumed) + "/" +
               std::to_string(entry.second.counter(kind).released) + " versus " +
               std::to_string(other.counter(kind).reserved) + "/" +
               std::to_string(other.counter(kind).consumed) + "/" +
               std::to_string(other.counter(kind).released);
      }
    }
  }

  return "no difference found";
}

}  // namespace autonomous_foundry
