// examples/ex_basic_population.cpp
//
// Basic population.
//
// This example drives the reference task through one complete in-process
// population:
//
//   1. define the reference policy and the reference task
//   2. create and start a population
//   3. register three worker incarnations and mark them ready
//   4. for every worker slot: authorize_attempt -> confirm_dispatch ->
//      acknowledge_attempt -> publish_candidate with a generated reference
//      solution
//   5. evaluate every published candidate through the EvaluatorRegistry and
//      record the evidence durably
//   6. print the resulting candidate states
//
// Steps 4 and 5 are genuinely separate: dispatch records that an assignment was
// handed to a worker, publication records that output was committed, and only a
// complete authoritative evaluation record makes a mandatory gate satisfied.
//
// Exit code 0 means every step succeeded and the final state is self-consistent.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
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
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/worker.hpp"
#include "autonomous_foundry/workspace.hpp"

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

std::string describe(const af::Status& status) {
  std::string text(af::error_code_name(status.code()));
  text.append(": ");
  text.append(status.message());
  return text;
}

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

// A scratch tree that is removed on every exit path, including failure.
struct ScratchGuard {
  std::filesystem::path root;

  explicit ScratchGuard(std::filesystem::path path) : root(std::move(path)) {}
  ScratchGuard(const ScratchGuard&) = delete;
  ScratchGuard& operator=(const ScratchGuard&) = delete;
  ~ScratchGuard() {
    if (!root.empty()) {
      std::error_code error;
      (void)af::remove_tree_bounded(root);
      std::filesystem::remove_all(root, error);
    }
  }
};

// ---------------------------------------------------------------------------
// Worker slots
// ---------------------------------------------------------------------------

enum class SlotStrategy { ClosedForm, Iterative, OffByOne };

const char* slot_strategy_label(SlotStrategy strategy) {
  switch (strategy) {
    case SlotStrategy::ClosedForm:
      return "reference-closed-form";
    case SlotStrategy::Iterative:
      return "reference-iterative";
    case SlotStrategy::OffByOne:
      return "reference-off-by-one";
  }
  return "reference-unknown";
}

af::ReferenceStrategy slot_reference_strategy(SlotStrategy strategy) {
  switch (strategy) {
    case SlotStrategy::ClosedForm:
      return af::ReferenceStrategy::ClosedForm;
    case SlotStrategy::Iterative:
      return af::ReferenceStrategy::Iterative;
    case SlotStrategy::OffByOne:
      return af::ReferenceStrategy::OffByOne;
  }
  return af::ReferenceStrategy::OffByOne;
}

struct WorkerSlot {
  std::string label;
  SlotStrategy strategy{SlotStrategy::ClosedForm};
  af::WorkerId worker;
  af::WorkerSessionAuthority session;
  af::WorkerOperationAuthority authority;
  af::CandidateId candidate;
  std::string source;
};

af::WorkerOperationAuthority make_operation_authority(const af::PendingDispatch& pending) {
  af::WorkerOperationAuthority authority;
  authority.session = pending.session;
  authority.population = pending.attempt.population;
  authority.population_generation = pending.attempt.population_generation;
  authority.task = pending.attempt.task;
  authority.task_generation = pending.attempt.task_generation;
  authority.candidate = pending.attempt.candidate;
  authority.candidate_generation = pending.attempt.candidate_generation;
  authority.attempt = pending.attempt.id;
  authority.attempt_generation = pending.attempt.generation;
  authority.assignment = pending.attempt.assignment;
  return authority;
}

// ---------------------------------------------------------------------------
// Production for one worker slot.
//
// Dispatch and publication are deliberately separate phases. Every worker slot
// is dispatched while the population is still Running, because a population
// that has already committed a publication is Evaluating and refuses new
// production work until that output has been evaluated. Acknowledging an
// assignment is not completion, and dispatching an assignment is not
// publication.
// ---------------------------------------------------------------------------

bool dispatch_attempt(Report& report, af::FoundryCore& core, WorkerSlot& slot) {
  const af::Result<af::PendingDispatch> pending = core.authorize_attempt(slot.session);
  if (!report.expect_status(pending.status(), slot.label + ": authorize_attempt")) {
    return false;
  }
  const af::PendingDispatch dispatch = pending.value();
  report.expect(dispatch.attempt.state == af::AttemptState::Authorized,
                slot.label + ": attempt " + dispatch.attempt.id.to_string() +
                    " is durable but not yet dispatched");

  const af::Result<af::AttemptPackage> package =
      core.confirm_dispatch(slot.session, dispatch.attempt.id, dispatch.attempt.generation);
  if (!report.expect_status(package.status(), slot.label + ": confirm_dispatch")) {
    return false;
  }
  bool declares_source = false;
  for (const std::string& required : package.value().required_outputs) {
    if (required == std::string(af::kReferenceTaskSourceArtifact)) {
      declares_source = true;
    }
  }
  report.expect(declares_source, slot.label + ": task package requires solution.cpp");
  report.expect(package.value().input_files.size() == 1,
                slot.label + ": task package carries exactly one input file");
  report.expect(!package.value().workspace_root.empty(),
                slot.label + ": task package names a confined workspace root");

  slot.authority = make_operation_authority(dispatch);
  slot.candidate = dispatch.attempt.candidate;
  if (!report.expect_status(core.acknowledge_attempt(slot.authority),
                            slot.label + ": acknowledge_attempt")) {
    return false;
  }
  return true;
}

bool publish_result(Report& report, af::FoundryCore& core, WorkerSlot& slot) {
  slot.source = af::generate_reference_solution(slot_reference_strategy(slot.strategy));
  af::ArtifactRef artifact;
  artifact.name = std::string(af::kReferenceTaskSourceArtifact);
  artifact.size_bytes = static_cast<std::uint64_t>(slot.source.size());
  artifact.content_digest = af::sha256_hex(slot.source);

  std::vector<af::ArtifactRef> artifacts;
  artifacts.push_back(artifact);

  const af::Result<af::PublicationOutcome> published =
      core.publish_candidate(slot.authority, artifacts, slot_strategy_label(slot.strategy));
  if (!report.expect_status(published.status(), slot.label + ": publish_candidate")) {
    return false;
  }
  report.expect(published.value().candidate == slot.candidate,
                slot.label + ": publication names candidate " + slot.candidate.to_string() +
                    " at generation " + published.value().candidate_generation.to_string());
  return true;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

struct ToolchainProbe {
  bool usable{false};
  af::ReferenceToolchain toolchain;
  std::string description;
};

ToolchainProbe probe_toolchain() {
  ToolchainProbe probe;
  const af::Result<af::ReferenceToolchain> resolved = af::resolve_reference_toolchain();
  if (!resolved.ok()) {
    probe.description = describe(resolved.status());
    return probe;
  }
  probe.toolchain = resolved.value();
  probe.usable = probe.toolchain.usable();
  probe.description = probe.toolchain.describe();
  return probe;
}

bool record_evaluations(Report& report, af::FoundryCore& core, const af::TaskSpec& task,
                        af::PopulationId population, af::PopulationGeneration population_generation,
                        af::IdAllocator& allocator, const std::filesystem::path& scratch_root,
                        const ToolchainProbe& probe, std::vector<WorkerSlot>& slots,
                        std::vector<af::EvaluationRecord>& recorded) {
  const af::EvaluatorRegistry registry = af::EvaluatorRegistry::make_reference();
  std::map<std::string, af::EvaluatorId> evaluator_ids;

  for (WorkerSlot& slot : slots) {
    const af::Result<af::CandidateRecord> candidate = core.candidate(slot.candidate);
    if (!report.expect_status(candidate.status(),
                              "candidate " + slot.candidate.to_string() + " readable")) {
      return false;
    }
    const af::CandidateRecord record = candidate.value();
    if (!report.expect_status(core.begin_evaluation(record.id),
                              slot.label + ": begin_evaluation on " + record.id.to_string())) {
      return false;
    }

    const std::filesystem::path candidate_scratch =
        scratch_root / ("candidate-" + std::to_string(record.id.counter()));
    std::error_code directory_error;
    std::filesystem::create_directories(candidate_scratch, directory_error);
    if (!report.expect(!directory_error,
                       slot.label + ": scratch directory created for " + record.id.to_string())) {
      return false;
    }

    for (const af::EvaluationRequirement& requirement : task.requirements) {
      const std::shared_ptr<af::Evaluator> evaluator = registry.find(requirement.evaluator_key);
      if (evaluator == nullptr) {
        report.expect(false, slot.label + ": registry exposes '" + requirement.evaluator_key + "'");
        return false;
      }

      af::EvaluatorId& evaluator_id = evaluator_ids[requirement.evaluator_key];
      if (!evaluator_id.valid()) {
        const af::Result<af::EvaluatorId> minted = allocator.next<af::EvaluatorIdTag>();
        if (!report.expect_status(minted.status(),
                                  "evaluator identity minted for '" + requirement.evaluator_key +
                                      "'")) {
          return false;
        }
        evaluator_id = minted.value();
      }

      af::EvaluationRequest request;
      request.candidate = record.id;
      request.candidate_generation = record.generation;
      request.task = task.id;
      request.task_generation = task.generation;
      request.population = population;
      request.population_generation = population_generation;
      request.artifacts.emplace(std::string(af::kReferenceTaskSourceArtifact), slot.source);
      for (const af::InputFile& input : task.inputs) {
        request.inputs.emplace(input.name, input.content);
      }
      request.evaluator_key = requirement.evaluator_key;
      request.requirement_class = requirement.requirement_class;
      request.scratch_directory = candidate_scratch;
      request.toolchain = probe.toolchain;

      const af::EvaluationResult result = evaluator->evaluate(request);

      af::EvaluationRecord evaluation;
      evaluation.candidate = record.id;
      evaluation.candidate_generation = record.generation;
      evaluation.task = task.id;
      evaluation.task_generation = task.generation;
      evaluation.population = population;
      evaluation.population_generation = population_generation;
      evaluation.evaluator = evaluator_id;
      evaluation.evaluator_key = requirement.evaluator_key;
      evaluation.kind = evaluator->kind();
      evaluation.requirement_class = requirement.requirement_class;
      evaluation.outcome = result.outcome;
      evaluation.complete = true;
      evaluation.has_score = result.has_score;
      evaluation.score = result.score;
      evaluation.decided_epoch = core.epoch();
      evaluation.diagnostics = result.diagnostics;
      evaluation.evidence_digest = result.evidence_digest;
      evaluation.duration_micros = result.duration_micros;

      const std::string label = slot.label + ": record_evaluation '" + requirement.evaluator_key +
                                "' as " + std::string(af::evaluation_outcome_name(result.outcome));
      if (!report.expect_status(core.record_evaluation(evaluation), label)) {
        return false;
      }
      recorded.push_back(evaluation);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

void print_candidate_states(af::FoundryCore& core, const std::vector<WorkerSlot>& slots,
                            const af::TaskSpec& task) {
  Report::section("candidate states");
  for (const WorkerSlot& slot : slots) {
    const af::Result<af::CandidateRecord> candidate = core.candidate(slot.candidate);
    if (!candidate.ok()) {
      Report::item(slot.label + ": candidate " + slot.candidate.to_string() + " unreadable: " +
                   describe(candidate.status()));
      continue;
    }
    const af::CandidateRecord& record = candidate.value();
    std::string summary = slot.label;
    summary.append("  candidate=").append(record.id.to_string());
    summary.append("  state=").append(af::candidate_state_name(record.state));
    summary.append("  artifacts=").append(std::to_string(record.artifacts.size()));
    summary.append("  attempts=").append(std::to_string(record.attempt_count));
    summary.append("  depth=").append(std::to_string(record.depth));
    summary.append("  diversity=").append(record.diversity_key);
    Report::item(summary);

    if (!record.artifacts.empty()) {
      Report::note("artifact " + record.artifacts.front().name + " bytes=" +
                   std::to_string(record.artifacts.front().size_bytes) + " digest=" +
                   record.artifacts.front().content_digest.substr(0, 16));
    }

    const af::Result<af::AttemptRecord> attempt = core.attempt(record.attempt);
    if (attempt.ok()) {
      Report::note("attempt " + attempt.value().id.to_string() +
                   " state=" + std::string(af::attempt_state_name(attempt.value().state)));
    }

    for (const af::EvaluationRequirement& requirement : task.requirements) {
      const std::vector<af::EvaluationRecord> evidence = core.candidate_evaluations(record.id);
      const af::EvaluationRecord* found = nullptr;
      for (const af::EvaluationRecord& item : evidence) {
        if (item.evaluator_key == requirement.evaluator_key && item.complete) {
          found = &item;
        }
      }
      std::string text = "requirement '";
      text.append(requirement.evaluator_key);
      text.append("' (");
      text.append(af::requirement_class_name(requirement.requirement_class));
      text.append("): ");
      if (found == nullptr) {
        text.append("no complete record");
      } else {
        text.append(af::evaluation_outcome_name(found->outcome));
        text.append(" by ");
        text.append(af::evaluator_kind_name(found->kind));
        if (found->has_score) {
          text.append(" score=");
          text.append(std::to_string(found->score));
        }
      }
      Report::note(text);
    }
  }
}

void print_statistics(af::FoundryCore& core) {
  const af::FoundryStatistics statistics = core.statistics();
  Report::section("foundry statistics");
  Report::item("populations=" + std::to_string(statistics.populations_created) +
               " candidate_slots=" + std::to_string(statistics.candidate_slots_created) +
               " published=" + std::to_string(statistics.candidates_published));
  Report::item("attempts_authorized=" + std::to_string(statistics.attempts_authorized) +
               " attempts_completed=" + std::to_string(statistics.attempts_completed) +
               " attempts_failed=" + std::to_string(statistics.attempts_failed));
  Report::item("evaluations_recorded=" + std::to_string(statistics.evaluations_recorded) +
               " stale_authority_rejections=" +
               std::to_string(statistics.stale_authority_rejections) +
               " duplicate_rejections=" + std::to_string(statistics.duplicate_rejections));
}

}  // namespace

int main() {
  Report report;
  Report::line("Autonomous Foundry reference example: basic population");
  Report::line("task: " + std::string(af::kReferenceTaskApiName) +
               " compiled and executed by the reference evaluator");

  OperatorContext context;
  if (!build_operator(report, context)) {
    return 1;
  }
  std::unique_ptr<af::FoundryCore> core = make_core(context);
  Report::note("foundry=" + context.foundry.to_string() + " run=" + context.run.to_string() +
               " epoch=" + core->epoch().to_string());

  Report::section("policy and task");
  const af::Result<af::PolicyId> policy_id = [&]() {
    af::PolicyId minted;
    const af::Result<af::PolicyId> allocated = context.allocator.next<af::PolicyIdTag>();
    if (!allocated.ok()) {
      return allocated;
    }
    minted = allocated.value();
    return core->define_policy(af::make_reference_policy(minted, af::PolicyGeneration::first()));
  }();
  if (!report.expect_status(policy_id.status(), "reference policy defined")) {
    return 1;
  }
  const af::FoundryPolicy policy = core->policy(policy_id.value()).value();
  Report::item("policy " + policy.name + " factors=" + std::to_string(policy.selection.factors.size()) +
               " retain_top_k=" + std::to_string(policy.retention.retain_top_k));

  const af::Result<af::TaskId> task_id = [&]() {
    af::TaskId minted;
    const af::Result<af::TaskId> allocated = context.allocator.next<af::TaskIdTag>();
    if (!allocated.ok()) {
      return af::Result<af::TaskId>(allocated.status());
    }
    minted = allocated.value();
    const af::Result<af::TaskSpec> spec = af::make_reference_task(
        minted, policy_id.value(), policy.generation, 3u, af::BudgetLimits{});
    if (!spec.ok()) {
      return af::Result<af::TaskId>(spec.status());
    }
    return core->define_task(spec.value());
  }();
  if (!report.expect_status(task_id.status(), "reference task defined")) {
    return 1;
  }
  const af::TaskSpec task = core->task(task_id.value()).value();
  Report::item("task " + task.name + " generation=" + task.generation.to_string() +
               " requirements=" + std::to_string(task.requirements.size()) +
               " mandatory=" + std::to_string(task.mandatory_requirement_count()));
  Report::item("task digest=" + task.content_digest.substr(0, 16));

  Report::section("population");
  af::PopulationSpec population_spec;
  population_spec.name = "P1-basic";
  population_spec.task = task.id;
  population_spec.task_generation = task.generation;
  population_spec.policy = policy.id;
  population_spec.policy_generation = policy.generation;
  population_spec.candidate_budget = 3;
  population_spec.worker_budget = 3;
  population_spec.population_index = 1;

  const af::Result<af::PopulationId> population_id = core->create_population(population_spec);
  if (!report.expect_status(population_id.status(), "population created")) {
    return 1;
  }
  const af::PopulationId population = population_id.value();
  const std::uint32_t creation_generation = core->population(population).value().generation.value();
  if (!report.expect_status(core->start_population(population, af::PopulationGeneration::from_value(
                                                          creation_generation)),
                            "population started")) {
    return 1;
  }
  const af::PopulationRecord started = core->population(population).value();
  const af::PopulationGeneration population_generation = started.generation;
  Report::item("population " + population.to_string() + " state=" +
               std::string(af::population_state_name(started.state)) +
               " generation=" + population_generation.to_string());

  Report::section("worker slots");
  std::vector<WorkerSlot> slots;
  const SlotStrategy strategies[] = {SlotStrategy::ClosedForm, SlotStrategy::Iterative,
                                     SlotStrategy::OffByOne};
  for (std::size_t index = 0; index < 3; ++index) {
    WorkerSlot slot;
    slot.label = "slot-" + std::to_string(index + 1);
    slot.strategy = strategies[index];

    const af::Result<af::WorkerId> worker_id = context.allocator.next<af::WorkerIdTag>();
    if (!report.expect_status(worker_id.status(), slot.label + ": worker identity minted")) {
      return 1;
    }
    slot.worker = worker_id.value();

    af::WorkerRegistrationRequest request;
    request.worker = slot.worker;
    request.boot = af::make_worker_boot_id();
    request.label = slot.label;
    request.capability = "reference-cpp20";
    request.process_id = 0;

    const af::Result<af::WorkerSessionAuthority> session = core->register_worker(request);
    if (!report.expect_status(session.status(), slot.label + ": register_worker")) {
      return 1;
    }
    slot.session = session.value();
    if (!report.expect_status(core->worker_ready(slot.session, "reference-cpp20"),
                              slot.label + ": worker_ready")) {
      return 1;
    }
    Report::item(slot.label + " worker=" + slot.worker.to_string() +
                 " session=" + slot.session.session.to_string() +
                 " session_generation=" + slot.session.session_generation.to_string() +
                 " strategy=" + slot_strategy_label(slot.strategy));
    slots.push_back(slot);
  }

  Report::section("production: authorize, dispatch, acknowledge");
  for (WorkerSlot& slot : slots) {
    if (!dispatch_attempt(report, *core, slot)) {
      return 1;
    }
  }
  const af::PopulationRecord dispatched = core->population(population).value();
  Report::item("population state after dispatch=" +
               std::string(af::population_state_name(dispatched.state)) +
               " candidates=" + std::to_string(dispatched.candidates.size()));

  Report::section("publication");
  for (WorkerSlot& slot : slots) {
    if (!publish_result(report, *core, slot)) {
      return 1;
    }
  }
  const af::PopulationRecord producing = core->population(population).value();
  Report::item("population state after publication=" +
               std::string(af::population_state_name(producing.state)) +
               " candidates=" + std::to_string(producing.candidates.size()));

  const ToolchainProbe probe = probe_toolchain();
  Report::section("evaluation");
  Report::item("toolchain: " + probe.description);
  if (!probe.usable) {
    Report::note("the compile-backed mandatory gate cannot be decided without a toolchain; the "
                 "registry is still consulted and every record below states that honestly");
  }

  const af::Result<std::filesystem::path> scratch = af::make_transient_directory("af-basic-pop");
  if (!report.expect_status(scratch.status(), "evaluation scratch directory created")) {
    return 1;
  }
  const ScratchGuard scratch_guard(scratch.value());

  std::vector<af::EvaluationRecord> recorded;
  if (!record_evaluations(report, *core, task, population, population_generation, context.allocator,
                          scratch.value(), probe, slots, recorded)) {
    return 1;
  }
  Report::item("evaluation records ingested: " + std::to_string(recorded.size()));
  report.expect(recorded.size() == slots.size() * task.requirements.size(),
                "every declared requirement produced one record per candidate");

  print_candidate_states(*core, slots, task);
  print_statistics(*core);

  Report::section("self-consistency audit");
  const std::vector<std::string> violations = core->audit();
  report.expect(violations.empty(), "audit reports no violations");
  for (const std::string& violation : violations) {
    Report::note(violation);
  }

  if (probe.usable) {
    for (const WorkerSlot& slot : slots) {
      const af::CandidateRecord record = core->candidate(slot.candidate).value();
      report.expect(record.state == af::CandidateState::Evaluated,
                    slot.label + ": candidate reached Evaluated once every requirement had a "
                                "complete authoritative record");
    }
  } else {
    Report::note("without a toolchain the mandatory compile-and-run gate stays undecided, so no "
                 "candidate is reported as Evaluated in this environment");
  }

  if (report.failures == 0) {
    Report::line("\nRESULT: PASS");
    return 0;
  }
  Report::line("\nRESULT: FAIL failures=" + std::to_string(report.failures));
  return 1;
}
