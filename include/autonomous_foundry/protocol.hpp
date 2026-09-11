#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/attempt.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/lineage.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/promotion.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/version.hpp"
#include "autonomous_foundry/worker.hpp"

// The framed wire protocol.
//
// Every frame carries magic, protocol version, message type, flags, a
// sequence number, a payload length and a CRC-32C over the payload. Payload
// length is bounded before allocation. Every mutating message carries enough
// authority to be rejected outright when it is stale, so a payload that is
// otherwise perfectly valid can never mutate current state on the strength of
// an old epoch, boot, session or generation.

namespace autonomous_foundry {

/// "AFY1" in ASCII.
inline constexpr std::uint32_t kFrameMagic = 0x41465931u;

/// Fixed frame header size in bytes.
inline constexpr std::size_t kFrameHeaderBytes = 28;

enum class MessageType : std::uint16_t {
  Invalid = 0,

  // session
  Hello = 1,
  HelloAck = 2,
  WorkerReady = 3,
  Revalidate = 4,
  RevalidateAck = 5,
  Heartbeat = 6,
  HeartbeatAck = 7,

  // control plane
  CreateTask = 10,
  TaskCreated = 11,
  DefinePolicy = 12,
  PolicyDefined = 13,
  CreatePopulation = 14,
  PopulationCreated = 15,
  StartPopulation = 16,
  PopulationStarted = 17,
  ClosePopulation = 18,
  PopulationClosed = 19,
  AdvancePopulation = 20,
  PopulationAdvanced = 21,
  RequestSelection = 22,
  SelectionDecisionMessage = 23,
  RequestRetention = 24,
  RetentionDecisionMessage = 25,
  RequestPromotion = 26,
  PromotionRequestMessage = 27,
  RequestRevalidation = 28,
  RevalidationApplied = 29,
  CancelAttemptRequest = 30,
  AttemptCancelled = 31,

  // data plane
  AssignAttempt = 40,
  AttemptAccepted = 41,
  AttemptResult = 42,
  CandidatePublish = 43,
  PublishAck = 44,
  AttemptFailed = 45,
  EvaluationReport = 46,
  SelfReport = 47,

  // query plane
  QueryPopulation = 60,
  PopulationDetail = 61,
  QueryCandidate = 62,
  CandidateDetail = 63,
  QueryLineage = 64,
  LineageDetail = 65,
  QueryStatistics = 66,
  StatisticsDetail = 67,
  QueryAudit = 68,
  AuditDetail = 69,

  // lifecycle
  ShutdownCoordinator = 80,
  ShutdownAck = 81,
  ErrorResponse = 90,
  ControlAck = 91,
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view message_type_name(
    MessageType type) noexcept;

enum class SessionRole : std::uint8_t {
  Worker = 0,
  Controller = 1,
  Observer = 2,
};

struct FrameHeader {
  std::uint32_t magic{kFrameMagic};
  std::uint16_t version{kProtocolVersion};
  MessageType type{MessageType::Invalid};
  std::uint32_t flags{0};
  std::uint64_t sequence{0};
  std::uint32_t payload_length{0};
  std::uint32_t payload_crc32c{0};
};

/// A parsed frame: header plus owned payload bytes.
struct Frame {
  FrameHeader header;
  std::string payload;

  [[nodiscard]] std::size_t encoded_size() const noexcept {
    return kFrameHeaderBytes + payload.size();
  }
};

/// Encode a header plus payload into a single contiguous buffer.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string encode_frame(const FrameHeader& header,
                                                              std::string_view payload);

/// Attempt to decode exactly one frame from a buffer.
///
/// consumed is set to the number of bytes taken from the front of the buffer.
/// Returns Incomplete through the Result when the buffer holds only part of a
/// frame; the caller then reads more bytes and retries. This is what makes
/// split reads and coalesced reads both correct.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<Frame> decode_frame(std::string_view buffer,
                                                                std::size_t* consumed);

/// True when the status says "read more bytes and try again".
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool status_is_incomplete(const Status& status) noexcept;

// ---------------------------------------------------------------------------
// Payload codec
// ---------------------------------------------------------------------------

class AUTONOMOUS_FOUNDRY_API ByteWriter {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void f64(double value);
  void boolean(bool value);
  void str(std::string_view value);
  void bytes(std::string_view value);

  template <typename Id>
  void identity(Id id) {
    u64(id.raw());
  }

  template <typename Tag>
  void generation(Generation<Tag> generation) {
    u32(generation.value());
  }

  void epoch(CoordinatorEpoch epoch) { u64(epoch.value()); }
  void salt(std::uint32_t value) { u32(value); }

  [[nodiscard]] const std::string& data() const noexcept { return buffer_; }
  [[nodiscard]] std::string take() noexcept { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::string buffer_;
};

class AUTONOMOUS_FOUNDRY_API ByteReader {
 public:
  explicit ByteReader(std::string_view data) noexcept : data_(data) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<std::int64_t> i64();
  [[nodiscard]] Result<double> f64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::string> str();
  [[nodiscard]] Result<std::string> bytes();

  template <typename Tag>
  [[nodiscard]] Result<StrongId<Tag>> identity(std::string_view field) {
    const Result<std::uint64_t> probe = u64();
    if (!probe.ok()) {
      return probe.status();
    }
    return strong_id_from_raw_checked<Tag>(probe.value(), field);
  }

  /// Identity that is allowed to be null (optional reference fields).
  template <typename Tag>
  [[nodiscard]] Result<StrongId<Tag>> optional_identity(std::string_view field) {
    const Result<std::uint64_t> probe = u64();
    if (!probe.ok()) {
      return probe.status();
    }
    if (probe.value() == 0) {
      return StrongId<Tag>::null();
    }
    return strong_id_from_raw_checked<Tag>(probe.value(), field);
  }

  template <typename Tag>
  [[nodiscard]] Result<Generation<Tag>> generation() {
    const Result<std::uint32_t> probe = u32();
    if (!probe.ok()) {
      return probe.status();
    }
    return Generation<Tag>::from_value(probe.value());
  }

  [[nodiscard]] Result<CoordinatorEpoch> epoch();

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ >= data_.size(); }

  /// Fail unless every byte has been consumed. Catches trailing garbage that a
  /// sender appended hoping it would be ignored.
  [[nodiscard]] Status require_exhausted() const;

 private:
  [[nodiscard]] Result<std::string_view> take(std::size_t count);

  std::string_view data_;
  std::size_t offset_{0};
};

// ---------------------------------------------------------------------------
// Message structures
// ---------------------------------------------------------------------------

struct HelloMessage {
  SessionRole role{SessionRole::Worker};
  FoundryRunId run;
  WorkerId worker;
  WorkerBootId boot;
  ControllerId controller;
  std::string label;
  std::string capability;
  std::uint32_t process_id{0};
  /// Epoch the sender believes is current. Zero for a first connection.
  CoordinatorEpoch observed_epoch;
};

struct HelloAckMessage {
  CoordinatorEpoch coordinator_epoch;
  FoundryRunId run;
  SessionId session;
  WorkerSessionGeneration session_generation;
  bool revalidation_required{false};
  std::string detail;
};

struct WorkerReadyMessage {
  WorkerSessionAuthority session;
  std::string capability;
};

struct RevalidateMessage {
  WorkerSessionAuthority session;
  std::string detail;
};

struct RevalidateAckMessage {
  WorkerSessionAuthority session;
  bool accepted{false};
  std::string detail;
};

struct HeartbeatMessage {
  WorkerSessionAuthority session;
  std::uint64_t sequence{0};
};

struct CreateTaskMessage {
  ControllerSessionAuthority session;
  TaskSpec task;
};

struct TaskCreatedMessage {
  TaskId task;
  TaskGeneration generation;
  std::string content_digest;
  std::string detail;
};

struct DefinePolicyMessage {
  ControllerSessionAuthority session;
  FoundryPolicy policy;
};

struct PolicyDefinedMessage {
  PolicyId policy;
  PolicyGeneration generation;
  std::string content_digest;
};

struct CreatePopulationMessage {
  ControllerSessionAuthority session;
  PopulationSpec spec;
};

struct PopulationCreatedMessage {
  PopulationId population;
  PopulationGeneration generation;
};

struct StartPopulationMessage {
  ControllerOperationAuthority authority;
};

struct PopulationStateMessage {
  PopulationId population;
  PopulationGeneration generation;
  PopulationState state{PopulationState::Created};
  std::string detail;
};

struct ClosePopulationMessage {
  ControllerOperationAuthority authority;
};

struct AdvancePopulationMessage {
  ControllerOperationAuthority authority;
  std::string next_name;
};

struct PopulationAdvancedMessage {
  PopulationId predecessor;
  PopulationId successor;
  PopulationGeneration successor_generation;
  CandidateId seed_candidate;
  CandidateGeneration seed_candidate_generation;
  std::string detail;
};

struct RequestSelectionMessage {
  ControllerOperationAuthority authority;
};

struct RequestRetentionMessage {
  ControllerOperationAuthority authority;
};

struct RequestPromotionMessage {
  ControllerOperationAuthority authority;
};

struct RequestRevalidationMessage {
  ControllerOperationAuthority authority;
  std::string reason;
};

struct CancelAttemptRequestMessage {
  ControllerOperationAuthority authority;
  AttemptId attempt;
  AttemptGeneration attempt_generation;
  std::string reason;
};

struct AttemptCancelledMessage {
  AttemptId attempt;
  AttemptState state{AttemptState::Cancelled};
  std::string detail;
};

struct AssignAttemptMessage {
  WorkerSessionAuthority session;
  AttemptPackage package;
};

struct AttemptAcceptedMessage {
  WorkerOperationAuthority authority;
};

struct AttemptResultMessage {
  WorkerOperationAuthority authority;
  std::string detail;
  std::uint64_t duration_micros{0};
};

struct CandidatePublishMessage {
  WorkerOperationAuthority authority;
  std::vector<ArtifactRef> artifacts;
  std::string declared_strategy;
  /// The worker's own claim. Recorded, never authoritative.
  bool self_reported_success{false};
  std::string self_report_detail;
};

struct PublishAckMessage {
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  bool accepted{false};
  std::string detail;
};

struct AttemptFailedMessage {
  WorkerOperationAuthority authority;
  std::string reason;
};

struct EvaluationReportMessage {
  EvaluationRecord record;
};

struct SelfReportMessage {
  WorkerOperationAuthority authority;
  std::string claim;
  bool claimed_success{false};
};

struct QueryPopulationMessage {
  ControllerSessionAuthority session;
  PopulationId population;
};

struct PopulationDetailMessage {
  PopulationRecord population;
  std::uint32_t committed_selection_generation{0};
  bool retention_committed{false};
  std::uint64_t candidate_count{0};
  std::vector<std::string> closure_blockers;
};

struct QueryCandidateMessage {
  ControllerSessionAuthority session;
  CandidateId candidate;
};

struct CandidateDetailMessage {
  CandidateRecord candidate;
  std::vector<EvaluationRecord> evaluations;
};

struct QueryLineageMessage {
  ControllerSessionAuthority session;
  CandidateId root;
};

struct LineageDetailMessage {
  std::vector<LineageNode> nodes;
};

struct QueryStatisticsMessage {
  ControllerSessionAuthority session;
};

struct StatisticsDetailMessage {
  FoundryStatistics statistics;
  std::uint64_t revision{0};
  CoordinatorEpoch epoch;
  FoundryRunId run;
};

struct QueryAuditMessage {
  ControllerSessionAuthority session;
};

struct AuditDetailMessage {
  std::vector<std::string> violations;
};

struct SelectionDecisionMessage {
  SelectionDecision decision;
  bool committed{false};
};

struct RetentionDecisionMessage {
  RetentionDecision decision;
};

struct PromotionRequestMessage {
  PromotionRequest request;
  PromotionReceipt receipt;
};

struct ShutdownCoordinatorMessage {
  ControllerSessionAuthority session;
};

struct ControlAckMessage {
  bool ok{true};
  ErrorCode code{ErrorCode::Ok};
  std::string detail;
};

struct ErrorResponseMessage {
  ErrorCode code{ErrorCode::Internal};
  std::string detail;
  MessageType in_reply_to{MessageType::Invalid};
};

struct RevalidationAppliedMessage {
  PopulationId population;
  PopulationGeneration generation;
  PopulationState state{PopulationState::Created};
  std::string detail;
};

struct Message {
  MessageType type{MessageType::Invalid};
  std::uint64_t sequence{0};
  std::string payload;
};

// -- encode/decode ---------------------------------------------------------

[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const HelloMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const HelloAckMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const WorkerReadyMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RevalidateMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RevalidateAckMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const HeartbeatMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const CreateTaskMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const TaskCreatedMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const DefinePolicyMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const PolicyDefinedMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const CreatePopulationMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const PopulationCreatedMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const StartPopulationMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const PopulationStateMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const ClosePopulationMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const AdvancePopulationMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const PopulationAdvancedMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RequestSelectionMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RequestRetentionMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RequestPromotionMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RequestRevalidationMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const CancelAttemptRequestMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const AttemptCancelledMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const AssignAttemptMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const AttemptAcceptedMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const AttemptResultMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const CandidatePublishMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const PublishAckMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const AttemptFailedMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const EvaluationReportMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const SelfReportMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const QueryPopulationMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const PopulationDetailMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const QueryCandidateMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const CandidateDetailMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const QueryLineageMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const LineageDetailMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const QueryStatisticsMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const StatisticsDetailMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const QueryAuditMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const AuditDetailMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const SelectionDecisionMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RetentionDecisionMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const PromotionRequestMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const ShutdownCoordinatorMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const ControlAckMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const ErrorResponseMessage& message);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_payload(
    const RevalidationAppliedMessage& message);

[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<HelloMessage> decode_hello(std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<HelloAckMessage> decode_hello_ack(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<WorkerReadyMessage> decode_worker_ready(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RevalidateMessage> decode_revalidate(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RevalidateAckMessage> decode_revalidate_ack(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<HeartbeatMessage> decode_heartbeat(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<CreateTaskMessage> decode_create_task(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<TaskCreatedMessage> decode_task_created(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<DefinePolicyMessage> decode_define_policy(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PolicyDefinedMessage> decode_policy_defined(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<CreatePopulationMessage> decode_create_population(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PopulationCreatedMessage> decode_population_created(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<StartPopulationMessage> decode_start_population(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PopulationStateMessage> decode_population_state(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ClosePopulationMessage> decode_close_population(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<AdvancePopulationMessage> decode_advance_population(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PopulationAdvancedMessage> decode_population_advanced(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RequestSelectionMessage> decode_request_selection(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RequestRetentionMessage> decode_request_retention(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RequestPromotionMessage> decode_request_promotion(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RequestRevalidationMessage> decode_request_revalidation(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<CancelAttemptRequestMessage>
decode_cancel_attempt_request(std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<AttemptCancelledMessage> decode_attempt_cancelled(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<AssignAttemptMessage> decode_assign_attempt(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<AttemptAcceptedMessage> decode_attempt_accepted(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<AttemptResultMessage> decode_attempt_result(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<CandidatePublishMessage> decode_candidate_publish(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PublishAckMessage> decode_publish_ack(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<AttemptFailedMessage> decode_attempt_failed(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<EvaluationReportMessage> decode_evaluation_report(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<SelfReportMessage> decode_self_report(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<QueryPopulationMessage> decode_query_population(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PopulationDetailMessage> decode_population_detail(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<QueryCandidateMessage> decode_query_candidate(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<CandidateDetailMessage> decode_candidate_detail(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<QueryLineageMessage> decode_query_lineage(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<LineageDetailMessage> decode_lineage_detail(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<QueryStatisticsMessage> decode_query_statistics(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<StatisticsDetailMessage> decode_statistics_detail(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<QueryAuditMessage> decode_query_audit(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<AuditDetailMessage> decode_audit_detail(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<SelectionDecisionMessage> decode_selection_decision(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RetentionDecisionMessage> decode_retention_decision(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PromotionRequestMessage> decode_promotion_request(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ShutdownCoordinatorMessage> decode_shutdown_coordinator(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ControlAckMessage> decode_control_ack(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ErrorResponseMessage> decode_error_response(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RevalidationAppliedMessage>
decode_revalidation_applied(std::string_view payload);

// Shared sub-encoders used by several messages.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_evaluation_record(
    const EvaluationRecord& record);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<EvaluationRecord> decode_evaluation_record(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_selection_decision(
    const SelectionDecision& decision);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<SelectionDecision> decode_selection_decision_value(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_retention_decision(
    const RetentionDecision& decision);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<RetentionDecision> decode_retention_decision_value(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_promotion_request_value(
    const PromotionRequest& request);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PromotionRequest> decode_promotion_request_value(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_promotion_receipt(
    const PromotionReceipt& receipt);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<PromotionReceipt> decode_promotion_receipt(
    std::string_view payload);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_worker_session_authority(
    const WorkerSessionAuthority& authority);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<WorkerSessionAuthority> decode_worker_session_authority(
    ByteReader& reader);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_worker_operation_authority(
    const WorkerOperationAuthority& authority);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<WorkerOperationAuthority> decode_worker_operation_authority(
    ByteReader& reader);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_controller_session_authority(
    const ControllerSessionAuthority& authority);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ControllerSessionAuthority>
decode_controller_session_authority(ByteReader& reader);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> encode_controller_operation_authority(
    const ControllerOperationAuthority& authority);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ControllerOperationAuthority>
decode_controller_operation_authority(ByteReader& reader);

}  // namespace autonomous_foundry
