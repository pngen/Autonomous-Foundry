// Authority suite.
//
// Proves that every authority check reports the documented code and a
// diagnostic that names the field, the expected value and the actual value, and
// that the check_identity template is instantiated for several identity
// domains (a template defect that only appears at instantiation is invisible to
// a suite that never instantiates it).

#include <string>
#include <string_view>

#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/id.hpp"
#include "test_support.hpp"

namespace {

using namespace autonomous_foundry;

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

/// Every authority diagnostic must name the field, the expected value and the
/// actual value. A message that omits any of them forces an operator to guess
/// which authority failed.
void expect_authority_message(const af_test::TestContext& ctx, const Status& status,
                              std::string_view field, std::string_view expected, std::string_view actual,
                              const char* file, int line) {
  if (!contains(status.message(), field)) {
    ctx.fail_at(file, line, std::string("authority message does not name the field '") +
                               std::string(field) + "': '" + status.message() + "'");
  }
  if (!contains(status.message(), expected)) {
    ctx.fail_at(file, line, std::string("authority message does not name the expected value '") +
                               std::string(expected) + "': '" + status.message() + "'");
  }
  if (!contains(status.message(), actual)) {
    ctx.fail_at(file, line, std::string("authority message does not name the actual value '") +
                               std::string(actual) + "': '" + status.message() + "'");
  }
}

#define EXPECT_AUTHORITY_MESSAGE(ctx, status, field, expected, actual)                       \
  expect_authority_message((ctx), (status), (field), (expected), (actual), __FILE__, __LINE__)

}  // namespace

AF_TEST_CASE(authority, every_check_reports_its_documented_code_and_message) {
  af_ctx.phase("VERIFY_COORDINATOR_EPOCH");
  {
    const Status ok = check_coordinator_epoch(CoordinatorEpoch::from_value(7),
                                              CoordinatorEpoch::from_value(7));
    EXPECT_OK(af_ctx, ok);
    const Status stale = check_coordinator_epoch(CoordinatorEpoch::from_value(7),
                                                 CoordinatorEpoch::from_value(9));
    EXPECT_STATUS_CODE(af_ctx, stale, ErrorCode::StaleCoordinatorEpoch);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, stale, "coordinator_epoch", "7", "9");
  }

  af_ctx.phase("VERIFY_RUN");
  {
    const FoundryRunId run_a = FoundryRunId::from_raw(0x0200000100000001ull);
    const FoundryRunId run_b = FoundryRunId::from_raw(0x0200000100000002ull);
    EXPECT_OK(af_ctx, check_run(run_a, run_a));
    const Status stale = check_run(run_a, run_b);
    EXPECT_STATUS_CODE(af_ctx, stale, ErrorCode::StaleAuthority);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, stale, "run", run_a.to_string(), run_b.to_string());
  }

  af_ctx.phase("VERIFY_WORKER_BOOT");
  {
    const WorkerBootId boot_a = WorkerBootId::from_raw(0x0800000000000011ull);
    const WorkerBootId boot_b = WorkerBootId::from_raw(0x0800000000000022ull);
    EXPECT_OK(af_ctx, check_worker_boot(boot_a, boot_a));
    const Status stale = check_worker_boot(boot_a, boot_b);
    EXPECT_STATUS_CODE(af_ctx, stale, ErrorCode::StaleWorkerBoot);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, stale, "worker_boot", boot_a.to_string(), boot_b.to_string());
  }

  af_ctx.phase("VERIFY_SESSION");
  {
    const SessionId session_a = SessionId::from_raw(0x1000000100000001ull);
    const SessionId session_b = SessionId::from_raw(0x1000000100000002ull);
    EXPECT_OK(af_ctx, check_session(session_a, session_a));
    const Status stale = check_session(session_a, session_b);
    EXPECT_STATUS_CODE(af_ctx, stale, ErrorCode::StaleSession);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, stale, "session", session_a.to_string(), session_b.to_string());

    const WorkerSessionGeneration generation_a = WorkerSessionGeneration::first();
    const WorkerSessionGeneration generation_b = WorkerSessionGeneration::from_value(4);
    EXPECT_OK(af_ctx, check_session_generation(generation_a, generation_a));
    const Status stale_generation = check_session_generation(generation_a, generation_b);
    EXPECT_STATUS_CODE(af_ctx, stale_generation, ErrorCode::StaleSession);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, stale_generation, "session_generation", "1", "4");
  }

  af_ctx.phase("VERIFY_DOMAIN_GENERATIONS");
  {
    EXPECT_OK(af_ctx, check_population_generation(PopulationGeneration::from_value(3),
                                                  PopulationGeneration::from_value(3)));
    const Status population_stale = check_population_generation(PopulationGeneration::from_value(3),
                                                                PopulationGeneration::from_value(5));
    EXPECT_STATUS_CODE(af_ctx, population_stale, ErrorCode::StalePopulationGeneration);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, population_stale, "population_generation", "3", "5");

    const Status task_stale =
        check_task_generation(TaskGeneration::from_value(1), TaskGeneration::from_value(2));
    EXPECT_STATUS_CODE(af_ctx, task_stale, ErrorCode::StaleTaskGeneration);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, task_stale, "task_generation", "1", "2");

    const Status candidate_stale = check_candidate_generation(CandidateGeneration::from_value(9),
                                                              CandidateGeneration::from_value(10));
    EXPECT_STATUS_CODE(af_ctx, candidate_stale, ErrorCode::StaleCandidateGeneration);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, candidate_stale, "candidate_generation", "9", "10");

    const Status evaluation_stale =
        check_evaluation_generation(EvaluationGeneration::from_value(4),
                                    EvaluationGeneration::from_value(6));
    EXPECT_STATUS_CODE(af_ctx, evaluation_stale, ErrorCode::StaleEvaluationGeneration);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, evaluation_stale, "evaluation_generation", "4", "6");

    const Status policy_stale =
        check_policy_generation(PolicyGeneration::from_value(2), PolicyGeneration::from_value(3));
    EXPECT_STATUS_CODE(af_ctx, policy_stale, ErrorCode::StalePolicyGeneration);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, policy_stale, "policy_generation", "2", "3");

    const Status attempt_stale =
        check_attempt_generation(AttemptGeneration::from_value(11), AttemptGeneration::from_value(12));
    EXPECT_STATUS_CODE(af_ctx, attempt_stale, ErrorCode::StaleAttempt);
    EXPECT_AUTHORITY_MESSAGE(af_ctx, attempt_stale, "attempt_generation", "11", "12");

    // Equal values are accepted for every domain.
    EXPECT_OK(af_ctx, check_population_generation(PopulationGeneration::from_value(3),
                                                  PopulationGeneration::from_value(3)));
    EXPECT_OK(af_ctx, check_task_generation(TaskGeneration::from_value(1),
                                            TaskGeneration::from_value(1)));
    EXPECT_OK(af_ctx, check_candidate_generation(CandidateGeneration::from_value(9),
                                                 CandidateGeneration::from_value(9)));
    EXPECT_OK(af_ctx, check_evaluation_generation(EvaluationGeneration::from_value(4),
                                                  EvaluationGeneration::from_value(4)));
    EXPECT_OK(af_ctx, check_policy_generation(PolicyGeneration::from_value(2),
                                              PolicyGeneration::from_value(2)));
    EXPECT_OK(af_ctx, check_attempt_generation(AttemptGeneration::from_value(11),
                                               AttemptGeneration::from_value(11)));
  }

  af_ctx.phase("VERIFY_STALE_CLASSIFICATION");
  // The stale predicate must agree with the codes the checks actually report,
  // because callers use it to decide whether a revalidation is required.
  EXPECT_TRUE(af_ctx, is_stale_authority(ErrorCode::StaleCoordinatorEpoch));
  EXPECT_TRUE(af_ctx, is_stale_authority(ErrorCode::StaleAuthority));
  EXPECT_TRUE(af_ctx, is_stale_authority(ErrorCode::StaleSelection));
  EXPECT_TRUE(af_ctx, is_stale_authority(ErrorCode::StalePopulationGeneration));
  EXPECT_TRUE(af_ctx, is_stale_authority(ErrorCode::StaleCandidateGeneration));
  EXPECT_TRUE(af_ctx, is_stale_authority(ErrorCode::StaleAttempt));
  EXPECT_TRUE(af_ctx, is_stale_authority(ErrorCode::GenerationRegression));
  EXPECT_FALSE(af_ctx, is_stale_authority(ErrorCode::Ok));
  EXPECT_FALSE(af_ctx, is_stale_authority(ErrorCode::NullIdentity));
  EXPECT_FALSE(af_ctx, is_stale_authority(ErrorCode::IllegalStateTransition));
  EXPECT_FALSE(af_ctx, is_stale_authority(ErrorCode::PersistenceCorrupt));
}

AF_TEST_CASE(authority, check_identity_is_instantiated_for_several_domains) {
  af_ctx.phase("VERIFY");
  // The template is instantiated for four domains with four different field
  // names. A defect that only appears at instantiation would be invisible to a
  // suite that exercised a single domain.
  const PopulationId population_a = PopulationId::from_raw(0x0300000100000001ull);
  const PopulationId population_b = PopulationId::from_raw(0x0300000100000002ull);
  EXPECT_OK(af_ctx, check_identity(population_a, population_a, "population"));
  const Status population_mismatch = check_identity(population_a, population_b, "population");
  EXPECT_STATUS_CODE(af_ctx, population_mismatch, ErrorCode::StaleAuthority);
  EXPECT_AUTHORITY_MESSAGE(af_ctx, population_mismatch, "population", population_a.to_string(),
                           population_b.to_string());

  const TaskId task_a = TaskId::from_raw(0x0400000100000001ull);
  const TaskId task_b = TaskId::from_raw(0x0400000100000002ull);
  EXPECT_OK(af_ctx, check_identity(task_a, task_a, "task"));
  const Status task_mismatch = check_identity(task_a, task_b, "task");
  EXPECT_STATUS_CODE(af_ctx, task_mismatch, ErrorCode::StaleAuthority);
  EXPECT_AUTHORITY_MESSAGE(af_ctx, task_mismatch, "task", task_a.to_string(), task_b.to_string());

  const WorkerId worker_a = WorkerId::from_raw(0x0700000100000001ull);
  const WorkerId worker_b = WorkerId::from_raw(0x0700000100000002ull);
  EXPECT_OK(af_ctx, check_identity(worker_a, worker_a, "worker"));
  const Status worker_mismatch = check_identity(worker_a, worker_b, "worker");
  EXPECT_STATUS_CODE(af_ctx, worker_mismatch, ErrorCode::StaleAuthority);
  EXPECT_AUTHORITY_MESSAGE(af_ctx, worker_mismatch, "worker", worker_a.to_string(),
                           worker_b.to_string());

  const AssignmentId assignment_a = AssignmentId::from_raw(0x0A00000100000001ull);
  const AssignmentId assignment_b = AssignmentId::from_raw(0x0A00000100000002ull);
  EXPECT_OK(af_ctx, check_identity(assignment_a, assignment_a, "assignment"));
  const Status assignment_mismatch = check_identity(assignment_a, assignment_b, "assignment");
  EXPECT_STATUS_CODE(af_ctx, assignment_mismatch, ErrorCode::StaleAuthority);
  EXPECT_AUTHORITY_MESSAGE(af_ctx, assignment_mismatch, "assignment", assignment_a.to_string(),
                           assignment_b.to_string());

  // A null identity is a value like any other: the check reports a mismatch
  // rather than silently accepting "no identity" as "the same identity".
  const LineageId lineage = LineageId::from_raw(0x0600000100000001ull);
  const Status null_mismatch = check_identity(LineageId(), lineage, "lineage");
  EXPECT_STATUS_CODE(af_ctx, null_mismatch, ErrorCode::StaleAuthority);
  EXPECT_AUTHORITY_MESSAGE(af_ctx, null_mismatch, "lineage", LineageId().to_string(),
                           lineage.to_string());
}

AF_TEST_CASE(authority, canonical_digest_rejects_empty_expected_value) {
  af_ctx.phase("VERIFY");
  const std::string current = "9f2c4c0c1f5a3d4e5b6c7d8e9f00112233445566778899aabbccddeeff001122";

  // Matching digests pass.
  EXPECT_OK(af_ctx, check_canonical_digest(current, current));
  EXPECT_OK(af_ctx, check_canonical_digest("", ""));

  // An empty expected digest means "this decision was never bound to any state
  // at all", which is a stale selection rather than a generic argument error.
  const Status empty_expected = check_canonical_digest("", current);
  EXPECT_STATUS_CODE(af_ctx, empty_expected, ErrorCode::StaleSelection);
  EXPECT_TRUE(af_ctx, contains(empty_expected.message(), "canonical_state_digest"));
  EXPECT_TRUE(af_ctx, contains(empty_expected.message(), "no state digest"));
  EXPECT_TRUE(af_ctx, contains(empty_expected.message(), current));

  // An empty actual digest is a state that lost its digest: also stale.
  const Status empty_actual = check_canonical_digest(current, "");
  EXPECT_STATUS_CODE(af_ctx, empty_actual, ErrorCode::StaleSelection);
  EXPECT_TRUE(af_ctx, contains(empty_actual.message(), "canonical_state_digest"));

  // Differing digests name both values.
  const Status mismatch = check_canonical_digest(current, "0000");
  EXPECT_STATUS_CODE(af_ctx, mismatch, ErrorCode::StaleSelection);
  EXPECT_TRUE(af_ctx, contains(mismatch.message(), current));
  EXPECT_TRUE(af_ctx, contains(mismatch.message(), "0000"));
  EXPECT_TRUE(af_ctx, is_stale_authority(mismatch.code()));
}

AF_TEST_CASE(authority, authority_structs_capture_the_whole_boundary) {
  af_ctx.phase("VERIFY");
  // A worker operation authority that differs in exactly one generation field
  // must compare unequal: nothing may be silently ignored by equality.
  WorkerOperationAuthority base;
  base.session.coordinator_epoch = CoordinatorEpoch::from_value(3);
  base.session.run = FoundryRunId::from_raw(0x0200000100000001ull);
  base.session.worker = WorkerId::from_raw(0x0700000100000001ull);
  base.session.boot = WorkerBootId::from_raw(0x0800000000000001ull);
  base.session.session = SessionId::from_raw(0x1000000100000001ull);
  base.session.session_generation = WorkerSessionGeneration::first();
  base.population = PopulationId::from_raw(0x0300000100000001ull);
  base.population_generation = PopulationGeneration::first();
  base.task = TaskId::from_raw(0x0400000100000001ull);
  base.task_generation = TaskGeneration::first();
  base.candidate = CandidateId::from_raw(0x0500000100000001ull);
  base.candidate_generation = CandidateGeneration::first();
  base.attempt = AttemptId::from_raw(0x0900000100000001ull);
  base.attempt_generation = AttemptGeneration::first();
  base.assignment = AssignmentId::from_raw(0x0A00000100000001ull);

  WorkerOperationAuthority same = base;
  EXPECT_TRUE(af_ctx, same == base);

  WorkerOperationAuthority shifted_epoch = base;
  shifted_epoch.session.coordinator_epoch = CoordinatorEpoch::from_value(4);
  EXPECT_TRUE(af_ctx, shifted_epoch != base);

  WorkerOperationAuthority shifted_boot = base;
  shifted_boot.session.boot = WorkerBootId::from_raw(0x0800000000000002ull);
  EXPECT_TRUE(af_ctx, shifted_boot != base);

  WorkerOperationAuthority shifted_session_generation = base;
  shifted_session_generation.session.session_generation = WorkerSessionGeneration::from_value(9);
  EXPECT_TRUE(af_ctx, shifted_session_generation != base);

  WorkerOperationAuthority shifted_population = base;
  shifted_population.population_generation = PopulationGeneration::from_value(2);
  EXPECT_TRUE(af_ctx, shifted_population != base);

  WorkerOperationAuthority shifted_task = base;
  shifted_task.task_generation = TaskGeneration::from_value(2);
  EXPECT_TRUE(af_ctx, shifted_task != base);

  WorkerOperationAuthority shifted_candidate = base;
  shifted_candidate.candidate_generation = CandidateGeneration::from_value(2);
  EXPECT_TRUE(af_ctx, shifted_candidate != base);

  WorkerOperationAuthority shifted_attempt = base;
  shifted_attempt.attempt = AttemptId::from_raw(0x0900000100000002ull);
  EXPECT_TRUE(af_ctx, shifted_attempt != base);

  WorkerOperationAuthority shifted_assignment = base;
  shifted_assignment.assignment = AssignmentId::from_raw(0x0A00000100000002ull);
  EXPECT_TRUE(af_ctx, shifted_assignment != base);

  af_ctx.phase("VERIFY_PREPARED_DECISION_AUTHORITY");
  PreparedDecisionAuthority decision;
  decision.coordinator_epoch = CoordinatorEpoch::from_value(3);
  decision.population = base.population;
  decision.population_generation = PopulationGeneration::first();
  decision.policy = PolicyId::from_raw(0x0D00000100000001ull);
  decision.policy_generation = PolicyGeneration::first();
  decision.canonical_state_digest = "abc";
  PreparedDecisionAuthority decision_copy = decision;
  EXPECT_TRUE(af_ctx, decision_copy == decision);
  decision_copy.canonical_state_digest = "abd";
  EXPECT_TRUE(af_ctx, decision_copy != decision);

  af_ctx.phase("VERIFY_CONTROLLER_AUTHORITY");
  ControllerOperationAuthority controller;
  controller.session.coordinator_epoch = CoordinatorEpoch::from_value(3);
  controller.session.run = base.session.run;
  controller.session.controller = ControllerId::from_raw(0x1100000100000001ull);
  controller.session.session = SessionId::from_raw(0x1000000100000002ull);
  controller.session.session_generation = WorkerSessionGeneration::first();
  controller.population = base.population;
  controller.population_generation = PopulationGeneration::first();
  controller.task = base.task;
  controller.task_generation = TaskGeneration::first();
  controller.policy = decision.policy;
  controller.policy_generation = PolicyGeneration::first();
  ControllerOperationAuthority controller_copy = controller;
  EXPECT_TRUE(af_ctx, controller_copy == controller);
  controller_copy.policy_generation = PolicyGeneration::from_value(2);
  EXPECT_TRUE(af_ctx, controller_copy != controller);
}
