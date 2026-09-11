// examples/ex_cancellation.cpp
//
// Cancellation at four different points of one attempt.
//
// The same assignment is created four times and cancelled at a different stage
// each time:
//
//   1. after authorize_attempt, before anything was sent to a worker
//   2. after confirm_dispatch, after the assignment frame was durably recorded
//   3. after acknowledge_attempt, while the worker was running
//   4. during evaluation, after the candidate had already published its output
//
// For every stage the example prints the attempt state and the candidate state
// before and after the cancellation, and then proves the cancellation is final:
// a cancelled attempt may never be acknowledged, may never publish, and a
// candidate whose production was cancelled may never accept evidence or enter
// ranking.
//
// Cancellation is deliberately different from a worker death. A cancelled
// attempt is a decision; a lost worker is an unknown. This example covers the
// decision.
//
// The mandatory gate of this task contract is a deterministic static rule over
// the candidate source, so the example never depends on an installed compiler.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/evaluator.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/worker.hpp"

namespace af = autonomous_foundry;

namespace {

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

struct Report {
  int failures{0};

  static void line(const std::string& text) { std::printf("%s\n", text.c_str()); }
  static void section(const std::string& title) { std::printf("\n== %s ==\n", title.c_str()); }
  static void note(const std::string& text) { std::printf("   %s\n", text.c_str()); }
  static void item(const std::string& text) { std::printf("   - %s\n", text.c_str()); }

  bool expect(bool condition, const std::string& what) {
    if (condition) {
      std::printf("   OK   %s\n", what.c_str());
      return true;
    }
    ++failures;
    std::printf("   FAIL %s\n", what.c_str());
    return false;
  }

  bool expect_status(const af::Status& status, const std::string& what) {
    if (status.ok()) {
      std::printf("   OK   %s\n", what.c_str());
      return true;
    }
    ++failures;
    std::printf("   FAIL %s: code=%s message=%s\n", what.c_str(),
                std::string(af::error_code_name(status.code())).c_str(), status.message().c_str());
    return false;
  }
};

// ---------------------------------------------------------------------------
// Operator identities and the foundry under test
// ---------------------------------------------------------------------------

struct OperatorContext {
  af::FoundryId foundry;
  af::FoundryRunId run;
  std::uint32_t salt{0};
  af::IdAllocator allocator;
};

bool build_operator(Report& report, OperatorContext& context) {
  context.salt = af::make_id_salt();
  context.allocator = af::IdAllocator(context.salt);
  const af::Result<af::FoundryId> foundry = context.allocator.next<af::FoundryIdTag>();
  if (!report.expect_status(foundry.status(), "operator identity minted")) {
    return false;
  }
  context.foundry = foundry.value();
  context.run = af::make_foundry_run_id();
  return true;
}

std::unique_ptr<af::FoundryCore> make_core(const OperatorContext& context) {
  af::FoundryConfig config;
  config.foundry = context.foundry;
  config.run = context.run;
  config.epoch = af::CoordinatorEpoch::from_value(1);
  config.id_salt = context.salt;
  return std::make_unique<af::FoundryCore>(config);
}

// ---------------------------------------------------------------------------
// Task contract with an in-process mandatory gate
// ---------------------------------------------------------------------------

af::Result<af::TaskSpec> make_in_process_task(af::TaskId id, af::PolicyId policy,
                                              af::PolicyGeneration policy_generation) {
  af::TaskSpec task;
  task.id = id;
  task.generation = af::TaskGeneration::first();
  task.name = "cancellation-reference";
  task.objective =
      "Produce solution.cpp. The mandatory gate is a deterministic static rule over the "
      "candidate source.";

  af::InputFile header;
  header.name = std::string(af::kReferenceTaskHeaderArtifact);
  header.content = af::reference_api_header_source();
  header.content_digest = af::sha256_hex(header.content);
  task.inputs.push_back(std::move(header));

  task.required_outputs.push_back(std::string(af::kReferenceTaskSourceArtifact));

  af::EvaluationRequirement gate;
  gate.requirement_class = af::RequirementClass::Mandatory;
  gate.evaluator_key = std::string(af::kEvaluatorSourcePolicy);
  gate.description = "deterministic static rule over the candidate source";
  gate.ranking_weight = 1.0;
  task.requirements.push_back(std::move(gate));

  af::EvaluationRequirement metric;
  metric.requirement_class = af::RequirementClass::Optional;
  metric.evaluator_key = std::string(af::kEvaluatorParsimony);
  metric.description = "reference structural metric: identifier-like token count";
  metric.ranking_weight = 1.0;
  task.requirements.push_back(std::move(metric));

  task.hard_constraints.push_back("the candidate source must satisfy the static rule");
  task.closure_criteria.push_back("every declared requirement has a complete record");

  task.policy = policy;
  task.policy_generation = policy_generation;

  std::string digest;
  AF_TRY_ASSIGN(digest, af::compute_task_digest(task));
  task.content_digest = std::move(digest);
  AF_TRY(af::validate_task(task));
  return task;
}

// ---------------------------------------------------------------------------
// Plumbing
// ---------------------------------------------------------------------------

af::WorkerOperationAuthority operation_authority(const af::PendingDispatch& dispatch) {
  af::WorkerOperationAuthority authority;
  authority.session = dispatch.session;
  authority.population = dispatch.attempt.population;
  authority.population_generation = dispatch.attempt.population_generation;
  authority.task = dispatch.attempt.task;
  authority.task_generation = dispatch.attempt.task_generation;
  authority.candidate = dispatch.attempt.candidate;
  authority.candidate_generation = dispatch.attempt.candidate_generation;
  authority.attempt = dispatch.attempt.id;
  authority.attempt_generation = dispatch.attempt.generation;
  authority.assignment = dispatch.attempt.assignment;
  return authority;
}

af::Result<af::WorkerSessionAuthority> open_worker(af::FoundryCore& core,
                                                   af::IdAllocator& allocator,
                                                   const std::string& label) {
  const af::Result<af::WorkerId> worker = allocator.next<af::WorkerIdTag>();
  if (!worker.ok()) {
    return worker.status();
  }
  af::WorkerRegistrationRequest request;
  request.worker = worker.value();
  request.boot = af::make_worker_boot_id();
  request.label = label;
  request.capability = "reference-static";
  request.process_id = 0;
  const af::Result<af::WorkerSessionAuthority> session = core.register_worker(request);
  if (!session.ok()) {
    return session.status();
  }
  const af::Status ready = core.worker_ready(session.value(), "reference-static");
  if (!ready.ok()) {
    return ready;
  }
  return session.value();
}

/// How far the assignment is advanced before it is cancelled.
enum class StopPoint {
  Authorized = 1,
  Dispatched = 2,
  Acknowledged = 3,
  Evaluating = 4,
};

const char* stop_point_name(StopPoint point) {
  switch (point) {
    case StopPoint::Authorized:
      return "after authorize_attempt, before anything was sent";
    case StopPoint::Dispatched:
      return "after confirm_dispatch, the assignment frame was recorded as sent";
    case StopPoint::Acknowledged:
      return "after acknowledge_attempt, the worker reported it was running";
    case StopPoint::Evaluating:
      return "during evaluation, after the candidate had published its output";
  }
  return "unknown";
}

af::AttemptState stop_point_attempt_state(StopPoint point) {
  switch (point) {
    case StopPoint::Authorized:
      return af::AttemptState::Authorized;
    case StopPoint::Dispatched:
      return af::AttemptState::Dispatched;
    case StopPoint::Acknowledged:
      return af::AttemptState::Running;
    case StopPoint::Evaluating:
      return af::AttemptState::Evaluating;
  }
  return af::AttemptState::Created;
}

struct CancellationCase {
  std::string label;
  StopPoint stop_point{StopPoint::Authorized};
  af::PopulationId population;
  af::CandidateId candidate;
  af::WorkerOperationAuthority authority;
  af::AttemptState attempt_before{af::AttemptState::Created};
  af::AttemptState attempt_after{af::AttemptState::Created};
  af::CandidateState candidate_before{af::CandidateState::Registered};
  af::CandidateState candidate_after{af::CandidateState::Registered};
};

af::Result<CancellationCase> stage_case(af::FoundryCore& core, af::IdAllocator& allocator,
                                        const af::TaskSpec& task,
                                        const af::FoundryPolicy& policy, const std::string& label,
                                        StopPoint point, const std::string& source) {
  const std::string prefix = label + ": ";

  af::PopulationSpec population_spec;
  population_spec.name = label;
  population_spec.task = task.id;
  population_spec.task_generation = task.generation;
  population_spec.policy = policy.id;
  population_spec.policy_generation = policy.generation;
  population_spec.candidate_budget = 1;
  population_spec.worker_budget = 1;
  population_spec.population_index = 1;
  const af::Result<af::PopulationId> created = core.create_population(population_spec);
  if (!created.ok()) {
    return created.status();
  }
  const af::PopulationId population = created.value();
  const af::Status started =
      core.start_population(population, core.population(population).value().generation);
  if (!started.ok()) {
    return started;
  }

  const af::Result<af::WorkerSessionAuthority> session = open_worker(core, allocator, label);
  if (!session.ok()) {
    return session.status();
  }

  const af::Result<af::PendingDispatch> pending = core.authorize_attempt(session.value());
  if (!pending.ok()) {
    return pending.status();
  }
  CancellationCase staged;
  staged.label = label;
  staged.stop_point = point;
  staged.population = population;
  staged.candidate = pending.value().attempt.candidate;
  staged.authority = operation_authority(pending.value());
  if (staged.authority.population != population) {
    return af::Status(af::ErrorCode::Internal, prefix + "the assignment went to another population");
  }

  if (point != StopPoint::Authorized) {
    const af::Status dispatched =
        core.confirm_dispatch(session.value(), pending.value().attempt.id,
                              pending.value().attempt.generation)
            .status();
    if (!dispatched.ok()) {
      return dispatched;
    }
  }
  if (point == StopPoint::Acknowledged || point == StopPoint::Evaluating) {
    const af::Status acknowledged = core.acknowledge_attempt(staged.authority);
    if (!acknowledged.ok()) {
      return acknowledged;
    }
  }
  if (point == StopPoint::Evaluating) {
    af::ArtifactRef artifact;
    artifact.name = std::string(af::kReferenceTaskSourceArtifact);
    artifact.size_bytes = static_cast<std::uint64_t>(source.size());
    artifact.content_digest = af::sha256_hex(source);
    std::vector<af::ArtifactRef> artifacts;
    artifacts.push_back(artifact);
    const af::Status published =
        core.publish_candidate(staged.authority, artifacts, "reference-closed-form").status();
    if (!published.ok()) {
      return published;
    }
    const af::Status evaluating = core.begin_evaluation(staged.candidate);
    if (!evaluating.ok()) {
      return evaluating;
    }
  }

  staged.attempt_before = core.attempt(staged.authority.attempt).value().state;
  staged.candidate_before = core.candidate(staged.candidate).value().state;
  const af::Status cancelled = core.cancel_attempt(
      staged.authority.attempt, "operator cancelled at " + std::string(stop_point_name(point)));
  if (!cancelled.ok()) {
    return cancelled;
  }
  staged.attempt_after = core.attempt(staged.authority.attempt).value().state;
  staged.candidate_after = core.candidate(staged.candidate).value().state;
  return staged;
}

}  // namespace

int main() {
  Report report;
  Report::line("Autonomous Foundry reference example: cancellation");
  Report::line("one assignment, four different moments, and the state each cancellation leaves");

  OperatorContext context;
  if (!build_operator(report, context)) {
    return 1;
  }
  std::unique_ptr<af::FoundryCore> core = make_core(context);

  Report::section("policy and task contract");
  af::PolicyId policy_id;
  {
    const af::Result<af::PolicyId> minted = context.allocator.next<af::PolicyIdTag>();
    if (!report.expect_status(minted.status(), "policy identity minted")) {
      return 1;
    }
    policy_id = minted.value();
  }
  const af::Result<af::PolicyId> defined =
      core->define_policy(af::make_reference_policy(policy_id, af::PolicyGeneration::first()));
  if (!report.expect_status(defined.status(), "reference policy defined")) {
    return 1;
  }
  const af::FoundryPolicy policy = core->policy(defined.value()).value();

  af::TaskId task_id;
  {
    const af::Result<af::TaskId> minted = context.allocator.next<af::TaskIdTag>();
    if (!report.expect_status(minted.status(), "task identity minted")) {
      return 1;
    }
    const af::Result<af::TaskSpec> spec =
        make_in_process_task(minted.value(), policy.id, policy.generation);
    if (!report.expect_status(spec.status(), "in-process task contract built")) {
      return 1;
    }
    const af::Result<af::TaskId> defined_task = core->define_task(spec.value());
    if (!report.expect_status(defined_task.status(), "in-process task defined")) {
      return 1;
    }
    task_id = defined_task.value();
  }
  const af::TaskSpec task = core->task(task_id).value();

  const std::string source = af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);

  Report::section("four cancellation points");
  const StopPoint points[] = {StopPoint::Authorized, StopPoint::Dispatched, StopPoint::Acknowledged,
                              StopPoint::Evaluating};
  const std::string labels[] = {"case-1-authorized", "case-2-dispatched", "case-3-acknowledged",
                                "case-4-evaluating"};
  std::vector<CancellationCase> cases;
  for (std::size_t index = 0; index < 4; ++index) {
    const af::Result<CancellationCase> staged =
        stage_case(*core, context.allocator, task, policy, labels[index], points[index], source);
    if (!report.expect_status(staged.status(), labels[index] + ": staging")) {
      return 1;
    }
    cases.push_back(staged.value());
  }

  for (const CancellationCase& entry : cases) {
    Report::item(entry.label + "  " + stop_point_name(entry.stop_point));
    Report::note("attempt before=" + std::string(af::attempt_state_name(entry.attempt_before)) +
                 "  attempt after=" + std::string(af::attempt_state_name(entry.attempt_after)));
    Report::note("candidate before=" +
                 std::string(af::candidate_state_name(entry.candidate_before)) +
                 "  candidate after=" +
                 std::string(af::candidate_state_name(entry.candidate_after)));
    report.expect(entry.attempt_after == af::AttemptState::Cancelled,
                  entry.label + ": the attempt is Cancelled");
    report.expect(entry.attempt_before == stop_point_attempt_state(entry.stop_point),
                  entry.label + ": the cancellation happened at the intended attempt state");
  }

  Report::section("a cancelled attempt can never be acknowledged or publish again");
  for (const CancellationCase& entry : cases) {
    const af::Status acknowledged = core->acknowledge_attempt(entry.authority);
    report.expect(!acknowledged.ok() && af::is_stale_authority(acknowledged.code()),
                  entry.label + ": acknowledge_attempt is refused after cancellation");
    Report::note(entry.label + " acknowledge code=" +
                 std::string(af::error_code_name(acknowledged.code())) + " message=" +
                 acknowledged.message());

    af::ArtifactRef artifact;
    artifact.name = std::string(af::kReferenceTaskSourceArtifact);
    artifact.size_bytes = static_cast<std::uint64_t>(source.size());
    artifact.content_digest = af::sha256_hex(source);
    std::vector<af::ArtifactRef> artifacts;
    artifacts.push_back(artifact);
    const af::Result<af::PublicationOutcome> published =
        core->publish_candidate(entry.authority, artifacts, "reference-closed-form");
    report.expect(!published.ok() && af::is_stale_authority(published.status().code()),
                  entry.label + ": publish_candidate is refused after cancellation");
    Report::note(entry.label + " publish code=" +
                 std::string(af::error_code_name(published.status().code())) + " message=" +
                 published.status().message());

    const af::Status recancelled =
        core->cancel_attempt(entry.authority.attempt, "cancelled a second time");
    report.expect(recancelled.ok(),
                  entry.label + ": cancelling an already cancelled attempt is an idempotent no-op");
  }

  Report::section("cancelled production is terminal for the candidate");
  std::map<std::string, af::EvaluatorId> evaluator_ids;
  const af::Result<af::EvaluatorId> evaluator_id =
      context.allocator.next<af::EvaluatorIdTag>();
  if (!report.expect_status(evaluator_id.status(), "evaluator identity minted")) {
    return 1;
  }
  evaluator_ids["source-policy"] = evaluator_id.value();

  for (const CancellationCase& entry : cases) {
    if (entry.stop_point == StopPoint::Evaluating) {
      Report::note(entry.label +
                   ": the candidate had already published, so its output and evidence remain "
                   "history; the cancellation is recorded against the attempt");
      continue;
    }
    report.expect(entry.candidate_after == af::CandidateState::ProductionCancelled,
                  entry.label + ": the candidate is ProductionCancelled, a terminal state");

    af::EvaluationRecord record;
    record.candidate = entry.candidate;
    record.candidate_generation = af::CandidateGeneration::first();
    record.task = task.id;
    record.task_generation = task.generation;
    record.population = entry.population;
    record.population_generation = core->population(entry.population).value().generation;
    record.evaluator = evaluator_ids["source-policy"];
    record.evaluator_key = std::string(af::kEvaluatorSourcePolicy);
    record.kind = af::EvaluatorKind::SourcePolicy;
    record.requirement_class = af::RequirementClass::Mandatory;
    record.outcome = af::EvaluationOutcome::Pass;
    record.complete = true;
    record.decided_epoch = core->epoch();
    record.diagnostics = "a late claim of success filed after the cancellation";
    const af::Status recorded = core->record_evaluation(record);
    report.expect(!recorded.ok() && af::is_stale_authority(recorded.code()),
                  entry.label + ": a late PASS record is refused for a cancelled candidate");
    Report::note(entry.label + " evidence code=" +
                 std::string(af::error_code_name(recorded.code())) + " message=" +
                 recorded.message());

    const af::Result<af::SelectionDecision> selection =
        core->prepare_selection(entry.population);
    if (!report.expect_status(selection.status(), entry.label + ": prepare_selection")) {
      return 1;
    }
    report.expect(selection.value().state == af::SelectionDecisionState::Impossible,
                  entry.label + ": selection is Impossible, ranking was not entered");
    report.expect(selection.value().excluded.size() == 1,
                  entry.label + ": the cancelled candidate is reported as excluded");
    if (selection.value().excluded.size() == 1) {
      const af::ExclusionEntry& excluded = selection.value().excluded.front();
      report.expect(excluded.candidate == entry.candidate,
                    entry.label + ": the exclusion names the cancelled candidate");
      Report::note(entry.label + " exclusion stage=" +
                   std::string(af::selection_stage_name(excluded.stage)) + " reason=" +
                   std::string(af::exclusion_reason_name(excluded.reason)) + " detail=" +
                   excluded.detail);
    }
  }

  Report::section("state of the case cancelled during evaluation");
  for (const CancellationCase& entry : cases) {
    if (entry.stop_point != StopPoint::Evaluating) {
      continue;
    }
    const af::AttemptRecord attempt = core->attempt(entry.authority.attempt).value();
    const af::CandidateRecord candidate = core->candidate(entry.candidate).value();
    Report::item("attempt " + attempt.id.to_string() +
                 " is " + std::string(af::attempt_state_name(attempt.state)) + ": " +
                 attempt.failure_detail);
    Report::item("candidate " + candidate.id.to_string() +
                 " is " + std::string(af::candidate_state_name(candidate.state)) +
                 " with " + std::to_string(candidate.artifacts.size()) + " published artifact(s)");
    report.expect(attempt.state == af::AttemptState::Cancelled,
                  "the attempt stays Cancelled after a cancellation during evaluation");
    report.expect(candidate.state != af::CandidateState::Evaluated,
                  "the cancelled attempt did not settle its candidate to Evaluated");
    report.expect(candidate.artifact_set_digest == af::artifact_set_digest(candidate.artifacts),
                  "the published artifact set digest is still consistent");
  }

  Report::section("summary");
  const af::FoundryStatistics statistics = core->statistics();
  Report::item("attempts_authorized=" + std::to_string(statistics.attempts_authorized) +
               " attempts_cancelled=" + std::to_string(statistics.attempts_cancelled) +
               " attempts_completed=" + std::to_string(statistics.attempts_completed) +
               " late_rejections=" + std::to_string(statistics.late_rejections));
  report.expect(statistics.attempts_cancelled == 4, "exactly four attempts were cancelled");
  report.expect(statistics.attempts_completed == 0,
                "no cancelled attempt was ever counted as completed");
  report.expect(statistics.candidates_published == 1,
                "only the case that published before cancellation has published output");

  Report::section("self-consistency audit");
  const std::vector<std::string> violations = core->audit();
  report.expect(violations.empty(), "audit reports no violations");
  for (const std::string& violation : violations) {
    Report::note(violation);
  }

  if (report.failures == 0) {
    Report::line("\nRESULT: PASS");
    return 0;
  }
  Report::line("\nRESULT: FAIL failures=" + std::to_string(report.failures));
  return 1;
}
