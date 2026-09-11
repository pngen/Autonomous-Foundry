// Protocol suite.
//
// Proves the framed wire format: the header survives encode/decode for every
// covered message type, each payload round trips through its matching decoder
// with the decoded value re-encoding to the exact bytes that arrived, a frame
// split across three reads is reassembled, three coalesced frames decode in
// order with the right consumed counts, each frame rejection carries its own
// code, a decoder refuses trailing bytes, and an identity smuggled into a
// payload with the wrong domain tag is refused rather than accepted.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/limits.hpp"
#include "autonomous_foundry/protocol.hpp"
#include "test_support.hpp"

using namespace autonomous_foundry;  // NOLINT(google-build-using-namespace)

namespace {

// ---------------------------------------------------------------------------
// Little endian frame surgery
// ---------------------------------------------------------------------------

unsigned byte_shift(std::size_t index) {
  return static_cast<unsigned>(8u * static_cast<unsigned>(index));
}

void write_u16_le(std::string& buffer, std::size_t offset, std::uint16_t value) {
  for (std::size_t index = 0; index < 2; ++index) {
    const auto byte = static_cast<unsigned char>((value >> byte_shift(index)) & 0xFFu);
    buffer[offset + index] = static_cast<char>(byte);
  }
}

void write_u32_le(std::string& buffer, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    const auto byte = static_cast<unsigned char>((value >> byte_shift(index)) & 0xFFu);
    buffer[offset + index] = static_cast<char>(byte);
  }
}

[[nodiscard]] std::uint32_t read_u32_le(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const auto byte =
        static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index]));
    value |= byte << byte_shift(index);
  }
  return value;
}

// ---------------------------------------------------------------------------
// Identity and authority values
//
// Every identity carries its own domain tag in its high byte, so a payload
// built here is accepted only when the decoder checks that tag.
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint64_t raw_identity(IdKind kind, std::uint32_t counter) {
  return (static_cast<std::uint64_t>(kind) << 56) |
         (static_cast<std::uint64_t>(0x5Au) << 32) | static_cast<std::uint64_t>(counter);
}

[[nodiscard]] WorkerSessionAuthority session_authority() {
  WorkerSessionAuthority authority;
  authority.coordinator_epoch = CoordinatorEpoch::from_value(7u);
  authority.run = FoundryRunId::from_raw(raw_identity(IdKind::FoundryRun, 1u));
  authority.worker = WorkerId::from_raw(raw_identity(IdKind::Worker, 2u));
  authority.boot = WorkerBootId::from_raw(raw_identity(IdKind::WorkerBoot, 3u));
  authority.session = SessionId::from_raw(raw_identity(IdKind::Session, 4u));
  authority.session_generation = WorkerSessionGeneration::from_value(2u);
  return authority;
}

[[nodiscard]] WorkerOperationAuthority operation_authority() {
  WorkerOperationAuthority authority;
  authority.session = session_authority();
  authority.population = PopulationId::from_raw(raw_identity(IdKind::Population, 5u));
  authority.population_generation = PopulationGeneration::from_value(2u);
  authority.task = TaskId::from_raw(raw_identity(IdKind::Task, 6u));
  authority.task_generation = TaskGeneration::first();
  authority.candidate = CandidateId::from_raw(raw_identity(IdKind::Candidate, 7u));
  authority.candidate_generation = CandidateGeneration::from_value(3u);
  authority.attempt = AttemptId::from_raw(raw_identity(IdKind::Attempt, 8u));
  authority.attempt_generation = AttemptGeneration::first();
  authority.assignment = AssignmentId::from_raw(raw_identity(IdKind::Assignment, 9u));
  return authority;
}

[[nodiscard]] ControllerOperationAuthority controller_operation() {
  ControllerOperationAuthority authority;
  authority.session.coordinator_epoch = CoordinatorEpoch::from_value(7u);
  authority.session.run = FoundryRunId::from_raw(raw_identity(IdKind::FoundryRun, 1u));
  authority.session.controller = ControllerId::from_raw(raw_identity(IdKind::Controller, 10u));
  authority.session.session = SessionId::from_raw(raw_identity(IdKind::Session, 11u));
  authority.session.session_generation = WorkerSessionGeneration::from_value(1u);
  authority.population = PopulationId::from_raw(raw_identity(IdKind::Population, 5u));
  authority.population_generation = PopulationGeneration::from_value(2u);
  authority.task = TaskId::from_raw(raw_identity(IdKind::Task, 6u));
  authority.task_generation = TaskGeneration::first();
  authority.policy = PolicyId::from_raw(raw_identity(IdKind::Policy, 12u));
  authority.policy_generation = PolicyGeneration::first();
  return authority;
}

[[nodiscard]] EvaluationRecord evaluation_record() {
  EvaluationRecord record;
  record.id = EvaluationId::from_raw(raw_identity(IdKind::Evaluation, 13u));
  record.generation = EvaluationGeneration::first();
  record.candidate = CandidateId::from_raw(raw_identity(IdKind::Candidate, 7u));
  record.candidate_generation = CandidateGeneration::from_value(3u);
  record.task = TaskId::from_raw(raw_identity(IdKind::Task, 6u));
  record.task_generation = TaskGeneration::first();
  record.population = PopulationId::from_raw(raw_identity(IdKind::Population, 5u));
  record.population_generation = PopulationGeneration::from_value(2u);
  record.evaluator = EvaluatorId::from_raw(raw_identity(IdKind::Evaluator, 14u));
  record.evaluator_key = "compile-and-run";
  record.kind = EvaluatorKind::ProcessCommand;
  record.requirement_class = RequirementClass::Mandatory;
  record.outcome = EvaluationOutcome::Pass;
  record.complete = true;
  record.has_score = true;
  record.score = 1.5;
  record.decided_epoch = CoordinatorEpoch::from_value(7u);
  record.diagnostics = "exit status zero";
  record.evidence_digest = std::string(static_cast<std::size_t>(64), 'c');
  record.duration_micros = 1234u;
  return record;
}

/// One value of every message shape this suite covers.
struct MessageSamples {
  HelloMessage hello;
  HelloAckMessage hello_ack;
  WorkerReadyMessage worker_ready;
  HeartbeatMessage heartbeat;
  TaskCreatedMessage task_created;
  PolicyDefinedMessage policy_defined;
  PopulationCreatedMessage population_created;
  PopulationStateMessage population_state;
  PublishAckMessage publish_ack;
  AttemptAcceptedMessage attempt_accepted;
  AttemptFailedMessage attempt_failed;
  EvaluationReportMessage evaluation_report;
  SelfReportMessage self_report;
  RequestSelectionMessage request_selection;
  ControlAckMessage control_ack;
  ErrorResponseMessage error_response;
  RevalidationAppliedMessage revalidation;
};

[[nodiscard]] MessageSamples make_samples() {
  const WorkerSessionAuthority session = session_authority();
  const WorkerOperationAuthority operation = operation_authority();

  MessageSamples samples;
  samples.hello.role = SessionRole::Worker;
  samples.hello.run = session.run;
  samples.hello.worker = session.worker;
  samples.hello.boot = session.boot;
  samples.hello.controller = ControllerId::from_raw(raw_identity(IdKind::Controller, 10u));
  samples.hello.label = "worker-1";
  samples.hello.capability = "reference";
  samples.hello.process_id = 4242u;
  samples.hello.observed_epoch = CoordinatorEpoch::from_value(7u);

  samples.hello_ack.coordinator_epoch = CoordinatorEpoch::from_value(7u);
  samples.hello_ack.run = session.run;
  samples.hello_ack.session = session.session;
  samples.hello_ack.session_generation = session.session_generation;
  samples.hello_ack.revalidation_required = false;
  samples.hello_ack.detail = "session established";

  samples.worker_ready.session = session;
  samples.worker_ready.capability = "reference";

  samples.heartbeat.session = session;
  samples.heartbeat.sequence = 991u;

  samples.task_created.task = operation.task;
  samples.task_created.generation = TaskGeneration::first();
  samples.task_created.content_digest = std::string(static_cast<std::size_t>(64), 'a');
  samples.task_created.detail = "task defined";

  samples.policy_defined.policy = PolicyId::from_raw(raw_identity(IdKind::Policy, 12u));
  samples.policy_defined.generation = PolicyGeneration::first();
  samples.policy_defined.content_digest = std::string(static_cast<std::size_t>(64), 'b');

  samples.population_created.population = operation.population;
  samples.population_created.generation = PopulationGeneration::first();

  samples.population_state.population = operation.population;
  samples.population_state.generation = PopulationGeneration::from_value(2u);
  samples.population_state.state = PopulationState::Running;
  samples.population_state.detail = "population running";

  samples.publish_ack.candidate = operation.candidate;
  samples.publish_ack.candidate_generation = operation.candidate_generation;
  samples.publish_ack.accepted = true;
  samples.publish_ack.detail = "candidate accepted";

  samples.attempt_accepted.authority = operation;

  samples.attempt_failed.authority = operation;
  samples.attempt_failed.reason = "the evaluator reported a failed gate";

  samples.evaluation_report.record = evaluation_record();

  samples.self_report.authority = operation;
  samples.self_report.claim = "the source compiles and exits zero";
  samples.self_report.claimed_success = true;

  samples.request_selection.authority = controller_operation();

  samples.control_ack.ok = false;
  samples.control_ack.code = ErrorCode::StalePopulationGeneration;
  samples.control_ack.detail = "the population generation moved";

  samples.error_response.code = ErrorCode::FrameTooLarge;
  samples.error_response.detail = "the declared payload exceeds the accepted maximum";
  samples.error_response.in_reply_to = MessageType::CandidatePublish;

  samples.revalidation.population = operation.population;
  samples.revalidation.generation = PopulationGeneration::from_value(3u);
  samples.revalidation.state = PopulationState::Running;
  samples.revalidation.detail = "revalidation applied";
  return samples;
}

/// The header the encoder is asked to frame. encode_frame derives the payload
/// length and the payload CRC from the payload itself, so the values written
/// here must agree with what decode_frame reports.
[[nodiscard]] FrameHeader frame_header(MessageType type, std::uint64_t sequence,
                                       std::uint32_t flags, std::string_view payload) {
  FrameHeader header;
  header.magic = kFrameMagic;
  header.version = kProtocolVersion;
  header.type = type;
  header.flags = flags;
  header.sequence = sequence;
  header.payload_length = static_cast<std::uint32_t>(payload.size());
  header.payload_crc32c = crc32c(payload);
  return header;
}

/// Encode one message, frame it, decode the frame and the payload again, and
/// prove the decoded value re-encodes to exactly the bytes that arrived.
template <typename Message, typename Decode>
void expect_round_trip(const af_test::TestContext& context, MessageType type,
                       std::uint64_t sequence, const Message& message, Decode decode) {
  const std::string payload = af_test::require_value(context, encode_payload(message),
                                                     "encode_payload", __FILE__, __LINE__);
  EXPECT_FALSE(context, payload.empty());

  const FrameHeader header = frame_header(type, sequence, 0x5u, payload);
  const std::string buffer = encode_frame(header, payload);
  EXPECT_EQ(context, buffer.size(), kFrameHeaderBytes + payload.size());

  std::size_t consumed = 0;
  const Result<Frame> decoded = decode_frame(buffer, &consumed);
  af_test::require_ok(context, decoded.status(), "decode_frame", __FILE__, __LINE__);
  const Frame& frame = decoded.value();
  EXPECT_EQ(context, consumed, buffer.size());
  EXPECT_EQ(context, frame.header.magic, kFrameMagic);
  EXPECT_EQ(context, frame.header.version, kProtocolVersion);
  EXPECT_EQ(context, static_cast<std::uint16_t>(frame.header.type),
            static_cast<std::uint16_t>(type));
  EXPECT_EQ(context, frame.header.flags, std::uint32_t{0x5});
  EXPECT_EQ(context, frame.header.sequence, sequence);
  EXPECT_EQ(context, frame.header.payload_length, static_cast<std::uint32_t>(payload.size()));
  EXPECT_EQ(context, frame.header.payload_crc32c, crc32c(payload));
  EXPECT_EQ(context, frame.payload, payload);

  const Message value = af_test::require_value(context, decode(payload), "decode_payload",
                                               __FILE__, __LINE__);
  const std::string reencoded = af_test::require_value(
      context, encode_payload(value), "encode_payload(decoded)", __FILE__, __LINE__);
  EXPECT_EQ(context, reencoded, payload);
}

}  // namespace

AF_TEST_CASE(protocol, frame_round_trip_for_seventeen_message_types) {
  af_ctx.phase("SETUP");
  const MessageSamples samples = make_samples();

  af_ctx.phase("DISPATCH");
  expect_round_trip(af_ctx, MessageType::Hello, 1u, samples.hello, decode_hello);
  expect_round_trip(af_ctx, MessageType::HelloAck, 2u, samples.hello_ack, decode_hello_ack);
  expect_round_trip(af_ctx, MessageType::WorkerReady, 3u, samples.worker_ready,
                    decode_worker_ready);
  expect_round_trip(af_ctx, MessageType::Heartbeat, 4u, samples.heartbeat, decode_heartbeat);
  expect_round_trip(af_ctx, MessageType::TaskCreated, 5u, samples.task_created,
                    decode_task_created);
  expect_round_trip(af_ctx, MessageType::PolicyDefined, 6u, samples.policy_defined,
                    decode_policy_defined);
  expect_round_trip(af_ctx, MessageType::PopulationCreated, 7u, samples.population_created,
                    decode_population_created);
  // A population state response travels under the transition that produced it;
  // the payload codec itself is independent of the header type.
  expect_round_trip(af_ctx, MessageType::PopulationStarted, 8u, samples.population_state,
                    decode_population_state);
  expect_round_trip(af_ctx, MessageType::PublishAck, 9u, samples.publish_ack, decode_publish_ack);
  expect_round_trip(af_ctx, MessageType::AttemptAccepted, 10u, samples.attempt_accepted,
                    decode_attempt_accepted);
  expect_round_trip(af_ctx, MessageType::AttemptFailed, 11u, samples.attempt_failed,
                    decode_attempt_failed);
  expect_round_trip(af_ctx, MessageType::EvaluationReport, 12u, samples.evaluation_report,
                    decode_evaluation_report);
  expect_round_trip(af_ctx, MessageType::SelfReport, 13u, samples.self_report,
                    decode_self_report);
  expect_round_trip(af_ctx, MessageType::RequestSelection, 14u, samples.request_selection,
                    decode_request_selection);
  expect_round_trip(af_ctx, MessageType::ControlAck, 15u, samples.control_ack,
                    decode_control_ack);
  expect_round_trip(af_ctx, MessageType::ErrorResponse, 16u, samples.error_response,
                    decode_error_response);
  expect_round_trip(af_ctx, MessageType::RevalidationApplied, 17u, samples.revalidation,
                    decode_revalidation_applied);

  af_ctx.phase("VERIFY");
  EXPECT_EQ(af_ctx, message_type_name(MessageType::Heartbeat), std::string_view("Heartbeat"));
  EXPECT_EQ(af_ctx, message_type_name(MessageType::RevalidationApplied),
            std::string_view("RevalidationApplied"));

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(protocol, decoded_values_match_their_originals) {
  af_ctx.phase("SETUP");
  const MessageSamples samples = make_samples();

  af_ctx.phase("DISPATCH");
  REQUIRE_VALUE(af_ctx, std::string, hello_payload, encode_payload(samples.hello));
  const Result<HelloMessage> hello = decode_hello(hello_payload);
  EXPECT_OK(af_ctx, hello);
  EXPECT_EQ(af_ctx, hello.value().role, SessionRole::Worker);
  EXPECT_EQ(af_ctx, hello.value().run, samples.hello.run);
  EXPECT_EQ(af_ctx, hello.value().worker, samples.hello.worker);
  EXPECT_EQ(af_ctx, hello.value().boot, samples.hello.boot);
  EXPECT_EQ(af_ctx, hello.value().controller, samples.hello.controller);
  EXPECT_EQ(af_ctx, hello.value().label, samples.hello.label);
  EXPECT_EQ(af_ctx, hello.value().capability, samples.hello.capability);
  EXPECT_EQ(af_ctx, hello.value().process_id, samples.hello.process_id);
  EXPECT_EQ(af_ctx, hello.value().observed_epoch, samples.hello.observed_epoch);

  REQUIRE_VALUE(af_ctx, std::string, ack_payload, encode_payload(samples.hello_ack));
  const Result<HelloAckMessage> ack = decode_hello_ack(ack_payload);
  EXPECT_OK(af_ctx, ack);
  EXPECT_EQ(af_ctx, ack.value().coordinator_epoch, samples.hello_ack.coordinator_epoch);
  EXPECT_EQ(af_ctx, ack.value().run, samples.hello_ack.run);
  EXPECT_EQ(af_ctx, ack.value().session, samples.hello_ack.session);
  EXPECT_EQ(af_ctx, ack.value().session_generation, samples.hello_ack.session_generation);
  EXPECT_EQ(af_ctx, ack.value().revalidation_required, samples.hello_ack.revalidation_required);
  EXPECT_EQ(af_ctx, ack.value().detail, samples.hello_ack.detail);

  REQUIRE_VALUE(af_ctx, std::string, ready_payload, encode_payload(samples.worker_ready));
  const Result<WorkerReadyMessage> ready = decode_worker_ready(ready_payload);
  EXPECT_OK(af_ctx, ready);
  EXPECT_EQ(af_ctx, ready.value().session, samples.worker_ready.session);
  EXPECT_EQ(af_ctx, ready.value().capability, samples.worker_ready.capability);

  REQUIRE_VALUE(af_ctx, std::string, state_payload, encode_payload(samples.population_state));
  const Result<PopulationStateMessage> state = decode_population_state(state_payload);
  EXPECT_OK(af_ctx, state);
  EXPECT_EQ(af_ctx, state.value().population, samples.population_state.population);
  EXPECT_EQ(af_ctx, state.value().generation, samples.population_state.generation);
  EXPECT_EQ(af_ctx, state.value().state, PopulationState::Running);
  EXPECT_EQ(af_ctx, state.value().detail, samples.population_state.detail);

  REQUIRE_VALUE(af_ctx, std::string, publish_payload, encode_payload(samples.publish_ack));
  const Result<PublishAckMessage> publish = decode_publish_ack(publish_payload);
  EXPECT_OK(af_ctx, publish);
  EXPECT_EQ(af_ctx, publish.value().candidate, samples.publish_ack.candidate);
  EXPECT_EQ(af_ctx, publish.value().candidate_generation, samples.publish_ack.candidate_generation);
  EXPECT_EQ(af_ctx, publish.value().accepted, samples.publish_ack.accepted);
  EXPECT_EQ(af_ctx, publish.value().detail, samples.publish_ack.detail);

  REQUIRE_VALUE(af_ctx, std::string, evaluation_payload,
                encode_payload(samples.evaluation_report));
  const Result<EvaluationReportMessage> evaluation = decode_evaluation_report(evaluation_payload);
  EXPECT_OK(af_ctx, evaluation);
  EXPECT_EQ(af_ctx, evaluation.value().record, samples.evaluation_report.record);
  EXPECT_EQ(af_ctx, evaluation.value().record.score, 1.5);

  REQUIRE_VALUE(af_ctx, std::string, control_payload, encode_payload(samples.control_ack));
  const Result<ControlAckMessage> control = decode_control_ack(control_payload);
  EXPECT_OK(af_ctx, control);
  EXPECT_EQ(af_ctx, control.value().ok, samples.control_ack.ok);
  EXPECT_EQ(af_ctx, control.value().code, ErrorCode::StalePopulationGeneration);
  EXPECT_EQ(af_ctx, control.value().detail, samples.control_ack.detail);

  REQUIRE_VALUE(af_ctx, std::string, error_payload, encode_payload(samples.error_response));
  const Result<ErrorResponseMessage> error = decode_error_response(error_payload);
  EXPECT_OK(af_ctx, error);
  EXPECT_EQ(af_ctx, error.value().code, ErrorCode::FrameTooLarge);
  EXPECT_EQ(af_ctx, error.value().detail, samples.error_response.detail);
  EXPECT_EQ(af_ctx, error.value().in_reply_to, MessageType::CandidatePublish);

  af_ctx.phase("VERIFY");
  EXPECT_NE(af_ctx, hello.value().label, samples.hello.capability);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(protocol, a_frame_split_across_three_reads_is_reassembled) {
  af_ctx.phase("SETUP");
  const MessageSamples samples = make_samples();
  REQUIRE_VALUE(af_ctx, std::string, payload, encode_payload(samples.heartbeat));
  const FrameHeader header = frame_header(MessageType::Heartbeat, 21u, 0u, payload);
  const std::string buffer = encode_frame(header, payload);
  const std::size_t total = buffer.size();
  const std::size_t half = total / 2u;
  EXPECT_TRUE(af_ctx, total > kFrameHeaderBytes);
  EXPECT_TRUE(af_ctx, half > 1u);
  EXPECT_TRUE(af_ctx, half > kFrameHeaderBytes);

  af_ctx.phase("WAIT");
  std::string accumulated;
  std::size_t consumed = 0;

  accumulated.append(buffer.substr(0, 1u));
  const Result<Frame> first = decode_frame(accumulated, &consumed);
  EXPECT_NOT_OK(af_ctx, first);
  EXPECT_TRUE(af_ctx, status_is_incomplete(first.status()));
  // A partial read consumes nothing: the caller retries with the whole buffer.
  EXPECT_EQ(af_ctx, consumed, std::size_t{0});

  accumulated.append(buffer.substr(1u, half - 1u));
  EXPECT_EQ(af_ctx, accumulated.size(), half);
  const Result<Frame> second = decode_frame(accumulated, &consumed);
  EXPECT_NOT_OK(af_ctx, second);
  EXPECT_TRUE(af_ctx, status_is_incomplete(second.status()));
  EXPECT_EQ(af_ctx, consumed, std::size_t{0});

  af_ctx.phase("VERIFY");
  accumulated.append(buffer.substr(half, total - half));
  EXPECT_EQ(af_ctx, accumulated.size(), total);
  const Result<Frame> third = decode_frame(accumulated, &consumed);
  EXPECT_OK(af_ctx, third);
  EXPECT_EQ(af_ctx, consumed, total);
  EXPECT_EQ(af_ctx, third.value().payload, payload);
  EXPECT_EQ(af_ctx, third.value().header.sequence, header.sequence);

  const Result<HeartbeatMessage> heartbeat = decode_heartbeat(third.value().payload);
  EXPECT_OK(af_ctx, heartbeat);
  EXPECT_EQ(af_ctx, heartbeat.value().sequence, samples.heartbeat.sequence);
  EXPECT_EQ(af_ctx, heartbeat.value().session, samples.heartbeat.session);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(protocol, three_coalesced_frames_decode_in_order) {
  af_ctx.phase("SETUP");
  const MessageSamples samples = make_samples();
  REQUIRE_VALUE(af_ctx, std::string, created_payload,
                encode_payload(samples.population_created));
  REQUIRE_VALUE(af_ctx, std::string, publish_payload, encode_payload(samples.publish_ack));
  REQUIRE_VALUE(af_ctx, std::string, control_payload, encode_payload(samples.control_ack));

  const std::string first_frame =
      encode_frame(frame_header(MessageType::PopulationCreated, 1u, 0u, created_payload),
                   created_payload);
  const std::string second_frame =
      encode_frame(frame_header(MessageType::PublishAck, 2u, 0u, publish_payload),
                   publish_payload);
  const std::string third_frame =
      encode_frame(frame_header(MessageType::ControlAck, 3u, 0u, control_payload),
                   control_payload);

  af_ctx.phase("DISPATCH");
  const std::string stream = first_frame + second_frame + third_frame;
  std::size_t offset = 0;
  std::size_t consumed = 0;

  const Result<Frame> first = decode_frame(std::string_view(stream).substr(offset), &consumed);
  EXPECT_OK(af_ctx, first);
  EXPECT_EQ(af_ctx, consumed, first_frame.size());
  EXPECT_EQ(af_ctx, static_cast<std::uint16_t>(first.value().header.type),
            static_cast<std::uint16_t>(MessageType::PopulationCreated));
  offset += consumed;

  const Result<Frame> second = decode_frame(std::string_view(stream).substr(offset), &consumed);
  EXPECT_OK(af_ctx, second);
  EXPECT_EQ(af_ctx, consumed, second_frame.size());
  EXPECT_EQ(af_ctx, static_cast<std::uint16_t>(second.value().header.type),
            static_cast<std::uint16_t>(MessageType::PublishAck));
  offset += consumed;

  const Result<Frame> third = decode_frame(std::string_view(stream).substr(offset), &consumed);
  EXPECT_OK(af_ctx, third);
  EXPECT_EQ(af_ctx, consumed, third_frame.size());
  EXPECT_EQ(af_ctx, static_cast<std::uint16_t>(third.value().header.type),
            static_cast<std::uint16_t>(MessageType::ControlAck));
  offset += consumed;

  af_ctx.phase("VERIFY");
  EXPECT_EQ(af_ctx, offset, stream.size());
  EXPECT_EQ(af_ctx, first.value().payload, created_payload);
  EXPECT_EQ(af_ctx, second.value().payload, publish_payload);
  EXPECT_EQ(af_ctx, third.value().payload, control_payload);

  const Result<PopulationCreatedMessage> created =
      decode_population_created(first.value().payload);
  EXPECT_OK(af_ctx, created);
  EXPECT_EQ(af_ctx, created.value().population, samples.population_created.population);

  const Result<PublishAckMessage> published = decode_publish_ack(second.value().payload);
  EXPECT_OK(af_ctx, published);
  EXPECT_EQ(af_ctx, published.value().candidate, samples.publish_ack.candidate);

  const Result<ControlAckMessage> control = decode_control_ack(third.value().payload);
  EXPECT_OK(af_ctx, control);
  EXPECT_EQ(af_ctx, control.value().code, samples.control_ack.code);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(protocol, frame_rejections_carry_their_own_codes) {
  af_ctx.phase("SETUP");
  const MessageSamples samples = make_samples();
  REQUIRE_VALUE(af_ctx, std::string, payload, encode_payload(samples.publish_ack));
  const std::string buffer =
      encode_frame(frame_header(MessageType::PublishAck, 5u, 0u, payload), payload);
  std::size_t consumed = 0;

  af_ctx.phase("VERIFY_BAD_MAGIC");
  std::string bad_magic = buffer;
  write_u32_le(bad_magic, 0, kFrameMagic ^ 0x1u);
  EXPECT_NE(af_ctx, read_u32_le(bad_magic, 0), kFrameMagic);
  const Result<Frame> magic_result = decode_frame(bad_magic, &consumed);
  EXPECT_STATUS_CODE(af_ctx, magic_result, ErrorCode::ProtocolViolation);
  EXPECT_FALSE(af_ctx, status_is_incomplete(magic_result.status()));

  af_ctx.phase("VERIFY_BAD_VERSION");
  std::string bad_version = buffer;
  const std::uint16_t other_version = static_cast<std::uint16_t>(kProtocolVersion + 1u);
  write_u16_le(bad_version, 4, other_version);
  EXPECT_NE(af_ctx, other_version, kProtocolVersion);
  const Result<Frame> version_result = decode_frame(bad_version, &consumed);
  EXPECT_STATUS_CODE(af_ctx, version_result, ErrorCode::ProtocolViolation);

  af_ctx.phase("VERIFY_OVERSIZED_DECLARATION");
  std::string oversized = buffer;
  write_u32_le(oversized, 20, kMaxFramePayloadBytes + 1u);
  EXPECT_EQ(af_ctx, read_u32_le(oversized, 20), kMaxFramePayloadBytes + 1u);
  const Result<Frame> large_result = decode_frame(oversized, &consumed);
  EXPECT_STATUS_CODE(af_ctx, large_result, ErrorCode::FrameTooLarge);
  EXPECT_FALSE(af_ctx, status_is_incomplete(large_result.status()));

  af_ctx.phase("VERIFY_PAYLOAD_CRC");
  std::string corrupt = buffer;
  const std::size_t probe = kFrameHeaderBytes + 3u;
  corrupt[probe] = static_cast<char>(static_cast<unsigned char>(corrupt[probe]) ^ 0xFFu);
  // The header CRC is deliberately left alone: only the payload moved.
  EXPECT_EQ(af_ctx, read_u32_le(corrupt, 24), read_u32_le(buffer, 24));
  EXPECT_NE(af_ctx, corrupt, buffer);
  const Result<Frame> crc_result = decode_frame(corrupt, &consumed);
  EXPECT_STATUS_CODE(af_ctx, crc_result, ErrorCode::FrameCorrupt);
  EXPECT_FALSE(af_ctx, status_is_incomplete(crc_result.status()));

  af_ctx.phase("VERIFY_INTACT_FRAME");
  // Every rejection above was caused by the mutation, not by the frame.
  const Result<Frame> intact = decode_frame(buffer, &consumed);
  EXPECT_OK(af_ctx, intact);
  EXPECT_EQ(af_ctx, consumed, buffer.size());
  const Result<PublishAckMessage> decoded = decode_publish_ack(intact.value().payload);
  EXPECT_OK(af_ctx, decoded);
  EXPECT_EQ(af_ctx, decoded.value().candidate, samples.publish_ack.candidate);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(protocol, decoders_refuse_trailing_bytes) {
  af_ctx.phase("SETUP");
  const MessageSamples samples = make_samples();
  REQUIRE_VALUE(af_ctx, std::string, task_payload, encode_payload(samples.task_created));
  REQUIRE_VALUE(af_ctx, std::string, control_payload, encode_payload(samples.control_ack));

  af_ctx.phase("VERIFY_TRAILING_BYTE");
  std::string extended = task_payload;
  extended.push_back('');
  const Result<TaskCreatedMessage> rejected = decode_task_created(extended);
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::MalformedEncoding);
  const Result<TaskCreatedMessage> accepted = decode_task_created(task_payload);
  EXPECT_OK(af_ctx, accepted);
  EXPECT_EQ(af_ctx, accepted.value().task, samples.task_created.task);
  EXPECT_EQ(af_ctx, accepted.value().content_digest, samples.task_created.content_digest);

  af_ctx.phase("VERIFY_TRAILING_TEXT");
  std::string trailing_text = control_payload;
  trailing_text.append("ignored?");
  const Result<ControlAckMessage> rejected_ack = decode_control_ack(trailing_text);
  EXPECT_STATUS_CODE(af_ctx, rejected_ack, ErrorCode::MalformedEncoding);
  const Result<ControlAckMessage> accepted_ack = decode_control_ack(control_payload);
  EXPECT_OK(af_ctx, accepted_ack);
  EXPECT_EQ(af_ctx, accepted_ack.value().detail, samples.control_ack.detail);

  af_ctx.phase("VERIFY_TRUNCATED_PAYLOAD");
  const std::string_view short_payload =
      std::string_view(control_payload).substr(0, control_payload.size() - 1u);
  const Result<ControlAckMessage> short_ack = decode_control_ack(short_payload);
  EXPECT_NOT_OK(af_ctx, short_ack);
  EXPECT_STATUS_CODE(af_ctx, short_ack, ErrorCode::MalformedEncoding);

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(protocol, cross_domain_identity_is_refused) {
  af_ctx.phase("SETUP");
  const TaskId task = TaskId::from_raw(raw_identity(IdKind::Task, 6u));

  // A hand-built task-created payload whose identity field carries the
  // Candidate domain tag in its high byte instead of the Task tag.
  ByteWriter smuggled;
  smuggled.u64((task.raw() & 0x00FFFFFFFFFFFFFFull) |
               (static_cast<std::uint64_t>(IdKind::Candidate) << 56));
  smuggled.u32(TaskGeneration::first().value());
  smuggled.str("9f2c");
  smuggled.str("task defined");
  const std::string smuggled_payload = smuggled.take();

  ByteWriter honest;
  honest.u64(task.raw());
  honest.u32(TaskGeneration::first().value());
  honest.str("9f2c");
  honest.str("task defined");
  const std::string honest_payload = honest.take();

  af_ctx.phase("DISPATCH");
  // The frame layer is domain agnostic: the identity check belongs to the
  // payload codec, and that is where the smuggled tag is caught.
  const std::string buffer =
      encode_frame(frame_header(MessageType::TaskCreated, 31u, 0u, smuggled_payload),
                   smuggled_payload);
  std::size_t consumed = 0;
  const Result<Frame> framed = decode_frame(buffer, &consumed);
  EXPECT_OK(af_ctx, framed);
  EXPECT_EQ(af_ctx, framed.value().payload, smuggled_payload);

  af_ctx.phase("VERIFY");
  const Result<TaskCreatedMessage> refused = decode_task_created(framed.value().payload);
  EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::CrossDomainIdentity);
  EXPECT_TRUE(af_ctx, refused.status().message().find("Candidate") != std::string::npos);

  // The same bytes with the correct domain tag are accepted, so the refusal is
  // about the tag and not about the rest of the payload.
  const Result<TaskCreatedMessage> accepted = decode_task_created(honest_payload);
  EXPECT_OK(af_ctx, accepted);
  EXPECT_EQ(af_ctx, accepted.value().task, task);
  EXPECT_EQ(af_ctx, accepted.value().generation, TaskGeneration::first());

  af_ctx.phase("VERIFY_NULL_IDENTITY");
  // An absent identity is a different refusal from a confused one.
  ByteWriter absent;
  absent.u64(0u);
  absent.u32(TaskGeneration::first().value());
  absent.str("9f2c");
  absent.str("task defined");
  const Result<TaskCreatedMessage> null_identity = decode_task_created(absent.take());
  EXPECT_STATUS_CODE(af_ctx, null_identity, ErrorCode::NullIdentity);

  af_ctx.phase("SHUTDOWN");
}
