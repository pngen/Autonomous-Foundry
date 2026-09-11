// Distributed worker-death suite.
//
// These cases start a REAL coordinator process and REAL af_worker processes
// that connect over real TCP and are assigned real attempts, then kill a worker
// process while it holds an in-flight attempt.
//
// What the suite proves, and how:
//
//   * the coordinator observes the disconnect through the closed socket, not
//     through a heartbeat and not through a timer;
//   * the attempt the dead worker held becomes ambiguous (OutcomeUnknown) and
//     the candidate slot it was producing becomes retryable;
//   * another worker incarnation is dispatched the retry of that same slot, so
//     the disconnection costs an attempt, not a candidate;
//   * a restarted worker process for the SAME durable WorkerId registers with a
//     NEW WorkerBootId, and the old boot's authority is refused with
//     StaleWorkerBoot / UnknownWorkerIncarnation rather than silently applied.
//
// Every wait loops on a real request/response against the coordinator. There is
// no deadline, no attempt cap and no watchdog.

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

/// Everything a worker-death case needs that is built before the first worker
/// connects: a live coordinator process, a controller session, a policy, a task
/// and one population whose worker budget admits a single attempt at a time.
struct DeathHarness {
  std::filesystem::path workspace;
  ChildProcess coordinator;
  ProtocolClient controller;
  PopulationId population;
  PopulationBinding binding;
  TaskId task;
  TaskGeneration task_generation;
};

}  // namespace

AF_TEST_CASE(distributed_worker_death,
             killed_worker_leaves_an_ambiguous_attempt_and_a_survivor_retries_the_slot) {
  af_ctx.phase("SETUP");
  const std::filesystem::path worker_exe =
      require_executable(af_ctx, locate_executable("AF_WORKER_EXE", "af_worker.exe"), "af_worker");
  const std::filesystem::path coordinator_exe =
      require_executable(af_ctx, locate_executable("AF_COORDINATOR_EXE", "af_coordinator.exe"),
                         "af_coordinator");

  af_test::TempDirectory workdir("af-worker-death");
  const std::filesystem::path state_path = workdir.child("coordinator.state");
  const std::filesystem::path workspace = workdir.child("workspace");

  Result<std::uint16_t> port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, port);
  af_ctx.note("coordinator endpoint 127.0.0.1:" + std::to_string(port.value()));

  DeathHarness harness;
  harness.workspace = workspace;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(harness.coordinator, coordinator_exe, port.value(),
                                               state_path, workspace, true, 4u));

  connect_client(af_ctx, harness.controller, harness.coordinator, port.value(), "the coordinator");
  Result<HelloAckMessage> controller_ack =
      harness.controller.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(),
                               "distributed-tests", "controller");
  AF_DIST_REQUIRE_OK(af_ctx, controller_ack);
  EXPECT_TRUE(af_ctx, controller_ack.value().coordinator_epoch.valid());

  const FoundryPolicy policy =
      make_reference_policy(placeholder_policy_id(0x21u), PolicyGeneration::first());
  Result<PolicyDefinedMessage> defined = define_policy(af_ctx, harness.controller, policy);
  AF_DIST_REQUIRE_OK(af_ctx, defined);

  Result<TaskSpec> task =
      reference_task(0x21u, defined.value().policy, defined.value().generation, 8u);
  AF_DIST_REQUIRE_OK(af_ctx, task);
  // Long enough that the attempt is observably in flight over the wire.
  add_bulk_inputs(task.value(), 24u, 128u * 1024u);
  Result<TaskCreatedMessage> created = create_task(af_ctx, harness.controller, task.value());
  AF_DIST_REQUIRE_OK(af_ctx, created);
  harness.task = created.value().task;
  harness.task_generation = created.value().generation;

  PopulationSpec spec;
  spec.name = "P1";
  spec.task = harness.task;
  spec.task_generation = harness.task_generation;
  spec.policy = defined.value().policy;
  spec.policy_generation = defined.value().generation;
  spec.candidate_budget = 8u;
  // One attempt at a time, so exactly one worker can hold an attempt and the
  // other worker is provably available to take the retry.
  spec.worker_budget = 1u;
  spec.population_index = 1u;
  Result<PopulationCreatedMessage> population =
      create_population(af_ctx, harness.controller, spec);
  AF_DIST_REQUIRE_OK(af_ctx, population);
  harness.population = population.value().population;
  harness.binding = population_binding(harness.population, population.value().generation,
                                       harness.task, harness.task_generation,
                                       defined.value().policy, defined.value().generation);

  const WorkerId doomed_worker = worker_identity(0x21u, 1u);
  const WorkerId survivor_worker = worker_identity(0x21u, 2u);

  ChildProcess doomed;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(doomed, worker_exe, port.value(), doomed_worker,
                                          workspace, "closed-form", "worker-doomed"));

  af_ctx.phase("DISPATCH");
  Result<PopulationStateMessage> started =
      start_population(af_ctx, harness.controller, harness.binding);
  AF_DIST_REQUIRE_OK(af_ctx, started);
  EXPECT_EQ(af_ctx, started.value().state, PopulationState::Running);

  // Wait until the coordinator has authorized an attempt for the only connected
  // worker. The candidate slot is created inside authorize_attempt, so the slot
  // appearing is the observable proof that an attempt now exists.
  af_ctx.note("waiting for the first attempt to be authorized");
  std::string observed_shape;
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, harness.coordinator, "the coordinator");
    require_running(af_ctx, doomed, "the doomed worker");
    const PopulationRecord observed =
        population_of(af_ctx, harness.controller, harness.population);
    const FoundryStatistics stats = statistics_of(af_ctx, harness.controller);
    const std::string shape = std::string(population_state_name(observed.state)) + ":gen" +
                              std::to_string(observed.generation.value()) + ":cand" +
                              std::to_string(observed.candidates.size()) + ":auth" +
                              std::to_string(stats.attempts_authorized);
    if (shape != observed_shape) {
      observed_shape = shape;
      af_ctx.note("population " + shape);
    }
    return observed.candidates.size() == 1u;
  });

  Result<std::vector<CandidateId>> initial_ids =
      population_candidates(af_ctx, harness.controller, harness.population);
  AF_DIST_REQUIRE_OK(af_ctx, initial_ids);
  const CandidateId slot = initial_ids.value().front();

  Result<CandidateDetailMessage> in_flight =
      query_candidate(af_ctx, harness.controller, slot);
  AF_DIST_REQUIRE_OK(af_ctx, in_flight);
  const AttemptId first_attempt = in_flight.value().candidate.attempt;
  EXPECT_TRUE(af_ctx, first_attempt.valid());
  af_ctx.note("attempt in flight: " + first_attempt.to_string() + " on " +
              doomed_worker.to_string() + " pid " + std::to_string(doomed.pid()));

  // The survivor is started before the kill so that it is connected over real
  // TCP and Ready when the retry becomes dispatchable.
  ChildProcess survivor;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(survivor, worker_exe, port.value(), survivor_worker,
                                          workspace, "iterative", "worker-survivor"));

  Result<StatisticsDetailMessage> before_stats =
      query_statistics(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, before_stats);
  const std::uint64_t unknown_before = before_stats.value().statistics.attempts_outcome_unknown;

  af_ctx.phase("DISPATCH");
  af_ctx.note("killing the worker that holds attempt " + first_attempt.to_string());
  AF_DIST_REQUIRE_OK(af_ctx, doomed.kill());
  EXPECT_FALSE(af_ctx, process_is_alive(doomed.pid()));

  // The coordinator observes the disconnect through the closed socket: the
  // attempt the dead incarnation held is recorded as OutcomeUnknown and the slot
  // it was producing returns to a retryable state.
  af_ctx.note("waiting for the coordinator to observe the disconnect");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, harness.coordinator, "the coordinator");
    Result<StatisticsDetailMessage> stats = query_statistics(af_ctx, harness.controller);
    if (!stats.ok() || stats.value().statistics.attempts_outcome_unknown < unknown_before + 1u) {
      return false;
    }
    Result<CandidateDetailMessage> detail = query_candidate(af_ctx, harness.controller, slot);
    return detail.ok() && detail.value().candidate.state == CandidateState::Registered;
  });

  af_ctx.phase("VERIFY");
  Result<StatisticsDetailMessage> after_kill = query_statistics(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, after_kill);
  EXPECT_TRUE(af_ctx,
              after_kill.value().statistics.attempts_outcome_unknown >= unknown_before + 1u);

  Result<CandidateDetailMessage> ambiguous = query_candidate(af_ctx, harness.controller, slot);
  AF_DIST_REQUIRE_OK(af_ctx, ambiguous);
  // The slot is retryable again and no work of the dead incarnation is still
  // presented as in flight. The survivor was deliberately started before the
  // kill so that it is Ready the moment the slot comes back, which means the
  // retry may be authorized between the wait above and this query: Registered,
  // and Registered-then-redispatched, are the same fact. What this must never
  // accept is the slot still sitting on the attempt whose worker is gone.
  af_ctx.note("slot after the kill: state " +
              std::string(candidate_state_name(ambiguous.value().candidate.state)) + " attempt " +
              ambiguous.value().candidate.attempt.to_string());
  EXPECT_TRUE(af_ctx, ambiguous.value().candidate.state == CandidateState::Registered ||
                          ambiguous.value().candidate.attempt != first_attempt);

  af_ctx.note("waiting for the surviving worker to be dispatched the retry");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, harness.coordinator, "the coordinator");
    require_running(af_ctx, survivor, "the surviving worker");
    Result<CandidateDetailMessage> detail = query_candidate(af_ctx, harness.controller, slot);
    if (!detail.ok()) {
      return false;
    }
    return detail.value().candidate.attempt != first_attempt &&
           candidate_state_at_least_published(detail.value().candidate.state);
  });

  af_ctx.phase("VERIFY");
  Result<CandidateDetailMessage> retried = query_candidate(af_ctx, harness.controller, slot);
  AF_DIST_REQUIRE_OK(af_ctx, retried);
  EXPECT_EQ(af_ctx, retried.value().candidate.id, slot);
  EXPECT_NE(af_ctx, retried.value().candidate.attempt, first_attempt);
  EXPECT_EQ(af_ctx, retried.value().candidate.producer_worker, survivor_worker);
  EXPECT_TRUE(af_ctx, candidate_state_at_least_published(retried.value().candidate.state));

  // The retry reused the slot rather than manufacturing a second candidate, and
  // the dead incarnation's attempt is recorded as terminal and ambiguous.
  Result<std::vector<CandidateId>> final_ids =
      population_candidates(af_ctx, harness.controller, harness.population);
  AF_DIST_REQUIRE_OK(af_ctx, final_ids);
  EXPECT_EQ(af_ctx, final_ids.value().size(), static_cast<std::size_t>(1));
  Result<StatisticsDetailMessage> final_stats = query_statistics(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, final_stats);
  EXPECT_TRUE(af_ctx, final_stats.value().statistics.attempts_authorized >= 2u);
  EXPECT_EQ(af_ctx, final_stats.value().statistics.attempts_outcome_unknown,
            unknown_before + 1u);
  af_ctx.note("attempts_authorized=" +
              std::to_string(final_stats.value().statistics.attempts_authorized) +
              " attempts_outcome_unknown=" +
              std::to_string(final_stats.value().statistics.attempts_outcome_unknown));

  af_ctx.phase("SHUTDOWN");
  Result<ControlAckMessage> shutdown = shutdown_coordinator(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, shutdown);
  EXPECT_TRUE(af_ctx, shutdown.value().ok);

  survivor.close();
  int coordinator_exit = -1;
  AF_DIST_REQUIRE_OK(af_ctx, harness.coordinator.wait(&coordinator_exit));
  EXPECT_EQ(af_ctx, coordinator_exit, 0);
  EXPECT_FALSE(af_ctx, process_is_alive(harness.coordinator.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(survivor.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(doomed.pid()));
  doomed.close();
}

AF_TEST_CASE(distributed_worker_death,
             restarted_worker_registers_a_new_boot_and_the_old_boot_is_refused) {
  af_ctx.phase("SETUP");
  const std::filesystem::path worker_exe =
      require_executable(af_ctx, locate_executable("AF_WORKER_EXE", "af_worker.exe"), "af_worker");
  const std::filesystem::path coordinator_exe =
      require_executable(af_ctx, locate_executable("AF_COORDINATOR_EXE", "af_coordinator.exe"),
                         "af_coordinator");

  af_test::TempDirectory workdir("af-worker-boot");
  const std::filesystem::path state_path = workdir.child("coordinator.state");
  const std::filesystem::path workspace = workdir.child("workspace");

  Result<std::uint16_t> port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, port);

  DeathHarness harness;
  harness.workspace = workspace;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(harness.coordinator, coordinator_exe, port.value(),
                                               state_path, workspace, true, 4u));
  connect_client(af_ctx, harness.controller, harness.coordinator, port.value(), "the coordinator");
  Result<HelloAckMessage> controller_ack =
      harness.controller.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(),
                               "distributed-tests", "controller");
  AF_DIST_REQUIRE_OK(af_ctx, controller_ack);

  const FoundryPolicy policy =
      make_reference_policy(placeholder_policy_id(0x22u), PolicyGeneration::first());
  Result<PolicyDefinedMessage> defined = define_policy(af_ctx, harness.controller, policy);
  AF_DIST_REQUIRE_OK(af_ctx, defined);
  Result<TaskSpec> task =
      reference_task(0x22u, defined.value().policy, defined.value().generation, 8u);
  AF_DIST_REQUIRE_OK(af_ctx, task);
  add_bulk_inputs(task.value(), 24u, 128u * 1024u);
  Result<TaskCreatedMessage> created = create_task(af_ctx, harness.controller, task.value());
  AF_DIST_REQUIRE_OK(af_ctx, created);
  harness.task = created.value().task;
  harness.task_generation = created.value().generation;

  // Two populations. The first lets the worker publish once, which is what
  // records the boot identity of its first incarnation in durable state. The
  // second stays Running so a killed attempt there is retryable by the
  // incarnation that replaces it.
  PopulationSpec first_spec;
  first_spec.name = "P1";
  first_spec.task = harness.task;
  first_spec.task_generation = harness.task_generation;
  first_spec.policy = defined.value().policy;
  first_spec.policy_generation = defined.value().generation;
  first_spec.candidate_budget = 4u;
  first_spec.worker_budget = 1u;
  first_spec.population_index = 1u;
  Result<PopulationCreatedMessage> first_population =
      create_population(af_ctx, harness.controller, first_spec);
  AF_DIST_REQUIRE_OK(af_ctx, first_population);

  PopulationSpec second_spec = first_spec;
  second_spec.name = "P2";
  Result<PopulationCreatedMessage> second_population =
      create_population(af_ctx, harness.controller, second_spec);
  AF_DIST_REQUIRE_OK(af_ctx, second_population);
  const PopulationId second_population_id = second_population.value().population;
  const PopulationBinding first_binding =
      population_binding(first_population.value().population, first_population.value().generation,
                         harness.task, harness.task_generation, defined.value().policy,
                         defined.value().generation);
  const PopulationBinding second_binding = population_binding(
      second_population_id, second_population.value().generation, harness.task,
      harness.task_generation, defined.value().policy, defined.value().generation);

  const WorkerId durable_worker = worker_identity(0x22u, 1u);
  ChildProcess worker;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(worker, worker_exe, port.value(), durable_worker,
                                          workspace, "closed-form", "worker-incarnation-one"));

  af_ctx.phase("DISPATCH");
  AF_DIST_REQUIRE_OK(af_ctx, start_population(af_ctx, harness.controller, first_binding));

  af_ctx.note("waiting for the first incarnation to publish, which records its boot identity");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, harness.coordinator, "the coordinator");
    require_running(af_ctx, worker, "the first worker incarnation");
    Result<std::vector<CandidateId>> ids = population_candidates(
        af_ctx, harness.controller, first_population.value().population);
    if (!ids.ok() || ids.value().empty()) {
      return false;
    }
    Result<CandidateDetailMessage> detail =
        query_candidate(af_ctx, harness.controller, ids.value().front());
    if (!detail.ok()) {
      return false;
    }
    return candidate_state_at_least_published(detail.value().candidate.state);
  });

  Result<std::vector<CandidateId>> first_ids = population_candidates(
      af_ctx, harness.controller, first_population.value().population);
  AF_DIST_REQUIRE_OK(af_ctx, first_ids);
  Result<CandidateDetailMessage> published =
      query_candidate(af_ctx, harness.controller, first_ids.value().front());
  AF_DIST_REQUIRE_OK(af_ctx, published);
  const WorkerBootId first_boot = published.value().candidate.producer_boot;
  EXPECT_EQ(af_ctx, published.value().candidate.producer_worker, durable_worker);
  EXPECT_TRUE(af_ctx, first_boot.valid());
  af_ctx.note("first incarnation produced under boot " + first_boot.to_string());

  af_ctx.phase("DISPATCH");
  AF_DIST_REQUIRE_OK(af_ctx, start_population(af_ctx, harness.controller, second_binding));

  af_ctx.note("waiting for the second population to hold an in-flight attempt");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, harness.coordinator, "the coordinator");
    require_running(af_ctx, worker, "the first worker incarnation");
    Result<std::vector<CandidateId>> ids =
        population_candidates(af_ctx, harness.controller, second_population_id);
    if (!ids.ok() || ids.value().empty()) {
      return false;
    }
    Result<CandidateDetailMessage> detail =
        query_candidate(af_ctx, harness.controller, ids.value().front());
    if (!detail.ok()) {
      return false;
    }
    return detail.value().candidate.attempt.valid() &&
           !candidate_state_at_least_published(detail.value().candidate.state);
  });

  Result<std::vector<CandidateId>> second_ids =
      population_candidates(af_ctx, harness.controller, second_population_id);
  AF_DIST_REQUIRE_OK(af_ctx, second_ids);
  const CandidateId second_slot = second_ids.value().front();
  Result<CandidateDetailMessage> second_in_flight =
      query_candidate(af_ctx, harness.controller, second_slot);
  AF_DIST_REQUIRE_OK(af_ctx, second_in_flight);
  const AttemptId dead_attempt = second_in_flight.value().candidate.attempt;
  const PopulationGeneration second_generation =
      second_in_flight.value().candidate.population_generation;
  const TaskId second_task = second_in_flight.value().candidate.task;
  const TaskGeneration second_task_generation =
      second_in_flight.value().candidate.task_generation;
  const CandidateGeneration second_candidate_generation =
      second_in_flight.value().candidate.generation;

  Result<StatisticsDetailMessage> before_kill_stats =
      query_statistics(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, before_kill_stats);
  const std::uint64_t unknown_before =
      before_kill_stats.value().statistics.attempts_outcome_unknown;

  af_ctx.note("killing the first incarnation while it holds " + dead_attempt.to_string());
  AF_DIST_REQUIRE_OK(af_ctx, worker.kill());
  EXPECT_FALSE(af_ctx, process_is_alive(worker.pid()));

  af_ctx.note("waiting for the attempt to be recorded as ambiguous");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, harness.coordinator, "the coordinator");
    Result<StatisticsDetailMessage> stats = query_statistics(af_ctx, harness.controller);
    if (!stats.ok()) {
      return false;
    }
    return stats.value().statistics.attempts_outcome_unknown >= unknown_before + 1u;
  });

  af_ctx.phase("RESTART");
  // The same durable WorkerId, a brand new process, and therefore a brand new
  // WorkerBootId. The runtime mints the boot identity, so the case does not
  // choose it: it reads it back out of durable state after the retry publishes.
  ChildProcess restarted;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(restarted, worker_exe, port.value(), durable_worker,
                                          workspace, "closed-form", "worker-incarnation-two"));

  af_ctx.note("waiting for the replacement incarnation to publish the retry");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, harness.coordinator, "the coordinator");
    require_running(af_ctx, restarted, "the replacement worker incarnation");
    Result<CandidateDetailMessage> detail =
        query_candidate(af_ctx, harness.controller, second_slot);
    if (!detail.ok()) {
      return false;
    }
    return detail.value().candidate.attempt != dead_attempt &&
           candidate_state_at_least_published(detail.value().candidate.state);
  });

  af_ctx.phase("VERIFY");
  Result<CandidateDetailMessage> republished =
      query_candidate(af_ctx, harness.controller, second_slot);
  AF_DIST_REQUIRE_OK(af_ctx, republished);
  const WorkerBootId second_boot = republished.value().candidate.producer_boot;
  EXPECT_EQ(af_ctx, republished.value().candidate.producer_worker, durable_worker);
  EXPECT_TRUE(af_ctx, second_boot.valid());
  // The same durable worker identity, two different boot incarnations.
  EXPECT_NE(af_ctx, second_boot, first_boot);
  af_ctx.note("replacement incarnation produced under boot " + second_boot.to_string());

  // A raw worker socket, registered under a third boot identity, presents the
  // dead incarnation's boot coordinates. The coordinator must refuse them.
  ProtocolClient probe;
  connect_client(af_ctx, probe, harness.coordinator, port.value(), "the coordinator");
  const WorkerBootId probe_boot = boot_identity(0x22u, 77u);
  Result<HelloAckMessage> probe_ack = probe.hello(af_ctx, SessionRole::Worker, durable_worker,
                                                  probe_boot, "probe", "probe");
  AF_DIST_REQUIRE_OK(af_ctx, probe_ack);
  const WorkerSessionAuthority probe_session =
      worker_session(probe_ack.value(), durable_worker, probe_boot);

  Result<ControlAckMessage> probe_ready =
      worker_ready(af_ctx, probe, probe_session, "probe");
  AF_DIST_REQUIRE_OK(af_ctx, probe_ready);
  EXPECT_TRUE(af_ctx, probe_ready.value().ok);

  Result<StatisticsDetailMessage> before_probes = query_statistics(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, before_probes);

  WorkerSessionAuthority stale_boot_session = probe_session;
  stale_boot_session.boot = first_boot;
  const Result<ControlAckMessage> stale_boot_ready =
      worker_ready(af_ctx, probe, stale_boot_session, "probe");
  EXPECT_STATUS_CODE(af_ctx, stale_boot_ready, ErrorCode::StaleWorkerBoot);

  WorkerSessionAuthority unknown_worker_session = probe_session;
  unknown_worker_session.worker = worker_identity(0x22u, 991u);
  const Result<ControlAckMessage> unknown_worker_ready =
      worker_ready(af_ctx, probe, unknown_worker_session, "probe");
  EXPECT_STATUS_CODE(af_ctx, unknown_worker_ready, ErrorCode::UnknownWorkerIncarnation);

  // Stale operation authority: the dead boot's session presented with the exact
  // attempt coordinates it used to hold. The refusal must be an authority
  // refusal, and it must not mutate any durable evidence.
  WorkerOperationAuthority stale_authority;
  stale_authority.session = stale_boot_session;
  stale_authority.population = second_population_id;
  stale_authority.population_generation = second_generation;
  stale_authority.task = second_task;
  stale_authority.task_generation = second_task_generation;
  stale_authority.candidate = second_slot;
  stale_authority.candidate_generation = second_candidate_generation;
  stale_authority.attempt = dead_attempt;
  stale_authority.attempt_generation = AttemptGeneration::first();
  const Result<ControlAckMessage> stale_attempt_ack =
      acknowledge_attempt(af_ctx, probe, stale_authority);
  EXPECT_STATUS_CODE(af_ctx, stale_attempt_ack, ErrorCode::StaleWorkerBoot);

  Result<StatisticsDetailMessage> after_probes = query_statistics(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, after_probes);
  EXPECT_EQ(af_ctx, after_probes.value().statistics.attempts_completed,
            before_probes.value().statistics.attempts_completed);
  EXPECT_EQ(af_ctx, after_probes.value().statistics.candidates_published,
            before_probes.value().statistics.candidates_published);
  EXPECT_EQ(af_ctx, after_probes.value().statistics.evaluations_recorded,
            before_probes.value().statistics.evaluations_recorded);
  EXPECT_TRUE(af_ctx, after_probes.value().statistics.stale_authority_rejections >
                          before_probes.value().statistics.stale_authority_rejections);

  probe.close();

  af_ctx.phase("SHUTDOWN");
  Result<ControlAckMessage> shutdown = shutdown_coordinator(af_ctx, harness.controller);
  AF_DIST_REQUIRE_OK(af_ctx, shutdown);
  EXPECT_TRUE(af_ctx, shutdown.value().ok);

  restarted.close();
  int coordinator_exit = -1;
  AF_DIST_REQUIRE_OK(af_ctx, harness.coordinator.wait(&coordinator_exit));
  EXPECT_EQ(af_ctx, coordinator_exit, 0);
  EXPECT_FALSE(af_ctx, process_is_alive(harness.coordinator.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(restarted.pid()));
  EXPECT_FALSE(af_ctx, process_is_alive(worker.pid()));
  worker.close();
}
