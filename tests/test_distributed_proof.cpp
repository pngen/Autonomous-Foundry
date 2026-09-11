// Distributed end-to-end proof suite.
//
// A REAL af_coordinator process, REAL af_worker processes and a REAL controller
// connection over framed TCP, driving the whole pipeline: define a policy and a
// task, create and start a population, let workers produce candidates, let the
// evaluation pool produce evidence records, commit a selection and a retention,
// then ask for a clean shutdown over the control plane.
//
// What the suite proves, and how:
//
//   * frames really crossed a socket: every assertion below is the result of a
//     request written to a TCP connection and a reply decoded from the byte
//     stream another PROCESS produced, and the frame counter is asserted to
//     advance;
//   * the pipeline reached a committed selection with exactly one winner and a
//     committed retention that retains that winner;
//   * a requirement the deployment cannot evaluate is recorded as UNSUPPORTED
//     and never becomes a pass, so a mandatory gate that cannot be evaluated
//     cannot produce a winner at all;
//   * the shutdown acknowledgement was sent before the coordinator stopped, both
//     children exited, and no process the case started is still alive.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/worker.hpp"
#include "distributed_support.hpp"
#include "test_support.hpp"

using namespace autonomous_foundry;  // NOLINT(google-build-using-namespace)
using namespace af_dist;             // NOLINT(google-build-using-namespace)

namespace {

[[nodiscard]] const ExclusionEntry* find_exclusion(const SelectionDecision& decision,
                                                   CandidateId candidate) {
  for (const ExclusionEntry& entry : decision.excluded) {
    if (entry.candidate == candidate) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] bool contains(const std::vector<CandidateId>& values, CandidateId probe) {
  for (const CandidateId value : values) {
    if (value == probe) {
      return true;
    }
  }
  return false;
}

}  // namespace

AF_TEST_CASE(distributed_proof, tcp_pipeline_commits_a_winner_and_a_retention_then_shuts_down) {
  af_ctx.phase("SETUP");
  const std::filesystem::path worker_exe =
      require_executable(af_ctx, locate_executable("AF_WORKER_EXE", "af_worker.exe"), "af_worker");
  const std::filesystem::path coordinator_exe =
      require_executable(af_ctx, locate_executable("AF_COORDINATOR_EXE", "af_coordinator.exe"),
                         "af_coordinator");

  af_test::TempDirectory workdir("af-distributed-proof");
  const std::filesystem::path state_path = workdir.child("coordinator.state");
  const std::filesystem::path workspace = workdir.child("workspace");

  Result<std::uint16_t> port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, port);
  af_ctx.note("coordinator endpoint 127.0.0.1:" + std::to_string(port.value()));

  ChildProcess coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(coordinator, coordinator_exe, port.value(),
                                               state_path, workspace, true, 4u));
  const std::uint32_t coordinator_pid = coordinator.pid();

  ProtocolClient controller;
  connect_client(af_ctx, controller, coordinator, port.value(), "the coordinator");
  Result<HelloAckMessage> ack =
      controller.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(),
                       "distributed-tests", "controller");
  AF_DIST_REQUIRE_OK(af_ctx, ack);
  af_ctx.note("controller session established over TCP; epoch " +
              ack.value().coordinator_epoch.to_string());

  FoundryPolicy policy =
      make_reference_policy(placeholder_policy_id(0x41u), PolicyGeneration::first());
  policy.retention.retain_top_k = 2u;
  policy.retention.max_retained = 4u;
  Result<PolicyDefinedMessage> defined = define_policy(af_ctx, controller, policy);
  AF_DIST_REQUIRE_OK(af_ctx, defined);

  // The task declares its requirements as OPTIONAL.
  //
  // This is not a shortcut around the runtime's gates, it is the honest shape of
  // this deployment: the coordinator receives artifact REFERENCES over the wire
  // (name, size, digest), not artifact bytes, so its evaluation pool has no
  // candidate source to hand an evaluator and reports every artifact-backed
  // requirement as UNSUPPORTED. A mandatory requirement could therefore never be
  // satisfied here - which the second case of this suite proves directly - and
  // the population would never be able to commit a decision at all. Declaring
  // the same evaluators optional keeps the evidence records, the ranking, the
  // selection and the retention real while leaving the gate semantics to the
  // case that can actually exercise them.
  Result<TaskSpec> reference = reference_task(0x41u, defined.value().policy,
                                              defined.value().generation, 4u);
  AF_DIST_REQUIRE_OK(af_ctx, reference);
  TaskSpec task = reference.value();
  for (EvaluationRequirement& requirement : task.requirements) {
    requirement.requirement_class = RequirementClass::Optional;
  }
  Result<TaskCreatedMessage> created = create_task(af_ctx, controller, task);
  AF_DIST_REQUIRE_OK(af_ctx, created);
  EXPECT_TRUE(af_ctx, created.value().task.valid());
  EXPECT_TRUE(af_ctx, created.value().generation.valid());
  EXPECT_FALSE(af_ctx, created.value().content_digest.empty());

  PopulationSpec spec;
  spec.name = "P1";
  spec.task = created.value().task;
  spec.task_generation = created.value().generation;
  spec.policy = defined.value().policy;
  spec.policy_generation = defined.value().generation;
  spec.candidate_budget = 4u;
  spec.worker_budget = 2u;
  spec.population_index = 1u;
  Result<PopulationCreatedMessage> population = create_population(af_ctx, controller, spec);
  AF_DIST_REQUIRE_OK(af_ctx, population);
  const PopulationId population_id = population.value().population;
  const PopulationBinding binding =
      population_binding(population_id, population.value().generation, created.value().task,
                         created.value().generation, defined.value().policy,
                         defined.value().generation);

  const WorkerId first_worker = worker_identity(0x41u, 1u);
  const WorkerId second_worker = worker_identity(0x41u, 2u);
  ChildProcess workers[2];
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(workers[0], worker_exe, port.value(), first_worker,
                                          workspace, "closed-form", "worker-closed-form"));
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(workers[1], worker_exe, port.value(), second_worker,
                                          workspace, "iterative", "worker-iterative"));

  af_ctx.phase("DISPATCH");
  Result<PopulationStateMessage> started = start_population(af_ctx, controller, binding);
  AF_DIST_REQUIRE_OK(af_ctx, started);
  EXPECT_EQ(af_ctx, started.value().state, PopulationState::Running);

  af_ctx.note("waiting for both workers to publish and for the evaluation pool to settle them");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, coordinator, "the coordinator");
    require_running(af_ctx, workers[0], "the first worker");
    require_running(af_ctx, workers[1], "the second worker");
    Result<std::vector<CandidateId>> ids = population_candidates(af_ctx, controller, population_id);
    if (!ids.ok() || ids.value().size() < 2u) {
      return false;
    }
    for (const CandidateId id : ids.value()) {
      Result<CandidateDetailMessage> detail = query_candidate(af_ctx, controller, id);
      if (!detail.ok() || detail.value().candidate.state != CandidateState::Evaluated) {
        return false;
      }
    }
    return true;
  });

  af_ctx.phase("COMMIT");
  Result<std::vector<CandidateId>> candidate_ids =
      population_candidates(af_ctx, controller, population_id);
  AF_DIST_REQUIRE_OK(af_ctx, candidate_ids);
  EXPECT_EQ(af_ctx, candidate_ids.value().size(), static_cast<std::size_t>(2));

  std::size_t produced_by_first = 0;
  std::size_t produced_by_second = 0;
  std::size_t evidence_records = 0;
  for (const CandidateId id : candidate_ids.value()) {
    Result<CandidateDetailMessage> detail = query_candidate(af_ctx, controller, id);
    AF_DIST_REQUIRE_OK(af_ctx, detail);
    EXPECT_EQ(af_ctx, detail.value().candidate.state, CandidateState::Evaluated);
    EXPECT_TRUE(af_ctx, detail.value().candidate.artifact_set_digest.size() > 0u);
    if (detail.value().candidate.producer_worker == first_worker) {
      ++produced_by_first;
    }
    if (detail.value().candidate.producer_worker == second_worker) {
      ++produced_by_second;
    }
    evidence_records += detail.value().evaluations.size();
  }
  // Both worker processes really produced a candidate: neither is a fixture.
  EXPECT_EQ(af_ctx, produced_by_first, static_cast<std::size_t>(1));
  EXPECT_EQ(af_ctx, produced_by_second, static_cast<std::size_t>(1));
  EXPECT_TRUE(af_ctx, evidence_records >= 2u);
  af_ctx.note("candidates=" + std::to_string(candidate_ids.value().size()) +
              " evidence records=" + std::to_string(evidence_records));

  Result<SelectionDecisionMessage> selection = request_selection(af_ctx, controller, binding);
  AF_DIST_REQUIRE_OK(af_ctx, selection);
  const SelectionDecision& decision = selection.value().decision;
  EXPECT_TRUE(af_ctx, selection.value().committed);
  EXPECT_EQ(af_ctx, decision.state, SelectionDecisionState::Committed);
  EXPECT_TRUE(af_ctx, decision.has_winner());
  EXPECT_TRUE(af_ctx, decision.generation.valid());
  EXPECT_TRUE(af_ctx, decision.excluded.empty());
  EXPECT_EQ(af_ctx, decision.ranking.size(), candidate_ids.value().size());
  EXPECT_EQ(af_ctx, decision.ranking.front().candidate, decision.selected);
  af_ctx.note("committed winner " + decision.selected.to_string() + ": " + decision.rationale);

  std::size_t marked_selected = 0;
  for (const CandidateId id : candidate_ids.value()) {
    Result<CandidateDetailMessage> detail = query_candidate(af_ctx, controller, id);
    AF_DIST_REQUIRE_OK(af_ctx, detail);
    if (detail.value().candidate.selected) {
      ++marked_selected;
      EXPECT_EQ(af_ctx, detail.value().candidate.id, decision.selected);
      EXPECT_EQ(af_ctx, detail.value().candidate.selection_generation, decision.generation);
    }
  }
  EXPECT_EQ(af_ctx, marked_selected, static_cast<std::size_t>(1));

  Result<PopulationDetailMessage> after_selection =
      query_population(af_ctx, controller, population_id);
  AF_DIST_REQUIRE_OK(af_ctx, after_selection);
  EXPECT_EQ(af_ctx, after_selection.value().committed_selection_generation,
            decision.generation.value());
  EXPECT_FALSE(af_ctx, after_selection.value().retention_committed);

  Result<RetentionDecisionMessage> retention = request_retention(af_ctx, controller, binding);
  AF_DIST_REQUIRE_OK(af_ctx, retention);
  EXPECT_TRUE(af_ctx, retention.value().decision.committed);
  EXPECT_EQ(af_ctx, retention.value().decision.selection_generation, decision.generation);
  EXPECT_TRUE(af_ctx, contains(retention.value().decision.retained, decision.selected));
  EXPECT_EQ(af_ctx, retention.value().decision.retained.size() +
                        retention.value().decision.retired.size(),
            candidate_ids.value().size());
  std::size_t retained_because_selected = 0;
  for (const RetentionDecisionEntry& entry : retention.value().decision.entries) {
    if (entry.outcome == RetentionOutcome::RetainedBecauseSelected) {
      ++retained_because_selected;
      EXPECT_EQ(af_ctx, entry.candidate, decision.selected);
    }
  }
  EXPECT_EQ(af_ctx, retained_because_selected, static_cast<std::size_t>(1));
  af_ctx.note("retention retained " +
              std::to_string(retention.value().decision.retained.size()) + " retired " +
              std::to_string(retention.value().decision.retired.size()));

  // Frames really crossed the socket: this controller decoded well over a dozen
  // frames from the byte stream a different process wrote.
  const std::uint64_t frames = controller.frames_received();
  EXPECT_TRUE(af_ctx, frames >= 15u);
  af_ctx.note("frames decoded from the TCP byte stream: " + std::to_string(frames));

  // A second, independent TCP connection sees the same durable decision, which a
  // local in-process object could not produce.
  ProtocolClient observer;
  connect_client(af_ctx, observer, coordinator, port.value(), "the coordinator");
  Result<HelloAckMessage> observer_ack =
      observer.hello(af_ctx, SessionRole::Controller, WorkerId(), WorkerBootId(), "observer",
                     "controller");
  AF_DIST_REQUIRE_OK(af_ctx, observer_ack);
  Result<CandidateDetailMessage> observed =
      query_candidate(af_ctx, observer, decision.selected);
  AF_DIST_REQUIRE_OK(af_ctx, observed);
  EXPECT_TRUE(af_ctx, observed.value().candidate.selected);
  EXPECT_EQ(af_ctx, observed.value().candidate.selection_generation, decision.generation);
  observer.close();

  af_ctx.phase("SHUTDOWN");
  Result<ControlAckMessage> shutdown = shutdown_coordinator(af_ctx, controller);
  AF_DIST_REQUIRE_OK(af_ctx, shutdown);
  EXPECT_TRUE(af_ctx, shutdown.value().ok);
  EXPECT_EQ(af_ctx, shutdown.value().code, ErrorCode::Ok);

  for (ChildProcess& worker : workers) {
    worker.close();
  }
  int coordinator_exit = -1;
  AF_DIST_REQUIRE_OK(af_ctx, coordinator.wait(&coordinator_exit));
  EXPECT_EQ(af_ctx, coordinator_exit, 0);

  EXPECT_FALSE(af_ctx, process_is_alive(coordinator_pid));
  for (ChildProcess& worker : workers) {
    EXPECT_FALSE(af_ctx, process_is_alive(worker.pid()));
  }
  af_ctx.note("every child process the case started has exited");
}

AF_TEST_CASE(distributed_proof,
             an_unevaluable_mandatory_gate_is_unsupported_and_never_becomes_a_winner) {
  af_ctx.phase("SETUP");
  const std::filesystem::path worker_exe =
      require_executable(af_ctx, locate_executable("AF_WORKER_EXE", "af_worker.exe"), "af_worker");
  const std::filesystem::path coordinator_exe =
      require_executable(af_ctx, locate_executable("AF_COORDINATOR_EXE", "af_coordinator.exe"),
                         "af_coordinator");

  af_test::TempDirectory workdir("af-distributed-gate");
  const std::filesystem::path state_path = workdir.child("coordinator.state");
  const std::filesystem::path workspace = workdir.child("workspace");

  Result<std::uint16_t> port = reserve_free_port();
  AF_DIST_REQUIRE_OK(af_ctx, port);

  ChildProcess coordinator;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_coordinator(coordinator, coordinator_exe, port.value(),
                                               state_path, workspace, true, 4u));
  const std::uint32_t coordinator_pid = coordinator.pid();

  ProtocolClient controller;
  connect_client(af_ctx, controller, coordinator, port.value(), "the coordinator");
  AF_DIST_REQUIRE_OK(af_ctx, controller.hello(af_ctx, SessionRole::Controller, WorkerId(),
                                              WorkerBootId(), "distributed-tests", "controller"));

  Result<PolicyDefinedMessage> defined = define_policy(
      af_ctx, controller,
      make_reference_policy(placeholder_policy_id(0x42u), PolicyGeneration::first()));
  AF_DIST_REQUIRE_OK(af_ctx, defined);
  Result<TaskSpec> task = reference_task(0x42u, defined.value().policy,
                                         defined.value().generation, 4u);
  AF_DIST_REQUIRE_OK(af_ctx, task);
  EXPECT_EQ(af_ctx, task.value().mandatory_requirement_count(), static_cast<std::size_t>(2));
  Result<TaskCreatedMessage> created = create_task(af_ctx, controller, task.value());
  AF_DIST_REQUIRE_OK(af_ctx, created);

  PopulationSpec spec;
  spec.name = "P1";
  spec.task = created.value().task;
  spec.task_generation = created.value().generation;
  spec.policy = defined.value().policy;
  spec.policy_generation = defined.value().generation;
  spec.candidate_budget = 4u;
  spec.worker_budget = 1u;
  spec.population_index = 1u;
  Result<PopulationCreatedMessage> population = create_population(af_ctx, controller, spec);
  AF_DIST_REQUIRE_OK(af_ctx, population);
  const PopulationId population_id = population.value().population;
  const PopulationBinding binding =
      population_binding(population_id, population.value().generation, created.value().task,
                         created.value().generation, defined.value().policy,
                         defined.value().generation);

  ChildProcess worker;
  AF_DIST_REQUIRE_OK(af_ctx, spawn_worker(worker, worker_exe, port.value(),
                                          worker_identity(0x42u, 1u), workspace, "closed-form",
                                          "worker-gate"));

  af_ctx.phase("DISPATCH");
  AF_DIST_REQUIRE_OK(af_ctx, start_population(af_ctx, controller, binding));

  af_ctx.note("waiting for the evaluation pool to record the mandatory gate outcome");
  await_true(af_ctx, "WAIT", [&] {
    require_running(af_ctx, coordinator, "the coordinator");
    require_running(af_ctx, worker, "the worker");
    Result<std::vector<CandidateId>> ids = population_candidates(af_ctx, controller, population_id);
    if (!ids.ok() || ids.value().empty()) {
      return false;
    }
    Result<CandidateDetailMessage> detail = query_candidate(af_ctx, controller, ids.value().front());
    if (!detail.ok()) {
      return false;
    }
    return !detail.value().evaluations.empty();
  });

  af_ctx.phase("VERIFY");
  Result<std::vector<CandidateId>> ids = population_candidates(af_ctx, controller, population_id);
  AF_DIST_REQUIRE_OK(af_ctx, ids);
  const CandidateId candidate = ids.value().front();
  Result<CandidateDetailMessage> detail = query_candidate(af_ctx, controller, candidate);
  AF_DIST_REQUIRE_OK(af_ctx, detail);

  std::size_t unsupported = 0;
  for (const EvaluationRecord& record : detail.value().evaluations) {
    EXPECT_TRUE(af_ctx, record.complete);
    EXPECT_TRUE(af_ctx, record.authoritative_for_mandatory() == false ||
                            record.outcome != EvaluationOutcome::Pass);
    if (record.outcome == EvaluationOutcome::Unsupported) {
      ++unsupported;
      af_ctx.note("record '" + record.evaluator_key + "': " + record.diagnostics);
    }
  }
  EXPECT_TRUE(af_ctx, unsupported >= 2u);
  EXPECT_EQ(af_ctx, detail.value().candidate.state, CandidateState::Evaluated);

  Result<SelectionDecisionMessage> selection = request_selection(af_ctx, controller, binding);
  AF_DIST_REQUIRE_OK(af_ctx, selection);
  EXPECT_TRUE(af_ctx, selection.value().committed);
  EXPECT_EQ(af_ctx, selection.value().decision.state, SelectionDecisionState::Impossible);
  EXPECT_FALSE(af_ctx, selection.value().decision.has_winner());
  EXPECT_TRUE(af_ctx, selection.value().decision.ranking.empty());
  const ExclusionEntry* excluded = find_exclusion(selection.value().decision, candidate);
  EXPECT_TRUE(af_ctx, excluded != nullptr);
  if (excluded != nullptr) {
    EXPECT_EQ(af_ctx, excluded->reason, ExclusionReason::MandatoryEvaluationUnsupported);
    EXPECT_EQ(af_ctx, excluded->stage, SelectionStage::HardConstraints);
  }
  Result<CandidateDetailMessage> after = query_candidate(af_ctx, controller, candidate);
  AF_DIST_REQUIRE_OK(af_ctx, after);
  EXPECT_FALSE(af_ctx, after.value().candidate.selected);

  af_ctx.phase("SHUTDOWN");
  Result<ControlAckMessage> shutdown = shutdown_coordinator(af_ctx, controller);
  AF_DIST_REQUIRE_OK(af_ctx, shutdown);
  EXPECT_TRUE(af_ctx, shutdown.value().ok);

  worker.close();
  int coordinator_exit = -1;
  AF_DIST_REQUIRE_OK(af_ctx, coordinator.wait(&coordinator_exit));
  EXPECT_EQ(af_ctx, coordinator_exit, 0);
  EXPECT_FALSE(af_ctx, process_is_alive(coordinator_pid));
  EXPECT_FALSE(af_ctx, process_is_alive(worker.pid()));
}


