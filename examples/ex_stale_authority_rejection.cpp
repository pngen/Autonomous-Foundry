// examples/ex_stale_authority_rejection.cpp
//
// Concrete stale-authority rejections.
//
// The example builds a small but real scenario: two worker incarnations produce
// two candidates in one population, the coordinator is restarted by adopting a
// durable snapshot, the task is revised, a population is advanced by an
// explicit revalidation, a retry is performed, an attempt is cancelled and a
// retention decision retires a candidate. Then it feeds the runtime eight
// authorities that must all be refused, and prints the exact error code and
// message of every refusal.
//
// Nothing in this example needs a C++ toolchain: its task contract declares a
// deterministic static rule over the candidate source as the mandatory gate, so
// the whole rejection matrix is reproducible in any environment.
//
// The table distinguishes two kinds of row:
//
//   verbatim  an authority that was genuinely issued earlier and is replayed
//             unchanged
//   adjusted  a genuinely issued authority with exactly one field set to a
//             value an older sender would still hold; the field is named
//
// Exit code 0 means every listed operation was refused, every refusal carried a
// stale-authority code, and the final state is self-consistent.

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
// The rejection table
// ---------------------------------------------------------------------------

struct RejectionRow {
  std::string subject;
  std::string operation;
  std::string construction;
  std::string code;
  std::string message;
};

struct RejectionTable {
  std::map<int, RejectionRow> rows;

  bool add(Report& report, int order, const std::string& subject, const std::string& operation,
           const std::string& construction, const af::Status& status) {
    RejectionRow row;
    row.subject = subject;
    row.operation = operation;
    row.construction = construction;
    row.code = std::string(af::error_code_name(status.code()));
    row.message = status.message();
    rows[order] = row;
    const std::string label = subject + " (" + operation + ")";
    bool ok = report.expect(!status.ok(), label + " is refused");
    ok = report.expect(af::is_stale_authority(status.code()),
                       label + " is refused with a stale-authority code") && ok;
    return ok;
  }

  void print() const {
    Report::section("stale authority rejection table");
    for (const auto& entry : rows) {
      Report::item("[" + std::to_string(entry.first) + "] " + entry.second.subject);
      Report::note("operation:   " + entry.second.operation);
      Report::note("authority:   " + entry.second.construction);
      Report::note("error code:  " + entry.second.code);
      Report::note("message:     " + entry.second.message);
    }
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
// A task contract whose mandatory gate is decidable in process.
//
// The gate is the runtime's own deterministic static rule over the candidate
// source, and the optional factor is the runtime's own reference structural
// metric. Both are real evaluators; neither spawns a process, so this example
// never depends on an installed compiler.
// ---------------------------------------------------------------------------

af::Result<af::TaskSpec> make_in_process_task(af::TaskId id, af::PolicyId policy,
                                              af::PolicyGeneration policy_generation) {
  af::TaskSpec task;
  task.id = id;
  task.generation = af::TaskGeneration::first();
  task.name = "static-gate-reference";
  task.objective =
      "Produce solution.cpp. The mandatory gate is a deterministic static rule over the "
      "candidate source, so the gate is decidable without executing anything.";

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
// Production plumbing
// ---------------------------------------------------------------------------

struct IssuedAuthority {
  af::CandidateId candidate;
  af::WorkerOperationAuthority authority;
  std::string source;
};

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

// Register a replacement incarnation of an existing worker identity: the
// durable WorkerId is kept and a fresh WorkerBootId is presented. Every
// authorization is bound to both, so every authority the previous incarnation
// held dies with the boot identity it was issued under.
af::Result<af::WorkerSessionAuthority> reopen_worker(af::FoundryCore& core,
                                                     const af::WorkerId& worker,
                                                     const std::string& label) {
  af::WorkerRegistrationRequest request;
  request.worker = worker;
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

bool publish(Report& report, af::FoundryCore& core, const af::CandidateId& candidate,
             const af::WorkerOperationAuthority& authority, const std::string& source,
             const std::string& strategy, const std::string& label) {
  af::ArtifactRef artifact;
  artifact.name = std::string(af::kReferenceTaskSourceArtifact);
  artifact.size_bytes = static_cast<std::uint64_t>(source.size());
  artifact.content_digest = af::sha256_hex(source);
  std::vector<af::ArtifactRef> artifacts;
  artifacts.push_back(artifact);
  const af::Result<af::PublicationOutcome> outcome =
      core.publish_candidate(authority, artifacts, strategy);
  if (!report.expect_status(outcome.status(), label + ": publish_candidate")) {
    return false;
  }
  return report.expect(outcome.value().candidate == candidate,
                       label + ": publication names the expected candidate");
}

std::string forbidden_token_source() {
  std::string source = af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);
  source.append("\n// The static rule is a textual scan, and this comment contains the token "
                "system( that it forbids.\n");
  return source;
}

// Run the mandatory and optional in-process evaluators of the task contract and
// record the evidence they produced.
bool evaluate(Report& report, af::FoundryCore& core, const af::TaskSpec& task,
              af::PopulationId population, af::IdAllocator& allocator,
              const std::map<af::CandidateId, std::string>& sources,
              std::map<std::string, af::EvaluatorId>& evaluator_ids) {
  const af::EvaluatorRegistry registry = af::EvaluatorRegistry::make_reference();
  for (const auto& entry : sources) {
    const af::Result<af::CandidateRecord> read = core.candidate(entry.first);
    if (!report.expect_status(read.status(), entry.first.to_string() + ": candidate readable")) {
      return false;
    }
    const af::CandidateRecord candidate = read.value();
    if (!report.expect_status(core.begin_evaluation(candidate.id),
                              candidate.id.to_string() + ": begin_evaluation")) {
      return false;
    }
    for (const af::EvaluationRequirement& requirement : task.requirements) {
      const std::shared_ptr<af::Evaluator> evaluator = registry.find(requirement.evaluator_key);
      if (evaluator == nullptr) {
        report.expect(false, "registry exposes '" + requirement.evaluator_key + "'");
        return false;
      }
      af::EvaluatorId& evaluator_id = evaluator_ids[requirement.evaluator_key];
      if (!evaluator_id.valid()) {
        const af::Result<af::EvaluatorId> minted = allocator.next<af::EvaluatorIdTag>();
        if (!report.expect_status(minted.status(), "evaluator identity minted for '" +
                                                       requirement.evaluator_key + "'")) {
          return false;
        }
        evaluator_id = minted.value();
      }

      af::EvaluationRequest request;
      request.candidate = candidate.id;
      request.candidate_generation = candidate.generation;
      request.task = task.id;
      request.task_generation = task.generation;
      request.population = population;
      request.population_generation = core.population(population).value().generation;
      request.artifacts.emplace(std::string(af::kReferenceTaskSourceArtifact), entry.second);
      for (const af::InputFile& input : task.inputs) {
        request.inputs.emplace(input.name, input.content);
      }
      request.evaluator_key = requirement.evaluator_key;
      request.requirement_class = requirement.requirement_class;

      const af::EvaluationResult result = evaluator->evaluate(request);

      af::EvaluationRecord record;
      record.candidate = candidate.id;
      record.candidate_generation = candidate.generation;
      record.task = task.id;
      record.task_generation = task.generation;
      record.population = population;
      record.population_generation = core.population(population).value().generation;
      record.evaluator = evaluator_id;
      record.evaluator_key = requirement.evaluator_key;
      record.kind = evaluator->kind();
      record.requirement_class = requirement.requirement_class;
      record.outcome = result.outcome;
      record.complete = true;
      record.has_score = result.has_score;
      record.score = result.score;
      record.decided_epoch = core.epoch();
      record.diagnostics = result.diagnostics;
      record.evidence_digest = result.evidence_digest;
      record.duration_micros = result.duration_micros;
      const std::string label = candidate.id.to_string() + ": record '" +
                                requirement.evaluator_key + "'=" +
                                std::string(af::evaluation_outcome_name(result.outcome));
      if (!report.expect_status(core.record_evaluation(record), label)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

int main() {
  Report report;
  Report::line("Autonomous Foundry reference example: stale authority rejection");
  Report::line("eight authorities that were once valid, and the exact code each one now gets");

  OperatorContext context;
  if (!build_operator(report, context)) {
    return 1;
  }
  std::unique_ptr<af::FoundryCore> core = make_core(context);
  RejectionTable table;

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
  af::TaskSpec task = core->task(task_id).value();
  Report::item("task " + task.name + " generation=" + task.generation.to_string() +
               " mandatory=" + std::to_string(task.mandatory_requirement_count()));
  for (const af::EvaluationRequirement& requirement : task.requirements) {
    Report::note(std::string(af::requirement_class_name(requirement.requirement_class)) + ": " +
                 requirement.evaluator_key);
  }

  Report::section("phase 1: build a real population");
  af::PopulationSpec p1_spec;
  p1_spec.name = "P1-stale";
  p1_spec.task = task.id;
  p1_spec.task_generation = task.generation;
  p1_spec.policy = policy.id;
  p1_spec.policy_generation = policy.generation;
  p1_spec.candidate_budget = 2;
  p1_spec.worker_budget = 2;
  p1_spec.population_index = 1;
  const af::Result<af::PopulationId> p1_created = core->create_population(p1_spec);
  if (!report.expect_status(p1_created.status(), "P1 created")) {
    return 1;
  }
  const af::PopulationId p1 = p1_created.value();
  if (!report.expect_status(
          core->start_population(p1, core->population(p1).value().generation), "P1 started")) {
    return 1;
  }

  const af::Result<af::WorkerSessionAuthority> first_session =
      open_worker(*core, context.allocator, "incarnation-a");
  if (!report.expect_status(first_session.status(), "first worker incarnation registered")) {
    return 1;
  }
  const af::Result<af::WorkerSessionAuthority> second_session =
      open_worker(*core, context.allocator, "incarnation-b");
  if (!report.expect_status(second_session.status(), "second worker incarnation registered")) {
    return 1;
  }

  std::vector<IssuedAuthority> issued;
  const std::string passing_source =
      af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);
  const std::string failing_source = forbidden_token_source();
  const std::string sources_one[] = {passing_source, failing_source};
  const af::WorkerSessionAuthority sessions_one[] = {first_session.value(), second_session.value()};
  const std::string strategies_one[] = {"reference-closed-form", "reference-closed-form-forbidden"};
  for (std::size_t index = 0; index < 2; ++index) {
    const af::Result<af::PendingDispatch> pending = core->authorize_attempt(sessions_one[index]);
    if (!report.expect_status(pending.status(), "P1 authorize_attempt")) {
      return 1;
    }
    const af::Result<af::AttemptPackage> package =
        core->confirm_dispatch(sessions_one[index], pending.value().attempt.id,
                               pending.value().attempt.generation);
    if (!report.expect_status(package.status(), "P1 confirm_dispatch")) {
      return 1;
    }
    IssuedAuthority entry;
    entry.candidate = pending.value().attempt.candidate;
    entry.authority = operation_authority(pending.value());
    entry.source = sources_one[index];
    issued.push_back(entry);
  }
  for (std::size_t index = 0; index < issued.size(); ++index) {
    if (!publish(report, *core, issued[index].candidate, issued[index].authority,
                 issued[index].source, strategies_one[index], "P1 candidate " +
                                                                  std::to_string(index + 1))) {
      return 1;
    }
  }

  std::map<af::CandidateId, std::string> sources;
  for (const IssuedAuthority& entry : issued) {
    sources[entry.candidate] = entry.source;
  }
  std::map<std::string, af::EvaluatorId> evaluator_ids;
  if (!evaluate(report, *core, task, p1, context.allocator, sources, evaluator_ids)) {
    return 1;
  }
  for (const IssuedAuthority& entry : issued) {
    const af::CandidateRecord candidate = core->candidate(entry.candidate).value();
    Report::item("P1 candidate " + entry.candidate.to_string() +
                 " state=" + std::string(af::candidate_state_name(candidate.state)));
  }

  const af::CoordinatorEpoch epoch_before = core->epoch();
  const af::FoundrySnapshot snapshot = core->snapshot();

  Report::section("phase 2: a new coordinator incarnation adopts the durable state");
  std::unique_ptr<af::FoundryCore> restarted = make_core(context);
  if (!report.expect_status(restarted->recover(snapshot), "recover from the durable snapshot")) {
    return 1;
  }
  const af::CoordinatorEpoch epoch_after = restarted->epoch();
  Report::item("coordinator epoch " + epoch_before.to_string() + " -> " +
               epoch_after.to_string());
  report.expect(epoch_after.value() == epoch_before.value() + 1,
                "the coordinator epoch advanced by exactly one on recovery");
  report.expect(restarted->population(p1).value().state == af::PopulationState::RevalidationRequired,
                "the recovered population requires explicit revalidation before further decisions");

  table.add(report, 1, "old coordinator epoch",
            "authorize_attempt with a session issued by the previous coordinator incarnation",
            "verbatim: the live session authority captured before recovery (epoch " +
                epoch_before.to_string() + ")",
            restarted->authorize_attempt(first_session.value()).status());

  // The replacement is a new incarnation of the same durable worker identity,
  // not a new worker: the WorkerId stays and the boot identity changes.
  const af::Result<af::WorkerSessionAuthority> replacement_session =
      reopen_worker(*restarted, first_session.value().worker, "incarnation-a-restarted");
  if (!report.expect_status(replacement_session.status(),
                            "a fresh incarnation re-registers the same worker identity")) {
    return 1;
  }
  report.expect(replacement_session.value().worker == first_session.value().worker &&
                    replacement_session.value().boot != first_session.value().boot,
                "the replacement incarnation shares the worker identity and differs in boot");

  af::WorkerSessionAuthority stale_boot = first_session.value();
  stale_boot.coordinator_epoch = epoch_after;
  table.add(report, 2, "old worker boot",
            "authorize_attempt with the previous incarnation's boot identity",
            "adjusted: coordinator_epoch refreshed to " + epoch_after.to_string() +
                ", boot left at the previous incarnation",
            restarted->authorize_attempt(stale_boot).status());

  Report::section("phase 3: revise the task and run a second population");
  const af::Result<af::TaskGeneration> revised =
      restarted->revise_task(task_id, restarted->task(task_id).value());
  if (!report.expect_status(revised.status(), "task revised to a new generation")) {
    return 1;
  }
  task = restarted->task(task_id).value();
  Report::item("task generation is now " + task.generation.to_string());

  af::PopulationSpec p2_spec;
  p2_spec.name = "P2-stale";
  p2_spec.task = task.id;
  p2_spec.task_generation = task.generation;
  p2_spec.policy = policy.id;
  p2_spec.policy_generation = policy.generation;
  p2_spec.candidate_budget = 4;
  p2_spec.worker_budget = 4;
  p2_spec.population_index = 1;
  const af::Result<af::PopulationId> p2_created = restarted->create_population(p2_spec);
  if (!report.expect_status(p2_created.status(), "P2 created")) {
    return 1;
  }
  const af::PopulationId p2 = p2_created.value();
  if (!report.expect_status(
          restarted->start_population(p2, restarted->population(p2).value().generation),
          "P2 started")) {
    return 1;
  }

  const af::Result<af::PendingDispatch> first_attempt =
      restarted->authorize_attempt(replacement_session.value());
  if (!report.expect_status(first_attempt.status(), "P2 first attempt authorized")) {
    return 1;
  }
  if (!report.expect_status(
          restarted->confirm_dispatch(replacement_session.value(), first_attempt.value().attempt.id,
                                      first_attempt.value().attempt.generation)
              .status(),
          "P2 first attempt dispatched")) {
    return 1;
  }
  const af::CandidateId retired_candidate = first_attempt.value().attempt.candidate;
  const af::WorkerOperationAuthority failed_authority = operation_authority(first_attempt.value());
  if (!report.expect_status(
          restarted->report_attempt_failure(failed_authority, "the worker could not produce output"),
          "P2 first attempt reported as failed")) {
    return 1;
  }
  report.expect(restarted->candidate(retired_candidate).value().state ==
                    af::CandidateState::Registered,
                "the failed attempt left the candidate slot retryable");

  table.add(report, 6, "old attempt",
            "publish_candidate with the authority of the superseded failed attempt",
            "verbatim: the operation authority of the attempt that already failed",
            restarted->publish_candidate(failed_authority,
                                         std::vector<af::ArtifactRef>{},
                                         "reference-closed-form")
                .status());

  const af::Result<af::PendingDispatch> retry = restarted->authorize_attempt(
      replacement_session.value());
  if (!report.expect_status(retry.status(), "P2 retry authorized for the same slot")) {
    return 1;
  }
  report.expect(retry.value().attempt.candidate == retired_candidate,
                "the retry reuses the candidate slot under a fresh attempt identity");
  report.expect(retry.value().attempt.retry_index == 1,
                "the retry records retry index 1 rather than reusing the first attempt");
  report.expect(retry.value().attempt.state == af::AttemptState::Authorized,
                "authorize_attempt leaves the retry Authorized: durable and reserving, but unsent");
  // acknowledge_attempt accepts a Dispatched attempt. A retry is dispatched
  // exactly like a first attempt, so the confirmation step is not optional.
  if (!report.expect_status(
          restarted->confirm_dispatch(replacement_session.value(), retry.value().attempt.id,
                                      retry.value().attempt.generation)
              .status(),
          "P2 retry dispatched")) {
    return 1;
  }
  report.expect(restarted->attempt(retry.value().attempt.id).value().state ==
                    af::AttemptState::Dispatched,
                "the dispatched retry is Dispatched and may now be acknowledged");
  if (!report.expect_status(
          restarted->acknowledge_attempt(operation_authority(retry.value())),
          "P2 retry acknowledged")) {
    return 1;
  }
  report.expect(restarted->attempt(retry.value().attempt.id).value().state ==
                    af::AttemptState::Running,
                "the acknowledged retry is Running");

  const af::Result<af::WorkerSessionAuthority> third_session =
      open_worker(*restarted, context.allocator, "incarnation-c");
  if (!report.expect_status(third_session.status(), "third worker incarnation registered")) {
    return 1;
  }
  const af::Result<af::PendingDispatch> winning_attempt =
      restarted->authorize_attempt(third_session.value());
  if (!report.expect_status(winning_attempt.status(), "P2 winning attempt authorized")) {
    return 1;
  }
  if (!report.expect_status(
          restarted->confirm_dispatch(third_session.value(), winning_attempt.value().attempt.id,
                                      winning_attempt.value().attempt.generation)
              .status(),
          "P2 winning attempt dispatched")) {
    return 1;
  }
  const af::CandidateId winner = winning_attempt.value().attempt.candidate;
  const af::WorkerOperationAuthority winner_authority =
      operation_authority(winning_attempt.value());

  if (!publish(report, *restarted, retired_candidate, operation_authority(retry.value()),
               failing_source, "reference-closed-form-forbidden", "P2 retired candidate")) {
    return 1;
  }
  if (!publish(report, *restarted, winner, winner_authority, passing_source,
               "reference-closed-form", "P2 winning candidate")) {
    return 1;
  }

  Report::section("phase 4: cancel an attempt before it produces output");
  af::PopulationSpec p3_spec;
  p3_spec.name = "P3-stale";
  p3_spec.task = task.id;
  p3_spec.task_generation = task.generation;
  p3_spec.policy = policy.id;
  p3_spec.policy_generation = policy.generation;
  p3_spec.candidate_budget = 1;
  p3_spec.worker_budget = 1;
  p3_spec.population_index = 1;
  const af::Result<af::PopulationId> p3_created = restarted->create_population(p3_spec);
  if (!report.expect_status(p3_created.status(), "P3 created")) {
    return 1;
  }
  const af::PopulationId p3 = p3_created.value();
  if (!report.expect_status(
          restarted->start_population(p3, restarted->population(p3).value().generation),
          "P3 started")) {
    return 1;
  }
  const af::Result<af::WorkerSessionAuthority> fourth_session =
      open_worker(*restarted, context.allocator, "incarnation-d");
  if (!report.expect_status(fourth_session.status(), "fourth worker incarnation registered")) {
    return 1;
  }
  const af::Result<af::PendingDispatch> cancelled_attempt =
      restarted->authorize_attempt(fourth_session.value());
  if (!report.expect_status(cancelled_attempt.status(), "P3 attempt authorized")) {
    return 1;
  }
  report.expect(cancelled_attempt.value().attempt.population == p3,
                "P3 received the assignment because no other population was accepting work");
  if (!report.expect_status(
          restarted->confirm_dispatch(fourth_session.value(), cancelled_attempt.value().attempt.id,
                                      cancelled_attempt.value().attempt.generation)
              .status(),
          "P3 attempt dispatched")) {
    return 1;
  }
  const af::WorkerOperationAuthority cancelled_authority =
      operation_authority(cancelled_attempt.value());
  if (!report.expect_status(
          restarted->cancel_attempt(cancelled_authority.attempt,
                                    "operator cancelled the assignment before it produced output"),
          "P3 attempt cancelled")) {
    return 1;
  }
  table.add(report, 7, "cancelled attempt",
            "acknowledge_attempt after the assignment was explicitly cancelled",
            "verbatim: the operation authority of the cancelled attempt",
            restarted->acknowledge_attempt(cancelled_authority));

  Report::section("phase 5: evaluate, select and retain in P2");
  std::map<af::CandidateId, std::string> sources_two;
  sources_two[retired_candidate] = failing_source;
  sources_two[winner] = passing_source;
  if (!evaluate(report, *restarted, task, p2, context.allocator, sources_two, evaluator_ids)) {
    return 1;
  }
  report.expect(restarted->candidate(retired_candidate).value().state ==
                    af::CandidateState::Evaluated,
                "a candidate that failed its mandatory gate is still Evaluated: the record exists");
  report.expect(restarted->candidate(winner).value().state == af::CandidateState::Evaluated,
                "a candidate that passed its mandatory gate is Evaluated");

  af::WorkerOperationAuthority stale_task = winner_authority;
  stale_task.task_generation = af::TaskGeneration::first();
  table.add(report, 4, "old task generation",
            "publish_candidate with an authority issued before the task was revised",
            "adjusted: task_generation set to 1 while the population is bound to generation " +
                task.generation.to_string(),
            restarted->publish_candidate(stale_task, std::vector<af::ArtifactRef>{},
                                         "reference-closed-form")
                .status());

  af::WorkerOperationAuthority stale_candidate = winner_authority;
  stale_candidate.candidate_generation = af::CandidateGeneration::from_value(
      winner_authority.candidate_generation.value() + 1);
  table.add(report, 5, "old candidate generation",
            "publish_candidate with an authority naming a candidate generation it does not hold",
            "adjusted: candidate_generation set to " +
                stale_candidate.candidate_generation.to_string() + " while the candidate is at " +
                winner_authority.candidate_generation.to_string(),
            restarted->publish_candidate(stale_candidate, std::vector<af::ArtifactRef>{},
                                         "reference-closed-form")
                .status());

  const af::Result<af::SelectionDecision> prepared = restarted->prepare_selection(p2);
  if (!report.expect_status(prepared.status(), "P2 prepare_selection")) {
    return 1;
  }
  const af::Result<af::SelectionDecision> committed =
      restarted->commit_selection(prepared.value());
  if (!report.expect_status(committed.status(), "P2 commit_selection")) {
    return 1;
  }
  report.expect(committed.value().selected == winner,
                "the candidate that passed its mandatory gate won selection");
  const af::Result<af::RetentionDecision> prepared_retention =
      restarted->prepare_retention(p2);
  if (!report.expect_status(prepared_retention.status(), "P2 prepare_retention")) {
    return 1;
  }
  const af::Result<af::RetentionDecision> retained =
      restarted->commit_retention(prepared_retention.value());
  if (!report.expect_status(retained.status(), "P2 commit_retention")) {
    return 1;
  }
  report.expect(restarted->candidate(retired_candidate).value().state ==
                    af::CandidateState::Retired,
                "the candidate that failed its gate was retired by the retention decision");

  table.add(report, 8, "retired candidate",
            "publish_candidate with the authority that originally published the retired candidate",
            "verbatim: the operation authority of the publication that created it",
            restarted->publish_candidate(operation_authority(retry.value()),
                                         std::vector<af::ArtifactRef>{},
                                         "reference-closed-form-forbidden")
                .status());

  Report::section("phase 6: revalidate the population");
  const af::PopulationGeneration generation_before =
      restarted->population(p2).value().generation;
  if (!report.expect_status(
          restarted->request_population_revalidation(p2, "operator requested revalidation"),
          "P2 revalidation requested")) {
    return 1;
  }
  const af::PopulationGeneration generation_after_request =
      restarted->population(p2).value().generation;
  if (!report.expect_status(restarted->revalidate_population(p2, generation_after_request,
                                                             "revalidation completed"),
                            "P2 revalidated")) {
    return 1;
  }
  const af::PopulationGeneration generation_after =
      restarted->population(p2).value().generation;
  Report::item("P2 generation " + generation_before.to_string() + " -> " +
               generation_after_request.to_string() + " -> " + generation_after.to_string());

  af::WorkerOperationAuthority stale_population = winner_authority;
  stale_population.population_generation = generation_before;
  table.add(report, 3, "old population generation",
            "publish_candidate with an authority issued before the population was revalidated",
            "adjusted: population_generation set to " + generation_before.to_string() +
                " while the population is at " + generation_after.to_string(),
            restarted->publish_candidate(stale_population, std::vector<af::ArtifactRef>{},
                                         "reference-closed-form")
                .status());

  table.print();

  Report::section("summary");
  Report::item("rows=" + std::to_string(table.rows.size()) +
               " refused=" + std::to_string(table.rows.size()));
  const af::FoundryStatistics statistics = restarted->statistics();
  Report::item("stale_authority_rejections=" +
               std::to_string(statistics.stale_authority_rejections) +
               " late_rejections=" + std::to_string(statistics.late_rejections) +
               " duplicate_rejections=" + std::to_string(statistics.duplicate_rejections));
  report.expect(table.rows.size() == 8, "all eight stale-authority rows were produced");

  Report::section("self-consistency audit");
  const std::vector<std::string> violations = restarted->audit();
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
