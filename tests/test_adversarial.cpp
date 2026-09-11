// Adversarial suite.
//
// Hostile input against the real core and the real codec: replayed identities,
// wrapped generations, late and duplicate completions, replayed decisions,
// corrupt snapshot images at every layer of the file format, malformed frames,
// and artifact names that try to escape a workspace.
//
// Every case builds its own state. Where a case needs more than one candidate in
// one population it performs the documented production round cycle explicitly:
// a publication moves the population to Evaluating, so the round's candidate
// must be driven to Evaluated through begin_evaluation plus complete evidence,
// and the round must then be closed with request_population_revalidation plus
// revalidate_population before the next slot can be dispatched.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/protocol.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

// ---------------------------------------------------------------------------
// Little endian byte-level editing of real encoded images
// ---------------------------------------------------------------------------

void write_u16_le(std::string& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<char>(value & 0xFFu);
  bytes[offset + 1] = static_cast<char>((value >> 8u) & 0xFFu);
}

void write_u32_le(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<char>((value >> (8u * index)) & 0xFFu);
  }
}

void write_u64_le(std::string& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<char>((value >> (8u * index)) & 0xFFu);
  }
}

[[nodiscard]] std::uint64_t read_u64_le(std::string_view bytes, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]))
             << (8u * index);
  }
  return value;
}

/// Recompute the snapshot payload CRC-32C so a tampered payload reaches the
/// semantic parser instead of being rejected by the integrity check.
void refresh_payload_crc(std::string& image) {
  const std::uint64_t payload_length = read_u64_le(image, 8);
  const std::string_view payload(
      image.data() + kSnapshotHeaderBytes, static_cast<std::size_t>(payload_length));
  write_u32_le(image, 16, crc32c(payload));
}

/// Payload offsets inside a real snapshot image.
constexpr std::size_t kRecordCountOffset = kSnapshotHeaderBytes;
constexpr std::size_t kFirstRecordTypeOffset = kSnapshotHeaderBytes + 4;
constexpr std::size_t kFirstRecordBodyLengthOffset = kSnapshotHeaderBytes + 5;
constexpr std::size_t kFirstRecordBodyOffset = kSnapshotHeaderBytes + 9;

// ---------------------------------------------------------------------------
// Core driving helpers
// ---------------------------------------------------------------------------

struct PreparedAttempt {
  PendingDispatch pending;
  AttemptPackage package;
  WorkerOperationAuthority authority;
};

[[nodiscard]] PreparedAttempt prepare_attempt(const af_test::TestContext& context, FoundryCore& core,
                                              const WorkerSessionAuthority& session) {
  PreparedAttempt prepared;
  prepared.pending = af_test::require_value(context, core.authorize_attempt(session),
                                            "authorize_attempt", __FILE__, __LINE__);
  prepared.package = af_test::require_value(
      context,
      core.confirm_dispatch(session, prepared.pending.attempt.id, prepared.pending.attempt.generation),
      "confirm_dispatch", __FILE__, __LINE__);

  WorkerOperationAuthority authority;
  authority.session = session;
  authority.population = prepared.package.population;
  authority.population_generation = prepared.package.population_generation;
  authority.task = prepared.package.task;
  authority.task_generation = prepared.package.task_generation;
  authority.candidate = prepared.package.candidate;
  authority.candidate_generation = prepared.package.candidate_generation;
  authority.attempt = prepared.package.attempt;
  authority.attempt_generation = prepared.package.attempt_generation;
  authority.assignment = prepared.package.assignment;

  const Status acknowledged = core.acknowledge_attempt(authority);
  if (!acknowledged.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("acknowledge_attempt failed: ") +
                        std::string(error_code_name(acknowledged.code())) + " '" +
                        acknowledged.message() + "'");
  }
  prepared.authority = authority;
  return prepared;
}

[[nodiscard]] std::vector<ArtifactRef> solution_artifacts(std::string_view source) {
  std::vector<ArtifactRef> artifacts;
  ArtifactRef solution;
  solution.name = std::string(kReferenceTaskSourceArtifact);
  solution.size_bytes = static_cast<std::uint64_t>(source.size());
  solution.content_digest = sha256_hex(source);
  artifacts.push_back(solution);
  return artifacts;
}

/// Every requirement af_test::make_task declares, so the candidate settles.
void record_full_evidence(const af_test::TestContext& context, FoundryCore& core,
                          const CandidateRecord& candidate) {
  af_test::record_evidence(context, core, candidate, kEvaluatorCompileAndRun,
                           EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                           RequirementClass::Mandatory, false, 0.0);
  af_test::record_evidence(context, core, candidate, kEvaluatorSourcePolicy,
                           EvaluationOutcome::Pass, EvaluatorKind::SourcePolicy,
                           RequirementClass::Mandatory, false, 0.0);
  af_test::record_evidence(context, core, candidate, kEvaluatorPerformance,
                           EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                           RequirementClass::Optional, true, 2.0);
  af_test::record_evidence(context, core, candidate, kEvaluatorParsimony,
                           EvaluationOutcome::Pass, EvaluatorKind::PortableReference,
                           RequirementClass::Optional, true, 2.0);
}

/// Production starts only from Running, and it leaves the population in
/// Evaluating, so the round has to be closed before the next slot opens.
void close_round(const af_test::TestContext& context, FoundryCore& core, PopulationId population) {
  const Status requested = core.request_population_revalidation(
      population, "the production round is closed; the next slot needs fresh authority");
  if (!requested.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("request_population_revalidation failed: ") +
                        std::string(error_code_name(requested.code())) + " '" +
                        requested.message() + "'");
  }
  const Result<PopulationRecord> pending = core.population(population);
  if (!pending.ok()) {
    context.fail_at(__FILE__, __LINE__, "the population disappeared during revalidation");
  }
  const Status applied = core.revalidate_population(population, pending.value().generation,
                                                    "the production round is closed");
  if (!applied.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("revalidate_population failed: ") +
                        std::string(error_code_name(applied.code())) + " '" + applied.message() +
                        "'");
  }
}

/// Produce one candidate, drive it to Evaluated, and close the round on request.
[[nodiscard]] CandidateId produce_evaluated(const af_test::TestContext& context, FoundryCore& core,
                                            const WorkerSessionAuthority& session,
                                            std::string_view source, std::string_view strategy,
                                            bool close_the_round) {
  const PreparedAttempt prepared = prepare_attempt(context, core, session);
  const PublicationOutcome outcome = af_test::require_value(
      context,
      core.publish_candidate(prepared.authority, solution_artifacts(source), std::string(strategy)),
      "publish_candidate", __FILE__, __LINE__);
  const Status begun = core.begin_evaluation(outcome.candidate);
  if (!begun.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("begin_evaluation failed: ") +
                        std::string(error_code_name(begun.code())) + " '" + begun.message() + "'");
  }
  record_full_evidence(context, core,
                       af_test::load_candidate(context, core, outcome.candidate));
  if (close_the_round) {
    close_round(context, core, prepared.authority.population);
  }
  return outcome.candidate;
}

/// Build a complete evaluation record without recording it, so a case can send
/// the same evidence twice or mutate one field.
[[nodiscard]] EvaluationRecord make_record(const af_test::TestContext& context, FoundryCore& core,
                                           const CandidateRecord& candidate, std::string_view key,
                                           EvaluationOutcome outcome, EvaluatorKind kind,
                                           RequirementClass requirement_class, bool has_score,
                                           double score) {
  const PopulationRecord population = af_test::require_value(
      context, core.population(candidate.population), "population", __FILE__, __LINE__);
  EvaluationRecord record;
  record.candidate = candidate.id;
  record.candidate_generation = candidate.generation;
  record.task = candidate.task;
  record.task_generation = candidate.task_generation;
  record.population = candidate.population;
  record.population_generation = population.generation;
  record.evaluator = EvaluatorId::from_raw((static_cast<std::uint64_t>(IdKind::Evaluator) << 56) |
                                           0x0000C1ull << 32 |
                                           static_cast<std::uint64_t>(candidate.generation.value()));
  record.evaluator_key = std::string(key);
  record.kind = kind;
  record.requirement_class = requirement_class;
  record.outcome = outcome;
  record.complete = true;
  record.has_score = has_score;
  record.score = score;
  record.evidence_digest = "9f2c4c0c1f5a3d4e5b6c7d8e9f00112233445566778899aabbccddeeff001122";
  return record;
}

[[nodiscard]] std::string render_ids(const std::vector<CandidateId>& values) {
  std::string text;
  for (const CandidateId value : values) {
    if (!text.empty()) {
      text.push_back(',');
    }
    text.append(value.to_string());
  }
  return text;
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

AF_TEST_CASE(adversarial, duplicate_worker_identity_registration_is_refused) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(7001));

  WorkerRegistrationRequest request;
  request.worker = af_test::worker_identity(1);
  request.boot = af_test::boot_identity(1);
  request.label = "worker";
  request.capability = "reference";
  request.process_id = 1001;

  af_ctx.phase("REGISTER");
  REQUIRE_VALUE(af_ctx, WorkerSessionAuthority, session,
                fixture.core.register_worker(request));
  const Status ready = fixture.core.worker_ready(session, "reference");
  EXPECT_OK(af_ctx, ready);

  af_ctx.phase("VERIFY_DUPLICATE");
  const Result<WorkerSessionAuthority> duplicate = fixture.core.register_worker(request);
  EXPECT_STATUS_CODE(af_ctx, duplicate, ErrorCode::AlreadyExists);
  EXPECT_EQ(af_ctx, fixture.core.workers().size(), static_cast<std::size_t>(1));

  // The refusal must not have disturbed the live incarnation.
  const Status still_ready = fixture.core.worker_ready(session, "reference");
  EXPECT_OK(af_ctx, still_ready);

  af_ctx.phase("VERIFY_REPLACED_INCARNATION");
  // A different boot identity is a new incarnation: the old authority can never
  // be replayed against it.
  request.boot = af_test::boot_identity(2);
  REQUIRE_VALUE(af_ctx, WorkerSessionAuthority, replacement,
                fixture.core.register_worker(request));
  EXPECT_TRUE(af_ctx, replacement.session != session.session);
  EXPECT_TRUE(af_ctx, replacement.session_generation.value() >
                          session.session_generation.value());
  const Status stale = fixture.core.worker_ready(session, "reference");
  EXPECT_STATUS_IN(af_ctx, stale, ErrorCode::StaleWorkerBoot, ErrorCode::StaleSession);
  EXPECT_TRUE(af_ctx, is_stale_authority(stale.code()));

  const Status replacement_ready = fixture.core.worker_ready(replacement, "reference");
  EXPECT_OK(af_ctx, replacement_ready);
  EXPECT_EQ(af_ctx, fixture.core.workers().size(), static_cast<std::size_t>(1));
}

AF_TEST_CASE(adversarial, cross_domain_identity_is_refused_by_the_codec) {
  af_ctx.phase("VERIFY_CODEC_DOMAIN_TAG");
  // A hand-built raw value whose high byte names a different identity domain.
  const std::uint64_t task_raw = TaskId::from_raw(0x0400000100000001ull).raw();
  {
    ByteWriter writer;
    writer.u64(task_raw);
    const std::string payload = writer.take();
    ByteReader reader(payload);
    const Result<CandidateId> smuggled = reader.identity<CandidateIdTag>("candidate");
    EXPECT_STATUS_CODE(af_ctx, smuggled, ErrorCode::CrossDomainIdentity);
    EXPECT_TRUE(af_ctx, contains(smuggled.status().message(), "candidate"));
    EXPECT_FALSE(af_ctx, is_stale_authority(smuggled.status().code()));
  }
  {
    const std::uint64_t candidate_raw = CandidateId::from_raw(0x0500000100000001ull).raw();
    ByteWriter writer;
    writer.u64(candidate_raw);
    const std::string payload = writer.take();
    ByteReader reader(payload);
    const Result<TaskId> smuggled = reader.identity<TaskIdTag>("task");
    EXPECT_STATUS_CODE(af_ctx, smuggled, ErrorCode::CrossDomainIdentity);
  }
  {
    ByteWriter writer;
    writer.u64(0);
    const std::string payload = writer.take();
    ByteReader reader(payload);
    const Result<CandidateId> null_identity = reader.identity<CandidateIdTag>("candidate");
    EXPECT_STATUS_CODE(af_ctx, null_identity, ErrorCode::NullIdentity);
    // The optional form is the documented way to spell "no identity".
    ByteReader optional_reader(payload);
    const Result<CandidateId> optional = optional_reader.optional_identity<CandidateIdTag>("candidate");
    EXPECT_OK(af_ctx, optional);
    EXPECT_TRUE(af_ctx, optional.value().is_null());
  }
  {
    const Result<CandidateId> direct =
        strong_id_from_raw_checked<CandidateIdTag>(task_raw, "candidate");
    EXPECT_STATUS_CODE(af_ctx, direct, ErrorCode::CrossDomainIdentity);
  }

  af_ctx.phase("VERIFY_SNAPSHOT_DOMAIN_CHECK");
  af_test::FoundryFixture fixture(af_test::make_config(7002));
  af_test::build_running_fixture(af_ctx, fixture, 2);
  REQUIRE_VALUE(af_ctx, std::string, image, serialize_snapshot(fixture.core.snapshot()));

  // The identity header record carries the foundry identity as its first field;
  // rewrite it with a worker-domain raw value and repair the payload CRC so the
  // file reaches the semantic decoder intact.
  std::string tampered = image;
  const std::uint64_t worker_raw = WorkerId::from_raw(0x0700000100000001ull).raw();
  write_u64_le(tampered, kFirstRecordBodyOffset, worker_raw);
  refresh_payload_crc(tampered);
  const Result<FoundrySnapshot> refused = deserialize_snapshot(tampered);
  EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::CrossDomainIdentity);

  // The same image with an untouched identity still loads, which proves the
  // refusal came from the identity domain and not from the byte patch itself.
  const Result<FoundrySnapshot> accepted = deserialize_snapshot(image);
  EXPECT_OK(af_ctx, accepted);
}

AF_TEST_CASE(adversarial, generation_boundaries_refuse_to_wrap) {
  af_ctx.phase("VERIFY_GENERATION_LIMIT");
  EXPECT_TRUE(af_ctx, PopulationGeneration::first().valid());
  EXPECT_EQ(af_ctx, PopulationGeneration::first().value(), 1u);
  EXPECT_FALSE(af_ctx, PopulationGeneration::from_value(0).valid());

  const std::optional<PopulationGeneration> penultimate =
      PopulationGeneration::from_value(0xFFFFFFFEu).next();
  EXPECT_TRUE(af_ctx, penultimate.has_value());
  if (penultimate.has_value()) {
    EXPECT_EQ(af_ctx, penultimate->value(), 0xFFFFFFFFu);
  }
  EXPECT_FALSE(af_ctx, PopulationGeneration::from_value(0xFFFFFFFFu).next().has_value());

  af_ctx.phase("VERIFY_EVERY_GENERATION_DOMAIN");
  EXPECT_FALSE(af_ctx, CandidateGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, AttemptGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, EvaluationGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, PolicyGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, TaskGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, SelectionGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, EvidenceGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, AssignmentGeneration::from_value(0xFFFFFFFFu).next().has_value());
  EXPECT_FALSE(af_ctx, WorkerSessionGeneration::from_value(0xFFFFFFFFu).next().has_value());

  const std::optional<CandidateGeneration> last =
      CandidateGeneration::from_value(0xFFFFFFFEu).next();
  EXPECT_TRUE(af_ctx, last.has_value());
  if (last.has_value()) {
    EXPECT_EQ(af_ctx, last->value(), 0xFFFFFFFFu);
    EXPECT_TRUE(af_ctx, last->valid());
    EXPECT_FALSE(af_ctx, last->next().has_value());
  }

  af_ctx.phase("VERIFY_EPOCH_LIMIT");
  EXPECT_FALSE(af_ctx, CoordinatorEpoch::from_value(0).valid());
  EXPECT_EQ(af_ctx, CoordinatorEpoch::from_value(3).value(), 3u);
  const std::optional<CoordinatorEpoch> epoch_last =
      CoordinatorEpoch::from_value(std::numeric_limits<std::uint64_t>::max() - 1u).next();
  EXPECT_TRUE(af_ctx, epoch_last.has_value());
  if (epoch_last.has_value()) {
    EXPECT_EQ(af_ctx, epoch_last->value(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(af_ctx, epoch_last->next().has_value());
  }
  EXPECT_FALSE(af_ctx,
               CoordinatorEpoch::from_value(std::numeric_limits<std::uint64_t>::max()).next().has_value());

  af_ctx.phase("VERIFY_ALLOCATOR_EXHAUSTION");
  // The identity allocator reports exhaustion rather than re-issuing an identity
  // that already exists.
  IdAllocator allocator(0x00ABCDEFu);
  REQUIRE_VALUE(af_ctx, CandidateId, first_id, allocator.next<CandidateIdTag>());
  EXPECT_EQ(af_ctx, first_id.counter(), 1u);
  const Status restored = allocator.restore_counter(IdKind::Candidate, 0xFFFFFFFFu);
  EXPECT_OK(af_ctx, restored);
  const Result<CandidateId> exhausted = allocator.next<CandidateIdTag>();
  EXPECT_STATUS_CODE(af_ctx, exhausted, ErrorCode::IdentityExhausted);
}

AF_TEST_CASE(adversarial, late_and_duplicate_publication_are_refused) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(7003));
  af_test::build_running_fixture(af_ctx, fixture, 2);

  af_ctx.phase("DISPATCH");
  const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, 1);
  const PreparedAttempt prepared = prepare_attempt(af_ctx, fixture.core, session);

  af_ctx.phase("CANCEL");
  const Status cancelled = fixture.core.cancel_attempt(prepared.authority.attempt, "operator cancel");
  EXPECT_OK(af_ctx, cancelled);
  REQUIRE_VALUE(af_ctx, AttemptRecord, attempt,
                fixture.core.attempt(prepared.authority.attempt));
  EXPECT_EQ(af_ctx, attempt.state, AttemptState::Cancelled);
  EXPECT_TRUE(af_ctx, attempt_state_is_terminal(attempt.state));
  REQUIRE_VALUE(af_ctx, CandidateRecord, candidate,
                fixture.core.candidate(prepared.authority.candidate));
  // An explicit cancellation is final for the candidate slot, so the late
  // publication below is refused by the candidate's own terminal state as well
  // as by the terminal attempt.
  EXPECT_EQ(af_ctx, candidate.state, CandidateState::ProductionCancelled);
  EXPECT_TRUE(af_ctx, candidate_state_is_terminal(candidate.state));

  af_ctx.phase("VERIFY_LATE_PUBLICATION");
  const Result<PublicationOutcome> late = fixture.core.publish_candidate(
      prepared.authority, solution_artifacts(af_test::reference_solution_source(0)), "late");
  EXPECT_STATUS_CODE(af_ctx, late, ErrorCode::LateCompletion);
  const CandidateRecord unchanged =
      af_test::load_candidate(af_ctx, fixture.core, prepared.authority.candidate);
  EXPECT_TRUE(af_ctx, unchanged.artifact_set_digest.empty());
  EXPECT_TRUE(af_ctx, unchanged.artifacts.empty());
  EXPECT_EQ(af_ctx, unchanged.state, CandidateState::ProductionCancelled);

  af_ctx.phase("VERIFY_DUPLICATE_PUBLICATION");
  af_test::FoundryFixture second(af_test::make_config(7004));
  af_test::build_running_fixture(af_ctx, second, 2);
  const WorkerSessionAuthority second_session = af_test::connect_worker(af_ctx, second.core, 1);
  const PreparedAttempt live = prepare_attempt(af_ctx, second.core, second_session);
  const std::vector<ArtifactRef> artifacts =
      solution_artifacts(af_test::reference_solution_source(1));
  // A repeated acknowledgement of an attempt that is already running is the one
  // idempotent message in this path: the assignment was already accepted, so no
  // second transition happens.
  const Status acknowledged_twice = second.core.acknowledge_attempt(live.authority);
  EXPECT_OK(af_ctx, acknowledged_twice);

  const Result<PublicationOutcome> published = second.core.publish_candidate(
      live.authority, artifacts, "strategy-1");
  EXPECT_OK(af_ctx, published);

  const Result<PublicationOutcome> duplicate = second.core.publish_candidate(
      live.authority, artifacts, "strategy-1");
  EXPECT_STATUS_CODE(af_ctx, duplicate, ErrorCode::DuplicateResult);
  const CandidateRecord published_candidate =
      af_test::load_candidate(af_ctx, second.core, live.authority.candidate);
  EXPECT_EQ(af_ctx, published_candidate.artifacts.size(), static_cast<std::size_t>(1));
  EXPECT_FALSE(af_ctx, published_candidate.artifact_set_digest.empty());

  af_ctx.phase("VERIFY_LATE_ACKNOWLEDGEMENT_IS_REFUSED");
  const Status acknowledged_again = second.core.acknowledge_attempt(live.authority);
  EXPECT_STATUS_CODE(af_ctx, acknowledged_again, ErrorCode::LateCompletion);
}

AF_TEST_CASE(adversarial, duplicate_and_late_evaluations_are_refused) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(7005));
  af_test::build_running_fixture(af_ctx, fixture, 2);

  af_ctx.phase("DISPATCH");
  const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, 1);
  const PreparedAttempt prepared = prepare_attempt(af_ctx, fixture.core, session);
  const Result<PublicationOutcome> published = fixture.core.publish_candidate(
      prepared.authority, solution_artifacts(af_test::reference_solution_source(0)), "strategy-1");
  REQUIRE_VALUE(af_ctx, PublicationOutcome, outcome, published);
  const Status begun = fixture.core.begin_evaluation(outcome.candidate);
  EXPECT_OK(af_ctx, begun);
  const CandidateRecord candidate =
      af_test::load_candidate(af_ctx, fixture.core, outcome.candidate);

  af_ctx.phase("COMMIT");
  const Status first = fixture.core.record_evaluation(
      make_record(af_ctx, fixture.core, candidate, kEvaluatorCompileAndRun,
                  EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                  RequirementClass::Mandatory, false, 0.0));
  EXPECT_OK(af_ctx, first);

  af_ctx.phase("VERIFY_DUPLICATE_EVALUATION");
  const Status duplicate = fixture.core.record_evaluation(
      make_record(af_ctx, fixture.core, candidate, kEvaluatorCompileAndRun,
                  EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                  RequirementClass::Mandatory, false, 0.0));
  EXPECT_STATUS_CODE(af_ctx, duplicate, ErrorCode::DuplicateResult);
  EXPECT_EQ(af_ctx, fixture.core.candidate_evaluations(outcome.candidate).size(),
            static_cast<std::size_t>(1));

  // The refusal is per evaluator key: an unrelated key is still accepted.
  const Status other_key = fixture.core.record_evaluation(
      make_record(af_ctx, fixture.core, candidate, "supplementary-check",
                  EvaluationOutcome::Pass, EvaluatorKind::PortableReference,
                  RequirementClass::Optional, false, 0.0));
  EXPECT_OK(af_ctx, other_key);
  EXPECT_EQ(af_ctx, fixture.core.candidate_evaluations(outcome.candidate).size(),
            static_cast<std::size_t>(2));

  af_ctx.phase("VERIFY_LATE_EVALUATION_AFTER_CANCELLATION");
  af_test::FoundryFixture second(af_test::make_config(7006));
  af_test::build_running_fixture(af_ctx, second, 2);
  const WorkerSessionAuthority second_session = af_test::connect_worker(af_ctx, second.core, 1);
  const PreparedAttempt live = prepare_attempt(af_ctx, second.core, second_session);
  const Result<PublicationOutcome> live_published = second.core.publish_candidate(
      live.authority, solution_artifacts(af_test::reference_solution_source(1)), "strategy-1");
  REQUIRE_VALUE(af_ctx, PublicationOutcome, live_outcome, live_published);

  // Cancelling the attempt withdraws the candidate slot as well, including one
  // whose output was already published.
  const Status cancelled_attempt =
      second.core.cancel_attempt(live.authority.attempt, "operator cancel");
  EXPECT_OK(af_ctx, cancelled_attempt);
  REQUIRE_VALUE(af_ctx, CandidateRecord, cancelled_candidate,
                second.core.candidate(live_outcome.candidate));
  EXPECT_EQ(af_ctx, cancelled_candidate.state, CandidateState::ProductionCancelled);
  EXPECT_TRUE(af_ctx, candidate_state_is_terminal(cancelled_candidate.state));
  const Status late = second.core.record_evaluation(
      make_record(af_ctx, second.core, cancelled_candidate, kEvaluatorCompileAndRun,
                  EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                  RequirementClass::Mandatory, false, 0.0));
  EXPECT_STATUS_CODE(af_ctx, late, ErrorCode::LateCompletion);
  EXPECT_TRUE(af_ctx, second.core.candidate_evaluations(live_outcome.candidate).empty());

  af_ctx.phase("VERIFY_TERMINAL_POPULATION_CANNOT_SELECT");
  // Cancelling the whole population is the coarser withdrawal: its attempts
  // become terminal and the population can never select again, so evidence that
  // arrives afterwards can never produce a winner. A candidate whose output was
  // already published is deliberately left Published by that operation, which is
  // why the population, not the candidate, is the boundary proven here.
  af_test::FoundryFixture third(af_test::make_config(7011));
  af_test::build_running_fixture(af_ctx, third, 2);
  const WorkerSessionAuthority third_session = af_test::connect_worker(af_ctx, third.core, 1);
  const PreparedAttempt third_attempt = prepare_attempt(af_ctx, third.core, third_session);
  const Result<PublicationOutcome> third_published = third.core.publish_candidate(
      third_attempt.authority, solution_artifacts(af_test::reference_solution_source(0)),
      "strategy-1");
  REQUIRE_VALUE(af_ctx, PublicationOutcome, third_outcome, third_published);
  REQUIRE_VALUE(af_ctx, PopulationRecord, third_population,
                third.core.population(third_attempt.authority.population));
  const Status cancelled_population = third.core.cancel_population(
      third_attempt.authority.population, third_population.generation, "operator cancel");
  EXPECT_OK(af_ctx, cancelled_population);
  REQUIRE_VALUE(af_ctx, PopulationRecord, terminal_population,
                third.core.population(third_attempt.authority.population));
  EXPECT_EQ(af_ctx, terminal_population.state, PopulationState::Cancelled);
  EXPECT_TRUE(af_ctx, population_state_is_terminal(terminal_population.state));
  REQUIRE_VALUE(af_ctx, AttemptRecord, cancelled_record,
                third.core.attempt(third_attempt.authority.attempt));
  EXPECT_EQ(af_ctx, cancelled_record.state, AttemptState::Cancelled);
  const Result<SelectionDecision> refused =
      third.core.prepare_selection(third_attempt.authority.population);
  EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::IllegalStateTransition);
}

AF_TEST_CASE(adversarial, selection_retention_and_promotion_replays_are_refused) {
  af_ctx.phase("SETUP");
  // Three candidates fill the policy's retain_top_k of three exactly, so
  // retention keeps every candidate and the population holds no exclusion. That
  // matters here because selection commits only when the eligible participant
  // set is the whole population.
  af_test::FoundryFixture fixture(af_test::make_config(7007));
  af_test::build_running_fixture(af_ctx, fixture, 3);

  af_ctx.phase("DISPATCH");
  std::vector<CandidateId> candidates;
  for (std::uint32_t slot = 1; slot <= 3; ++slot) {
    const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, slot);
    candidates.push_back(produce_evaluated(
        af_ctx, fixture.core, session,
        af_test::reference_solution_source(static_cast<int>(slot - 1)),
        "strategy-" + std::to_string(slot), slot < 3));
  }
  for (const CandidateId id : candidates) {
    EXPECT_EQ(af_ctx, af_test::load_candidate(af_ctx, fixture.core, id).state,
              CandidateState::Evaluated);
  }

  af_ctx.phase("COMMIT_SELECTION");
  const Result<SelectionDecision> selection_prepared =
      fixture.core.prepare_selection(fixture.population);
  REQUIRE_VALUE(af_ctx, SelectionDecision, selection, selection_prepared);
  EXPECT_TRUE(af_ctx, selection.has_winner());
  const Result<SelectionDecision> selection_committed =
      fixture.core.commit_selection(selection);
  REQUIRE_VALUE(af_ctx, SelectionDecision, committed_selection, selection_committed);

  af_ctx.phase("VERIFY_SELECTION_REPLAY");
  // The state the decision was derived from changed when the winner was
  // recorded, so the same prepared decision can never be applied twice.
  const Result<SelectionDecision> selection_replay = fixture.core.commit_selection(selection);
  EXPECT_STATUS_CODE(af_ctx, selection_replay, ErrorCode::StaleSelection);
  EXPECT_TRUE(af_ctx, is_stale_authority(selection_replay.status().code()));

  af_ctx.phase("COMMIT_RETENTION");
  const Result<RetentionDecision> retention_prepared =
      fixture.core.prepare_retention(fixture.population);
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention, retention_prepared);
  const Result<RetentionDecision> retention_committed =
      fixture.core.commit_retention(retention);
  REQUIRE_VALUE(af_ctx, RetentionDecision, committed_retention, retention_committed);
  EXPECT_TRUE(af_ctx, committed_retention.committed);
  EXPECT_TRUE(af_ctx, committed_retention.retired.empty());
  EXPECT_EQ(af_ctx, committed_retention.retained.size(), static_cast<std::size_t>(3));

  af_ctx.phase("VERIFY_RETENTION_REPLAY");
  // An immediate replay of the identical decision is either refused as stale or
  // absorbed without changing anything. Both are safe; a second, different
  // retention outcome would not be.
  const Result<RetentionDecision> retention_replay = fixture.core.commit_retention(retention);
  if (retention_replay.ok()) {
    EXPECT_TRUE(af_ctx, retention_replay.value() == committed_retention);
  } else {
    EXPECT_STATUS_CODE(af_ctx, retention_replay, ErrorCode::StaleSelection);
  }
  REQUIRE_VALUE(af_ctx, RetentionDecision, reconfirmed,
                fixture.core.retention(fixture.population));
  EXPECT_TRUE(af_ctx, reconfirmed == committed_retention);
  REQUIRE_VALUE(af_ctx, RetentionDecision, fresh_retention,
                fixture.core.prepare_retention(fixture.population));
  EXPECT_EQ(af_ctx, render_ids(fresh_retention.retained), render_ids(committed_retention.retained));
  EXPECT_EQ(af_ctx, render_ids(fresh_retention.retired), render_ids(committed_retention.retired));

  af_ctx.phase("VERIFY_LATE_EVALUATION_AFTER_RETIREMENT");
  // A population with more candidates than the policy's retain_top_k of three
  // retires the lowest ranked one, and evidence that arrives afterwards is a
  // late completion rather than a fresh fact.
  //
  // NOTE: Superseded is a documented candidate state, but no public core
  // operation produces it, so Retired and ProductionCancelled are the reachable
  // late-completion states.
  af_ctx.note("NOTE Superseded has no producing core operation; Retired and "
              "ProductionCancelled are the reachable late states");
  af_test::FoundryFixture crowded(af_test::make_config(7010));
  af_test::build_running_fixture(af_ctx, crowded, 5);
  std::vector<CandidateId> crowded_candidates;
  for (std::uint32_t slot = 1; slot <= 5; ++slot) {
    const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, crowded.core, slot);
    crowded_candidates.push_back(produce_evaluated(
        af_ctx, crowded.core, session,
        af_test::reference_solution_source(static_cast<int>(slot - 1)),
        "strategy-" + std::to_string(slot), slot < 5));
  }
  REQUIRE_VALUE(af_ctx, SelectionDecision, crowded_selection,
                crowded.core.prepare_selection(crowded.population));
  REQUIRE_VALUE(af_ctx, SelectionDecision, crowded_committed,
                crowded.core.commit_selection(crowded_selection));
  REQUIRE_VALUE(af_ctx, RetentionDecision, crowded_retention,
                crowded.core.prepare_retention(crowded.population));
  REQUIRE_VALUE(af_ctx, RetentionDecision, crowded_retention_committed,
                crowded.core.commit_retention(crowded_retention));
  EXPECT_FALSE(af_ctx, crowded_retention_committed.retired.empty());

  const CandidateId retired_id = crowded_retention_committed.retired.front();
  const CandidateRecord retired = af_test::load_candidate(af_ctx, crowded.core, retired_id);
  EXPECT_EQ(af_ctx, retired.state, CandidateState::Retired);
  EXPECT_TRUE(af_ctx, candidate_state_is_terminal(retired.state));
  const Status late = crowded.core.record_evaluation(
      make_record(af_ctx, crowded.core, retired, kEvaluatorCompileAndRun,
                  EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                  RequirementClass::Mandatory, false, 0.0));
  EXPECT_STATUS_CODE(af_ctx, late, ErrorCode::LateCompletion);
  EXPECT_TRUE(af_ctx, crowded_selection.has_winner());
  EXPECT_TRUE(af_ctx, crowded_committed.generation.valid());

  af_ctx.phase("VERIFY_RETENTION_ACROSS_A_NEW_SELECTION");
  // Committing a selection leaves the population Advancing, and Advancing has no
  // edge to Selecting, so a second selection round needs the documented
  // revalidation to return the population to Running first.
  close_round(af_ctx, fixture.core, fixture.population);
  REQUIRE_VALUE(af_ctx, PopulationRecord, reopened,
                fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, reopened.state, PopulationState::Running);

  // A retention decision prepared against an earlier selection generation is
  // refused once a later selection is committed.
  const Result<SelectionDecision> second_prepared =
      fixture.core.prepare_selection(fixture.population);
  REQUIRE_VALUE(af_ctx, SelectionDecision, second_decision, second_prepared);
  const Result<SelectionDecision> second_committed =
      fixture.core.commit_selection(second_decision);
  REQUIRE_VALUE(af_ctx, SelectionDecision, recommitted, second_committed);
  EXPECT_TRUE(af_ctx, recommitted.generation > committed_selection.generation);
  const Result<RetentionDecision> across_generations =
      fixture.core.commit_retention(retention);
  // The decision is refused as stale. Which stale code is reported depends on
  // which bound the revalidation reaches first: the population generation
  // advanced when the round was reopened, and the selection generation advanced
  // when the second selection committed.
  EXPECT_STATUS_IN(af_ctx, across_generations, ErrorCode::StalePopulationGeneration,
                   ErrorCode::StaleSelection);
  EXPECT_TRUE(af_ctx, is_stale_authority(across_generations.status().code()));
  // The read path reports the most recently prepared retention decision, and
  // that decision was prepared after the commit above, so it is the prepared
  // value and not the committed one. What the refused replay must not have
  // changed is the population's own durable record that retention was committed
  // for the earlier selection generation.
  REQUIRE_VALUE(af_ctx, RetentionDecision, stored_retention,
                fixture.core.retention(fixture.population));
  EXPECT_TRUE(af_ctx, stored_retention == fresh_retention);
  REQUIRE_VALUE(af_ctx, PopulationRecord, after_replay,
                fixture.core.population(fixture.population));
  EXPECT_TRUE(af_ctx, after_replay.retention_committed);
  EXPECT_EQ(af_ctx, after_replay.retention_selection_generation,
            committed_retention.selection_generation);
  EXPECT_TRUE(af_ctx, after_replay.committed_selection.value() >
                          committed_retention.selection_generation.value());

  af_ctx.phase("VERIFY_PROMOTION_REPLAY");
  const Result<PromotionRequest> request =
      fixture.core.prepare_promotion_request(fixture.population);
  REQUIRE_VALUE(af_ctx, PromotionRequest, first_request, request);
  EXPECT_EQ(af_ctx, first_request.eligibility, PromotionEligibilityState::Eligible);
  EXPECT_FALSE(af_ctx, first_request.canonical_state_digest.empty());

  PromotionReceipt receipt;
  receipt.handoff = PromotionHandoffState::AcceptedForProcessing;
  receipt.sink_name = "reference-local-sink";
  receipt.sink_reference = "sink-1";
  receipt.detail = "accepted for the sink's own processing";
  receipt.request_digest = first_request.canonical_state_digest;
  const Status recorded = fixture.core.record_promotion_receipt(first_request.id, receipt);
  EXPECT_OK(af_ctx, recorded);
  // A receipt is a statement about the receiving system's own processing, not
  // durable authority: replaying it cannot manufacture a promotion.
  const Status replayed = fixture.core.record_promotion_receipt(first_request.id, receipt);
  EXPECT_OK(af_ctx, replayed);
  REQUIRE_VALUE(af_ctx, PromotionRequest, reread,
                fixture.core.promotion_request(first_request.id));
  EXPECT_EQ(af_ctx, reread.canonical_state_digest, first_request.canonical_state_digest);
  EXPECT_EQ(af_ctx, reread.eligibility, first_request.eligibility);
  EXPECT_EQ(af_ctx, encode_promotion_request_canonical(reread),
            encode_promotion_request_canonical(first_request));
  const CandidateRecord winner =
      af_test::load_candidate(af_ctx, fixture.core, first_request.candidate);
  EXPECT_FALSE(af_ctx, winner.promotion_requested);
  EXPECT_FALSE(af_ctx, winner.promotion_eligible);

  af_ctx.phase("VERIFY_PROMOTION_REQUEST_IS_A_NEW_IDENTITY");
  const Result<PromotionRequest> second_request =
      fixture.core.prepare_promotion_request(fixture.population);
  REQUIRE_VALUE(af_ctx, PromotionRequest, next_request, second_request);
  EXPECT_TRUE(af_ctx, next_request.id != first_request.id);
  EXPECT_TRUE(af_ctx, next_request.id.valid());
  REQUIRE_VALUE(af_ctx, PopulationRecord, current_population,
                fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, current_population.committed_promotion_request, next_request.id);

  af_ctx.phase("VERIFY_MISMATCHED_RECEIPT_IS_REFUSED");
  PromotionReceipt mismatched = receipt;
  mismatched.request_digest = first_request.canonical_state_digest + "0";
  const Status refused = fixture.core.record_promotion_receipt(first_request.id, mismatched);
  EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::StaleAuthority);
  PromotionReceipt no_handoff;
  const Status empty = fixture.core.record_promotion_receipt(first_request.id, no_handoff);
  EXPECT_STATUS_CODE(af_ctx, empty, ErrorCode::InvalidArgument);
  const Status unknown = fixture.core.record_promotion_receipt(
      PromotionRequestId::from_raw(0x0F000001000000FFull), receipt);
  EXPECT_STATUS_CODE(af_ctx, unknown, ErrorCode::UnknownIdentity);
}

AF_TEST_CASE(adversarial, corrupt_and_truncated_snapshot_images_are_refused) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(7008));
  af_test::build_running_fixture(af_ctx, fixture, 3);

  af_ctx.phase("DISPATCH");
  std::vector<CandidateId> candidates;
  for (std::uint32_t slot = 1; slot <= 2; ++slot) {
    const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, slot);
    candidates.push_back(produce_evaluated(
        af_ctx, fixture.core, session,
        af_test::reference_solution_source(static_cast<int>(slot - 1)),
        "strategy-" + std::to_string(slot), slot < 2));
  }
  af_ctx.phase("COMMIT");
  REQUIRE_VALUE(af_ctx, SelectionDecision, selection,
                fixture.core.prepare_selection(fixture.population));
  REQUIRE_VALUE(af_ctx, SelectionDecision, committed_selection,
                fixture.core.commit_selection(selection));
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention,
                fixture.core.prepare_retention(fixture.population));
  REQUIRE_VALUE(af_ctx, RetentionDecision, committed_retention,
                fixture.core.commit_retention(retention));

  af_ctx.phase("SERIALIZE");
  {
    // The core accepts caller-minted worker identities and never advances its own
    // Worker counter, so a raw image of this state is refused at parse time
    // unless the counter its state requires is declared and installed through
    // the documented recovery path. Either outcome is acceptable here; silently
    // loading a different state is not.
    const FoundrySnapshot raw = fixture.core.snapshot();
    REQUIRE_VALUE(af_ctx, std::string, raw_image, serialize_snapshot(raw));
    const Result<FoundrySnapshot> reloaded = deserialize_snapshot(raw_image);
    if (reloaded.ok()) {
      EXPECT_TRUE(af_ctx, snapshots_are_equal(raw, reloaded.value()));
    } else {
      EXPECT_STATUS_CODE(af_ctx, reloaded, ErrorCode::PersistenceRejectedContent);
      EXPECT_TRUE(af_ctx, contains(reloaded.status().message(), "Worker"));
    }
  }

  FoundrySnapshot declared = fixture.core.snapshot();
  std::uint32_t worker_counter = declared.id_counters[static_cast<std::size_t>(IdKind::Worker)];
  for (const auto& entry : declared.workers) {
    worker_counter = std::max(worker_counter, entry.first.counter());
  }
  declared.id_counters[static_cast<std::size_t>(IdKind::Worker)] = worker_counter;
  EXPECT_OK(af_ctx, fixture.core.recover(declared));

  const FoundrySnapshot snapshot = fixture.core.snapshot();
  REQUIRE_VALUE(af_ctx, std::string, image, serialize_snapshot(snapshot));
  REQUIRE_VALUE(af_ctx, FoundrySnapshot, baseline, deserialize_snapshot(image));
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, baseline));
  EXPECT_TRUE(af_ctx, read_u64_le(image, 8) >= 4);

  af_ctx.phase("VERIFY_TRUNCATION");
  {
    const std::string partial_header = image.substr(0, 8);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(partial_header);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceTruncated);
  }
  {
    const std::string cut_payload = image.substr(0, image.size() - 1);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(cut_payload);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceTruncated);
    EXPECT_TRUE(af_ctx, is_persistence_rejection(refused.status().code()));
  }
  {
    const std::string trailing = image + std::string("AFSN-trailing");
    const Result<FoundrySnapshot> refused = deserialize_snapshot(trailing);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceTrailingGarbage);
  }

  af_ctx.phase("VERIFY_HEADER_TAMPERING");
  {
    std::string bad_magic = image;
    write_u32_le(bad_magic, 0, 0x58585858u);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(bad_magic);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceCorrupt);
  }
  {
    std::string bad_version = image;
    write_u32_le(bad_version, 4, 0xFFFFFFFFu);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(bad_version);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceVersionUnsupported);
    EXPECT_TRUE(af_ctx, is_persistence_rejection(refused.status().code()));
  }
  {
    std::string bad_reserved = image;
    write_u32_le(bad_reserved, 20, 1u);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(bad_reserved);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceCorrupt);
  }
  {
    std::string bad_length = image;
    write_u64_le(bad_length, 8, read_u64_le(image, 8) + 4096u);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(bad_length);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceTruncated);
  }

  af_ctx.phase("VERIFY_INTEGRITY_CHECK");
  {
    std::string flipped = image;
    flipped[kSnapshotHeaderBytes] = static_cast<char>(
        static_cast<unsigned char>(flipped[kSnapshotHeaderBytes]) ^ 0x01u);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(flipped);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceIntegrityMismatch);
    EXPECT_TRUE(af_ctx, is_persistence_rejection(refused.status().code()));
    EXPECT_TRUE(af_ctx, contains(refused.status().message(), "CRC-32C"));
  }

  af_ctx.phase("VERIFY_ABSURD_RECORD_COUNT");
  {
    std::string absurd = image;
    write_u32_le(absurd, kRecordCountOffset, 0xFFFFFFFFu);
    refresh_payload_crc(absurd);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(absurd);
    // The count is rejected by whichever bound is reached first: the collection
    // size check against the bytes that remain, or the absolute record ceiling.
    EXPECT_STATUS_IN(af_ctx, refused, ErrorCode::MalformedEncoding,
                     ErrorCode::PersistenceTruncated);
    EXPECT_TRUE(af_ctx, contains(refused.status().message(), std::to_string(0xFFFFFFFFu)));
  }

  af_ctx.phase("VERIFY_ABSURD_RECORD_BODY_LENGTH");
  {
    std::string absurd = image;
    write_u32_le(absurd, kFirstRecordBodyLengthOffset, 0xFFFFFFF0u);
    refresh_payload_crc(absurd);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(absurd);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceTruncated);
  }
  {
    std::string absurd = image;
    write_u32_le(absurd, kFirstRecordBodyLengthOffset, 4u);
    refresh_payload_crc(absurd);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(absurd);
    EXPECT_NOT_OK(af_ctx, refused);
  }

  af_ctx.phase("VERIFY_UNKNOWN_RECORD_KIND");
  {
    std::string absurd = image;
    absurd[kFirstRecordTypeOffset] = static_cast<char>(0x7F);
    refresh_payload_crc(absurd);
    const Result<FoundrySnapshot> refused = deserialize_snapshot(absurd);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::PersistenceRejectedContent);
  }

  af_ctx.phase("VERIFY_ORIGINAL_IMAGE_STILL_LOADS");
  REQUIRE_VALUE(af_ctx, FoundrySnapshot, intact, deserialize_snapshot(image));
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, intact));
  EXPECT_EQ(af_ctx, intact.revision, snapshot.revision);
  EXPECT_EQ(af_ctx, intact.candidates.size(), snapshot.candidates.size());
}

AF_TEST_CASE(adversarial, malformed_frames_are_refused) {
  af_ctx.phase("SETUP");
  FrameHeader header;
  header.type = MessageType::Heartbeat;
  header.sequence = 7;
  header.flags = 0;
  const std::string payload = "heartbeat-payload";
  const std::string frame = encode_frame(header, payload);
  EXPECT_FALSE(af_ctx, frame.empty());
  EXPECT_EQ(af_ctx, frame.size(), kFrameHeaderBytes + payload.size());

  af_ctx.phase("VERIFY_VALID_FRAME");
  std::size_t consumed = 0;
  const Result<Frame> decoded = decode_frame(frame, &consumed);
  EXPECT_OK(af_ctx, decoded);
  EXPECT_EQ(af_ctx, consumed, frame.size());
  EXPECT_EQ(af_ctx, decoded.value().payload, payload);
  EXPECT_EQ(af_ctx, decoded.value().header.type, MessageType::Heartbeat);
  EXPECT_EQ(af_ctx, decoded.value().header.sequence, std::uint64_t{7});

  const Result<Frame> no_sink = decode_frame(frame, nullptr);
  EXPECT_STATUS_CODE(af_ctx, no_sink, ErrorCode::InvalidArgument);

  af_ctx.phase("VERIFY_SPLIT_READ_IS_NOT_CORRUPTION");
  {
    std::size_t partial_consumed = 0;
    const std::string partial_header = frame.substr(0, kFrameHeaderBytes - 1);
    const Result<Frame> refused = decode_frame(partial_header, &partial_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::FrameTruncated);
    EXPECT_TRUE(af_ctx, status_is_incomplete(refused.status()));
    EXPECT_EQ(af_ctx, partial_consumed, static_cast<std::size_t>(0));

    const std::string partial_payload = frame.substr(0, frame.size() - 1);
    const Result<Frame> refused_payload = decode_frame(partial_payload, &partial_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused_payload, ErrorCode::FrameTruncated);
    EXPECT_TRUE(af_ctx, status_is_incomplete(refused_payload.status()));
  }

  af_ctx.phase("VERIFY_BAD_MAGIC");
  {
    std::string bad_magic = frame;
    write_u32_le(bad_magic, 0, 0x58585858u);
    std::size_t bad_consumed = 0;
    const Result<Frame> refused = decode_frame(bad_magic, &bad_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::ProtocolViolation);
    EXPECT_FALSE(af_ctx, status_is_incomplete(refused.status()));
    EXPECT_TRUE(af_ctx, contains(refused.status().message(), "magic"));
  }

  af_ctx.phase("VERIFY_BAD_VERSION");
  {
    std::string bad_version = frame;
    write_u16_le(bad_version, 4, 0xFFFFu);
    std::size_t bad_consumed = 0;
    const Result<Frame> refused = decode_frame(bad_version, &bad_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::ProtocolViolation);
  }

  af_ctx.phase("VERIFY_ABSURD_DECLARED_LENGTH");
  {
    std::string absurd = frame;
    write_u32_le(absurd, 20, 0xFFFFFFFFu);
    std::size_t absurd_consumed = 0;
    const Result<Frame> refused = decode_frame(absurd, &absurd_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::FrameTooLarge);
    EXPECT_TRUE(af_ctx, contains(refused.status().message(), "payload length"));
  }
  {
    std::string longer_than_buffer = frame;
    write_u32_le(longer_than_buffer, 20,
                 static_cast<std::uint32_t>(payload.size()) + 8u);
    std::size_t long_consumed = 0;
    const Result<Frame> refused = decode_frame(longer_than_buffer, &long_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::FrameTruncated);
  }

  af_ctx.phase("VERIFY_BAD_CRC");
  {
    std::string bad_crc = frame;
    bad_crc[kFrameHeaderBytes] = static_cast<char>(
        static_cast<unsigned char>(bad_crc[kFrameHeaderBytes]) ^ 0x01u);
    std::size_t crc_consumed = 0;
    const Result<Frame> refused = decode_frame(bad_crc, &crc_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::FrameCorrupt);
    EXPECT_TRUE(af_ctx, contains(refused.status().message(), "CRC-32C"));
  }
  {
    std::string declared = frame;
    write_u32_le(declared, 24, crc32c(payload) + 1u);
    std::size_t declared_consumed = 0;
    const Result<Frame> refused = decode_frame(declared, &declared_consumed);
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::FrameCorrupt);
  }

  af_ctx.phase("VERIFY_OVERSIZED_PAYLOAD_IS_NEVER_ENCODED");
  {
    const std::string oversized(kAbsoluteMaxFramePayloadBytes + 1u, 'x');
    const std::string refused_frame = encode_frame(header, oversized);
    EXPECT_TRUE(af_ctx, refused_frame.empty());
  }
}

AF_TEST_CASE(adversarial, artifact_names_cannot_escape_the_workspace) {
  af_ctx.phase("VERIFY_NAME_VALIDATION");
  const char* const rejected_names[] = {
      "../escape.txt",
      "..\\escape.txt",
      "a/../b.txt",
      "/etc/passwd",
      "C:\\windows\\system32\\evil.dll",
      "solution/../../evil",
      "con",
      "NUL.txt",
      ".hidden",
      "trailing.",
      "with space",
      "with:colon",
      "with\x01control",
      "",
  };
  for (const char* name : rejected_names) {
    const Status status = validate_artifact_name(name);
    EXPECT_STATUS_CODE(af_ctx, status, ErrorCode::UnsafePath);
  }
  {
    const std::string over_long(static_cast<std::size_t>(kMaxArtifactNameLength) + 1u, 'a');
    const Status status = validate_artifact_name(over_long);
    EXPECT_STATUS_CODE(af_ctx, status, ErrorCode::UnsafePath);
  }
  EXPECT_OK(af_ctx, validate_artifact_name("solution.txt"));
  EXPECT_OK(af_ctx, validate_artifact_name("reference-source.CPP"));
  EXPECT_OK(af_ctx, validate_artifact_name("a-b_c.1.2"));

  af_ctx.phase("VERIFY_REF_VALIDATION");
  {
    ArtifactRef ref;
    ref.name = "solution.txt";
    ref.size_bytes = 4;
    ref.content_digest = sha256_hex("data");
    EXPECT_OK(af_ctx, validate_artifact_ref(ref));
    ref.content_digest = "ABCDEF";
    EXPECT_STATUS_CODE(af_ctx, validate_artifact_ref(ref), ErrorCode::MalformedEncoding);
    ref.content_digest = sha256_hex("data");
    ref.size_bytes = kMaxArtifactBytes + 1u;
    EXPECT_STATUS_CODE(af_ctx, validate_artifact_ref(ref), ErrorCode::LengthOutOfRange);
  }

  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(7009));
  af_test::build_running_fixture(af_ctx, fixture, 2);

  af_ctx.phase("DISPATCH");
  const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, 1);
  const PreparedAttempt prepared = prepare_attempt(af_ctx, fixture.core, session);

  af_ctx.phase("VERIFY_ESCAPING_NAME_IS_REFUSED");
  {
    std::vector<ArtifactRef> artifacts;
    ArtifactRef escaping;
    escaping.name = "../escape.txt";
    escaping.size_bytes = 8;
    escaping.content_digest = sha256_hex("escape!!");
    artifacts.push_back(escaping);
    const Result<PublicationOutcome> refused =
        fixture.core.publish_candidate(prepared.authority, artifacts, "strategy-1");
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::UnsafePath);
  }
  {
    std::vector<ArtifactRef> artifacts;
    ArtifactRef drive;
    drive.name = "C:\\absolute.txt";
    drive.size_bytes = 4;
    drive.content_digest = sha256_hex("data");
    artifacts.push_back(drive);
    const Result<PublicationOutcome> refused =
        fixture.core.publish_candidate(prepared.authority, artifacts, "strategy-1");
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::UnsafePath);
  }
  {
    // Two artifacts claiming the same logical name is a duplicate identity, not
    // a silent overwrite.
    std::vector<ArtifactRef> artifacts = solution_artifacts("data");
    artifacts.push_back(artifacts.front());
    const Result<PublicationOutcome> refused =
        fixture.core.publish_candidate(prepared.authority, artifacts, "strategy-1");
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::DuplicateIdentity);
  }

  af_ctx.phase("VERIFY_MISSING_REQUIRED_OUTPUT_IS_REFUSED");
  {
    std::vector<ArtifactRef> artifacts;
    ArtifactRef unrelated;
    unrelated.name = "other.txt";
    unrelated.size_bytes = 4;
    unrelated.content_digest = sha256_hex("data");
    artifacts.push_back(unrelated);
    const Result<PublicationOutcome> refused =
        fixture.core.publish_candidate(prepared.authority, artifacts, "strategy-1");
    EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::MandatoryEvidenceMissing);
  }

  af_ctx.phase("VERIFY_PUBLICATION_STATE_IS_UNTOUCHED");
  {
    const CandidateRecord candidate =
        af_test::load_candidate(af_ctx, fixture.core, prepared.authority.candidate);
    EXPECT_TRUE(af_ctx, candidate.artifacts.empty());
    EXPECT_TRUE(af_ctx, candidate.artifact_set_digest.empty());
    EXPECT_EQ(af_ctx, candidate.state, CandidateState::Producing);
  }

  af_ctx.phase("VERIFY_VALID_PUBLICATION_STILL_WORKS");
  {
    const Result<PublicationOutcome> published = fixture.core.publish_candidate(
        prepared.authority, solution_artifacts(af_test::reference_solution_source(0)),
        "strategy-1");
    EXPECT_OK(af_ctx, published);
    const CandidateRecord candidate =
        af_test::load_candidate(af_ctx, fixture.core, prepared.authority.candidate);
    EXPECT_EQ(af_ctx, candidate.artifacts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(af_ctx, candidate.artifacts.front().name,
              std::string(kReferenceTaskSourceArtifact));
    EXPECT_FALSE(af_ctx, candidate.artifact_set_digest.empty());
  }
}
