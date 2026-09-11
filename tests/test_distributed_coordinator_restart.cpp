// Distributed coordinator-restart suite.
//
// These cases run the REAL af_coordinator executable as a child process with a
// durable --state file, kill it hard, and start a second af_coordinator on the
// same state file.
//
// What the suite proves, and how:
//
//   * the coordinator epoch advanced by exactly one across the restart, so every
//     authority the previous incumbent issued is dead by construction;
//   * a session bearing the old epoch is refused with StaleCoordinatorEpoch over
//     the wire, and a restored worker incarnation must revalidate before it may
//     act again;
//   * work that was durably dispatched when the process died is restored as
//     ambiguous (OutcomeUnknown) or Cancelled, never as a success or a failure;
//   * a candidate whose output was published but whose evidence never completed
//     is restored as RevalidationRequired;
//   * a recovered population refuses to select until it is explicitly
//     revalidated, and the durable snapshot is otherwise intact.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/worker.hpp"
#include "distributed_support.hpp"
#include "test_support.hpp"

using namespace autonomous_foundry;  // NOLINT(google-build-using-namespace)
using namespace af_dist;             // NOLINT(google-build-using-namespace)

namespace {

/// The task, policy and population a restart case builds once and then keeps
/// across the process boundary.
struct RestartFixture {
  std::filesystem::path state_path;
  std::filesystem::path workspace;
  std::uint16_t port{0};
  PolicyId policy;
  PolicyGeneration policy_generation;
  TaskId task;
  TaskGeneration task_generation;
  PopulationId population;
  PopulationGeneration population_generation;
  PopulationBinding binding;
};

/// Define the policy and the task through the control plane and create the
/// population. Everything here is durable state, so it must survive the restart
/// unchanged.
inline void build_population(const af_test::TestContext& context, ProtocolClient& controller,
                             RestartFixture& fixture, std::uint32_t salt, const TaskSpec& task_spec,
                             std::uint32_t candidate_budget, std::uint32_t worker_budget,
                             std::uint32_t population_index) {
  Result<PolicyDefinedMessage> defined = define_policy(
      context, controller,
      make_reference_policy(placeholder_policy_id(salt), PolicyGeneration::first()));
  AF_DIST_REQUIRE_OK(context, defined);
  fixture.policy = defined.value().policy;
  fixture.policy_generation = defined.value().generation;

  TaskSpec task = task_spec;
  task.policy = fixture.policy;
  task.policy_generation = fixture.policy_generation;
  Result<TaskCreatedMessage> created = create_task(context, controller, task);
  AF_DIST_REQUIRE_OK(context, created);
  fixture.task = created.value().task;
  fixture.task_generation = created.value().generation;

  PopulationSpec spec;
  spec.name = "P" + std::to_string(population_index);
  spec.task = fixture.task;
  spec.task_generation = fixture.task_generation;
  spec.policy = fixture.policy;
  spec.policy_generation = fixture.policy_generation;
  spec.candidate_budget = candidate_budget;
  spec.worker_budget = worker_budget;
  spec.population_index = population_index;
  Result<PopulationCreatedMessage> population = create_population(context, controller, spec);
  AF_DIST_REQUIRE_OK(context, population);
  fixture.population = population.value().population;
  fixture.population_generation = population.value().generation;
  fixture.binding = population_binding(fixture.population, fixture.population_generation,
                                       fixture.task, fixture.task_generation, fixture.policy,
                                       fixture.policy_generation);
}

}  // namespace

AF_TEST_CASE(distributed_coordinator_restart,
             restart_advances_the_epoch_and_refuses_authority_from_the_previous_incumbent) {
  af_ctx.phase("SETUP");
  const std::filesystem::path worker_exe =
      require_executable(af_ctx, locate_executable("AF_WORKER_EXE", "af_worker.exe"), "af_worker");
  const std::filesystem::path coordinator_exe =
      require_executable(af_ctx, locate_executable("AF_COORDINATOR_EXE", "af_coordinator.exe"),
                         "af_coordinator");

  af_test::TempDirectory workdir("af-coordinator-restart");
  RestartFixture fixture;
  fixture.state_path = workdir.child("coordinator.state");
  fixture.workspace = workdir.child("workspace");

  Result<std::uint16_t> first_port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, first_port);
  fixture.port = first_port.value();

  ChildProcess first_coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(first_coordinator, coordinator_exe, fixture.port,
                                               fixture.state_path, fixture.workspace, true, 4u));

  ProtocolClient first_controller;
  connect_client(af_ctx, first_controller, first_coordinator, fixture.port, "the coordinator");
  Result<HelloAckMessage> first_ack =
      first_controller.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(),
                             "distributed-tests", "controller");
  AF_DIST_REQUIRE_OK(af_ctx, first_ack);
  const CoordinatorEpoch first_epoch = first_ack.value().coordinator_epoch;
  const FoundryRunId run = first_ack.value().run;
  EXPECT_TRUE(af_ctx, first_epoch.valid());
  af_ctx.note("first incumbent epoch " + first_epoch.to_string());

  Result<TaskSpec> reference =
      reference_task(0x31u, placeholder_policy_id(0x31u), PolicyGeneration::first(), 4u);
  AF_DIST_REQUIRE_OK(af_ctx, reference);
  add_bulk_inputs(reference.value(), 8u, 128u * 1024u);
  build_population(af_ctx, first_controller, fixture, 0x31u, reference.value(), 4u, 1u, 1u);

  const WorkerId durable_worker = worker_identity(0x31u, 1u);
  ChildProcess worker;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(worker, worker_exe, fixture.port, durable_worker,
                                          fixture.workspace, "closed-form", "worker-restarted"));

  af_ctx.phase("DISPATCH");
  AF_DIST_REQUIRE_OK(
      af_ctx, start_population(af_ctx, first_controller, fixture.binding));

  af_ctx.note("waiting for durable work to exist before the restart");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, first_coordinator, "the coordinator");
    require_running(af_ctx, worker, "the worker");
    Result<std::vector<CandidateId>> ids =
        population_candidates(af_ctx, first_controller, fixture.population);
    if (!ids.ok() || ids.value().empty()) {
      return false;
    }
    Result<CandidateDetailMessage> detail =
        query_candidate(af_ctx, first_controller, ids.value().front());
    if (!detail.ok()) {
      return false;
    }
    return candidate_state_at_least_published(detail.value().candidate.state);
  });

  Result<std::vector<CandidateId>> pre_restart_ids =
      population_candidates(af_ctx, first_controller, fixture.population);
  AF_DIST_REQUIRE_OK(af_ctx, pre_restart_ids);
  const CandidateId survivor_candidate = pre_restart_ids.value().front();
  Result<CandidateDetailMessage> pre_restart =
      query_candidate(af_ctx, first_controller, survivor_candidate);
  AF_DIST_REQUIRE_OK(af_ctx, pre_restart);
  const WorkerBootId first_boot = pre_restart.value().candidate.producer_boot;
  const std::string artifact_digest = pre_restart.value().candidate.artifact_set_digest;
  EXPECT_TRUE(af_ctx, first_boot.valid());
  EXPECT_FALSE(af_ctx, artifact_digest.empty());
  Result<StatisticsDetailMessage> pre_stats = query_statistics(af_ctx, first_controller);
  AF_DIST_REQUIRE_OK(af_ctx, pre_stats);
  af_ctx.note("durable work before the restart: candidate " + survivor_candidate.to_string() +
              " boot " + first_boot.to_string());

  af_ctx.phase("RESTART");
  af_ctx.note("killing the coordinator process " + std::to_string(first_coordinator.pid()));
  AF_DIST_REQUIRE_OK(af_ctx, first_coordinator.kill());
  EXPECT_FALSE(af_ctx, process_is_alive(first_coordinator.pid()));
  first_controller.close();
  first_coordinator.close();

  Result<std::uint16_t> second_port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, second_port);
  ChildProcess second_coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(second_coordinator, coordinator_exe,
                                               second_port.value(), fixture.state_path,
                                               fixture.workspace, false, 4u));

  ProtocolClient second_controller;
  connect_client(af_ctx, second_controller, second_coordinator, second_port.value(),
                 "the restarted coordinator");
  Result<HelloAckMessage> second_ack =
      second_controller.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(),
                              "distributed-tests", "controller");
  AF_DIST_REQUIRE_OK(af_ctx, second_ack);
  const CoordinatorEpoch second_epoch = second_ack.value().coordinator_epoch;

  af_ctx.phase("VERIFY");
  EXPECT_EQ(af_ctx, second_epoch.value(), first_epoch.value() + 1u);
  EXPECT_EQ(af_ctx, second_ack.value().run, run);
  af_ctx.note("second incumbent epoch " + second_epoch.to_string());

  // The durable snapshot is intact: the same population, the same candidate, the
  // same producer incarnation and the same published artifact set.
  Result<PopulationDetailMessage> recovered_population =
      query_population(af_ctx, second_controller, fixture.population);
  AF_DIST_REQUIRE_OK(af_ctx, recovered_population);
  EXPECT_EQ(af_ctx, recovered_population.value().population.state,
            PopulationState::RevalidationRequired);
  EXPECT_EQ(af_ctx, recovered_population.value().population.candidates.size(),
            static_cast<std::size_t>(1));
  EXPECT_EQ(af_ctx, recovered_population.value().population.candidates.front(),
            survivor_candidate);
  EXPECT_FALSE(af_ctx,
               population_state_is_terminal(recovered_population.value().population.state));

  Result<CandidateDetailMessage> recovered_candidate =
      query_candidate(af_ctx, second_controller, survivor_candidate);
  AF_DIST_REQUIRE_OK(af_ctx, recovered_candidate);
  EXPECT_EQ(af_ctx, recovered_candidate.value().candidate.producer_worker, durable_worker);
  EXPECT_EQ(af_ctx, recovered_candidate.value().candidate.producer_boot, first_boot);
  EXPECT_EQ(af_ctx, recovered_candidate.value().candidate.artifact_set_digest, artifact_digest);

  Result<StatisticsDetailMessage> recovered_stats = query_statistics(af_ctx, second_controller);
  AF_DIST_REQUIRE_OK(af_ctx, recovered_stats);
  EXPECT_EQ(af_ctx, recovered_stats.value().statistics.candidate_slots_created,
            pre_stats.value().statistics.candidate_slots_created);
  EXPECT_EQ(af_ctx, recovered_stats.value().statistics.candidates_published,
            pre_stats.value().statistics.candidates_published);
  EXPECT_EQ(af_ctx, recovered_stats.value().statistics.selections_committed,
            pre_stats.value().statistics.selections_committed);

  // A recovered population refuses to select until it is explicitly revalidated.
  const Result<SelectionDecisionMessage> refused_selection =
      request_selection(af_ctx, second_controller, fixture.binding);
  EXPECT_STATUS_CODE(af_ctx, refused_selection, ErrorCode::RevalidationRequired);

  // A session carrying the old epoch reaches the coordinator and is refused.
  const WorkerBootId durable_boot = first_boot;
  ProtocolClient probe;
  connect_client(af_ctx, probe, second_coordinator, second_port.value(),
                 "the restarted coordinator");
  Result<HelloAckMessage> probe_ack = probe.hello(af_ctx, SessionRole::Worker, durable_worker,
                                                  durable_boot, "probe", "probe");
  AF_DIST_REQUIRE_OK(af_ctx, probe_ack);
  // The restored worker was Offline, not live: its record survived, its boot
  // still matches, and the incarnation must revalidate before it may work.
  EXPECT_TRUE(af_ctx, probe_ack.value().revalidation_required);

  WorkerSessionAuthority restored =
      worker_session(probe_ack.value(), durable_worker, durable_boot);
  Result<RevalidateAckMessage> revalidated =
      revalidate_worker(af_ctx, probe, restored, "restart proof");
  AF_DIST_REQUIRE_OK(af_ctx, revalidated);
  EXPECT_TRUE(af_ctx, revalidated.value().accepted);
  EXPECT_NE(af_ctx, revalidated.value().session.session_generation.value(),
            restored.session_generation.value());
  AF_DIST_REQUIRE_OK(af_ctx, worker_ready(af_ctx, probe, revalidated.value().session, "probe"));

  WorkerSessionAuthority stale_epoch = revalidated.value().session;
  stale_epoch.coordinator_epoch = first_epoch;
  const Result<ControlAckMessage> stale_epoch_ready =
      worker_ready(af_ctx, probe, stale_epoch, "probe");
  EXPECT_STATUS_CODE(af_ctx, stale_epoch_ready, ErrorCode::StaleCoordinatorEpoch);
  probe.close();

  af_ctx.phase("SHUTDOWN");
  Result<ControlAckMessage> shutdown = shutdown_coordinator(af_ctx, second_controller);
  AF_DIST_REQUIRE_OK(af_ctx, shutdown);
  EXPECT_TRUE(af_ctx, shutdown.value().ok);

  worker.close();
  int coordinator_exit = -1;
  AF_DIST_REQUIRE_OK(af_ctx, second_coordinator.wait(&coordinator_exit));
  EXPECT_EQ(af_ctx, coordinator_exit, 0);
  EXPECT_FALSE(af_ctx, process_is_alive(second_coordinator.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(worker.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(first_coordinator.pid()));
}

AF_TEST_CASE(distributed_coordinator_restart,
             restart_records_dispatched_work_as_ambiguous_rather_than_completed) {
  af_ctx.phase("SETUP");
  const std::filesystem::path worker_exe =
      require_executable(af_ctx, locate_executable("AF_WORKER_EXE", "af_worker.exe"), "af_worker");
  const std::filesystem::path coordinator_exe =
      require_executable(af_ctx, locate_executable("AF_COORDINATOR_EXE", "af_coordinator.exe"),
                         "af_coordinator");

  af_test::TempDirectory workdir("af-coordinator-ambiguous");
  RestartFixture fixture;
  fixture.state_path = workdir.child("coordinator.state");
  fixture.workspace = workdir.child("workspace");

  Result<std::uint16_t> first_port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, first_port);
  fixture.port = first_port.value();

  ChildProcess first_coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(first_coordinator, coordinator_exe, fixture.port,
                                               fixture.state_path, fixture.workspace, true, 4u));
  ProtocolClient first_controller;
  connect_client(af_ctx, first_controller, first_coordinator, fixture.port, "the coordinator");
  AF_DIST_REQUIRE_OK(af_ctx, first_controller.hello(af_ctx, SessionRole::Controller, WorkerId(),
                                                    WorkerBootId(), "distributed-tests",
                                                    "controller"));

  Result<TaskSpec> reference =
      reference_task(0x32u, placeholder_policy_id(0x32u), PolicyGeneration::first(), 4u);
  AF_DIST_REQUIRE_OK(af_ctx, reference);
  // The input payload is what keeps the attempt in flight long enough for the
  // kill to land on it deterministically.
  add_bulk_inputs(reference.value(), 24u, 128u * 1024u);
  build_population(af_ctx, first_controller, fixture, 0x32u, reference.value(), 4u, 1u, 1u);

  const WorkerId durable_worker = worker_identity(0x32u, 1u);
  ChildProcess worker;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(worker, worker_exe, fixture.port, durable_worker,
                                          fixture.workspace, "closed-form", "worker-in-flight"));

  af_ctx.phase("DISPATCH");
  AF_DIST_REQUIRE_OK(af_ctx, start_population(af_ctx, first_controller, fixture.binding));

  af_ctx.note("waiting for an attempt to exist, then killing the coordinator on top of it");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, first_coordinator, "the coordinator");
    require_running(af_ctx, worker, "the worker");
    Result<std::vector<CandidateId>> ids =
        population_candidates(af_ctx, first_controller, fixture.population);
    if (!ids.ok() || ids.value().empty()) {
      return false;
    }
    Result<CandidateDetailMessage> detail =
        query_candidate(af_ctx, first_controller, ids.value().front());
    if (!detail.ok()) {
      return false;
    }
    return detail.value().candidate.attempt.valid() &&
           !candidate_state_at_least_published(detail.value().candidate.state);
  });

  Result<std::vector<CandidateId>> ids =
      population_candidates(af_ctx, first_controller, fixture.population);
  AF_DIST_REQUIRE_OK(af_ctx, ids);
  const CandidateId slot = ids.value().front();
  Result<CandidateDetailMessage> in_flight = query_candidate(af_ctx, first_controller, slot);
  AF_DIST_REQUIRE_OK(af_ctx, in_flight);
  af_ctx.note("killing the coordinator while attempt " +
              in_flight.value().candidate.attempt.to_string() + " is in flight");

  af_ctx.phase("RESTART");
  AF_DIST_REQUIRE_OK(af_ctx, first_coordinator.kill());
  EXPECT_FALSE(af_ctx, process_is_alive(first_coordinator.pid()));
  first_controller.close();
  first_coordinator.close();
  worker.close();

  Result<std::uint16_t> second_port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, second_port);
  ChildProcess second_coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(second_coordinator, coordinator_exe,
                                               second_port.value(), fixture.state_path,
                                               fixture.workspace, false, 4u));
  ProtocolClient second_controller;
  connect_client(af_ctx, second_controller, second_coordinator, second_port.value(),
                 "the restarted coordinator");
  Result<HelloAckMessage> second_ack =
      second_controller.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(),
                              "distributed-tests", "controller");
  AF_DIST_REQUIRE_OK(af_ctx, second_ack);

  af_ctx.phase("VERIFY");
  Result<StatisticsDetailMessage> stats = query_statistics(af_ctx, second_controller);
  AF_DIST_REQUIRE_OK(af_ctx, stats);
  const std::uint64_t ambiguous = stats.value().statistics.attempts_outcome_unknown +
                                  stats.value().statistics.attempts_cancelled;
  af_ctx.note("restored attempts_outcome_unknown=" +
              std::to_string(stats.value().statistics.attempts_outcome_unknown) +
              " attempts_cancelled=" +
              std::to_string(stats.value().statistics.attempts_cancelled));
  EXPECT_TRUE(af_ctx, ambiguous >= 1u);
  EXPECT_EQ(af_ctx, stats.value().statistics.attempts_completed, static_cast<std::uint64_t>(0));

  Result<CandidateDetailMessage> recovered =
      query_candidate(af_ctx, second_controller, slot);
  AF_DIST_REQUIRE_OK(af_ctx, recovered);
  EXPECT_EQ(af_ctx, recovered.value().candidate.state, CandidateState::Registered);

  Result<PopulationDetailMessage> recovered_population =
      query_population(af_ctx, second_controller, fixture.population);
  AF_DIST_REQUIRE_OK(af_ctx, recovered_population);
  EXPECT_EQ(af_ctx, recovered_population.value().population.state,
            PopulationState::RevalidationRequired);

  af_ctx.phase("SHUTDOWN");
  Result<ControlAckMessage> shutdown = shutdown_coordinator(af_ctx, second_controller);
  AF_DIST_REQUIRE_OK(af_ctx, shutdown);
  int coordinator_exit = -1;
  AF_DIST_REQUIRE_OK(af_ctx, second_coordinator.wait(&coordinator_exit));
  EXPECT_EQ(af_ctx, coordinator_exit, 0);
  EXPECT_FALSE(af_ctx, process_is_alive(second_coordinator.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(first_coordinator.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(worker.pid()));
}

AF_TEST_CASE(distributed_coordinator_restart,
             restart_marks_a_published_unevaluated_candidate_revalidation_required) {
  af_ctx.phase("SETUP");
  const std::filesystem::path worker_exe =
      require_executable(af_ctx, locate_executable("AF_WORKER_EXE", "af_worker.exe"), "af_worker");
  const std::filesystem::path coordinator_exe =
      require_executable(af_ctx, locate_executable("AF_COORDINATOR_EXE", "af_coordinator.exe"),
                         "af_coordinator");

  af_test::TempDirectory workdir("af-coordinator-published");
  RestartFixture fixture;
  fixture.state_path = workdir.child("coordinator.state");
  fixture.workspace = workdir.child("workspace");

  Result<std::uint16_t> first_port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, first_port);
  fixture.port = first_port.value();

  ChildProcess first_coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(first_coordinator, coordinator_exe, fixture.port,
                                               fixture.state_path, fixture.workspace, true, 4u));
  ProtocolClient first_controller;
  connect_client(af_ctx, first_controller, first_coordinator, fixture.port, "the coordinator");
  AF_DIST_REQUIRE_OK(af_ctx, first_controller.hello(af_ctx, SessionRole::Controller, WorkerId(),
                                                    WorkerBootId(), "distributed-tests",
                                                    "controller"));

  // A task contract that declares no requirements at all. Nothing can complete
  // its evidence set, so the runtime leaves the published candidate exactly
  // where it is: Published, with a publication nobody has evaluated. That is a
  // deterministic way to hold the exact state the restart has to explain, with
  // no timing assumption anywhere.
  Result<TaskSpec> reference =
      reference_task(0x33u, placeholder_policy_id(0x33u), PolicyGeneration::first(), 4u);
  AF_DIST_REQUIRE_OK(af_ctx, reference);
  TaskSpec requirementless = reference.value();
  requirementless.requirements.clear();
  build_population(af_ctx, first_controller, fixture, 0x33u, requirementless, 4u, 1u, 1u);

  const WorkerId durable_worker = worker_identity(0x33u, 1u);
  ChildProcess worker;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(worker, worker_exe, fixture.port, durable_worker,
                                          fixture.workspace, "closed-form", "worker-published"));

  af_ctx.phase("DISPATCH");
  AF_DIST_REQUIRE_OK(af_ctx, start_population(af_ctx, first_controller, fixture.binding));

  af_ctx.note("waiting for a published candidate with no evaluator able to complete it");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, first_coordinator, "the coordinator");
    require_running(af_ctx, worker, "the worker");
    Result<std::vector<CandidateId>> ids =
        population_candidates(af_ctx, first_controller, fixture.population);
    if (!ids.ok() || ids.value().empty()) {
      return false;
    }
    Result<CandidateDetailMessage> detail =
        query_candidate(af_ctx, first_controller, ids.value().front());
    if (!detail.ok()) {
      return false;
    }
    return detail.value().candidate.state == CandidateState::Published;
  });

  Result<std::vector<CandidateId>> ids =
      population_candidates(af_ctx, first_controller, fixture.population);
  AF_DIST_REQUIRE_OK(af_ctx, ids);
  const CandidateId slot = ids.value().front();
  Result<CandidateDetailMessage> published = query_candidate(af_ctx, first_controller, slot);
  AF_DIST_REQUIRE_OK(af_ctx, published);
  EXPECT_EQ(af_ctx, published.value().candidate.state, CandidateState::Published);
  EXPECT_TRUE(af_ctx, published.value().candidate.artifact_set_digest.size() > 0u);
  EXPECT_TRUE(af_ctx, published.value().evaluations.empty());

  af_ctx.phase("RESTART");
  AF_DIST_REQUIRE_OK(af_ctx, first_coordinator.kill());
  EXPECT_FALSE(af_ctx, process_is_alive(first_coordinator.pid()));
  first_controller.close();
  first_coordinator.close();
  worker.close();

  Result<std::uint16_t> second_port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, second_port);
  ChildProcess second_coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(second_coordinator, coordinator_exe,
                                               second_port.value(), fixture.state_path,
                                               fixture.workspace, false, 4u));
  ProtocolClient second_controller;
  connect_client(af_ctx, second_controller, second_coordinator, second_port.value(),
                 "the restarted coordinator");
  Result<HelloAckMessage> second_ack =
      second_controller.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(),
                              "distributed-tests", "controller");
  AF_DIST_REQUIRE_OK(af_ctx, second_ack);

  af_ctx.phase("VERIFY");
  Result<CandidateDetailMessage> recovered = query_candidate(af_ctx, second_controller, slot);
  AF_DIST_REQUIRE_OK(af_ctx, recovered);
  EXPECT_EQ(af_ctx, recovered.value().candidate.state, CandidateState::RevalidationRequired);
  EXPECT_FALSE(af_ctx, recovered.value().candidate.failure_reason.empty());

  Result<PopulationDetailMessage> recovered_population =
      query_population(af_ctx, second_controller, fixture.population);
  AF_DIST_REQUIRE_OK(af_ctx, recovered_population);
  EXPECT_EQ(af_ctx, recovered_population.value().population.state,
            PopulationState::RevalidationRequired);
  EXPECT_FALSE(af_ctx, recovered_population.value().population.status_detail.empty());

  // Evidence that was never established cannot be silently inherited: the
  // recovered population must refuse to select until it is revalidated.
  const Result<SelectionDecisionMessage> refused_selection =
      request_selection(af_ctx, second_controller, fixture.binding);
  EXPECT_STATUS_CODE(af_ctx, refused_selection, ErrorCode::RevalidationRequired);

  af_ctx.phase("SHUTDOWN");
  Result<ControlAckMessage> shutdown = shutdown_coordinator(af_ctx, second_controller);
  AF_DIST_REQUIRE_OK(af_ctx, shutdown);
  int coordinator_exit = -1;
  AF_DIST_REQUIRE_OK(af_ctx, second_coordinator.wait(&coordinator_exit));
  EXPECT_EQ(af_ctx, coordinator_exit, 0);
  EXPECT_FALSE(af_ctx, process_is_alive(second_coordinator.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(first_coordinator.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(worker.pid()));
}
