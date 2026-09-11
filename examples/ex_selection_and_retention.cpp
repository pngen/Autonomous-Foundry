// examples/ex_selection_and_retention.cpp
//
// Selection and retention are different decisions.
//
// One population is filled with five candidates through the real production
// path, evaluated through the real evaluator registry, and then put through:
//
//   prepare_selection -> commit_selection -> prepare_retention -> commit_retention
//
// Selection answers "which candidate may advance". Retention answers "which
// alternatives remain worth keeping". The same population produces a different
// answer to each question, and this example prints both.
//
// The candidate set is built so that every selection stage is observable:
//
//   closed-form     passes both mandatory gates, fastest measured workload
//   iterative       passes both mandatory gates, slower measured workload
//   self-report     the producing worker reports an unconditional PASS, and the
//                   mandatory execution gate still fails: a worker's own claim
//                   is evidence about the worker, never authority over a gate
//   off-by-one      wrong answer, caught by execution
//   forbidden       correct and fast, but its source mentions a token the
//                   static source policy forbids, so the hard gate fails
//
// The compile-backed mandatory gate needs a real C++ toolchain. When none is
// resolvable the example records honest UNSUPPORTED evidence, prints why no
// ranking could be entered, and exits successfully: an environment limitation
// is not a runtime failure.

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
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
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

std::string number(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return std::string(buffer);
}

const char* direction_name(af::RankingDirection direction) {
  switch (direction) {
    case af::RankingDirection::HigherIsBetter:
      return "higher-is-better";
    case af::RankingDirection::LowerIsBetter:
      return "lower-is-better";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Operator identities, foundry, scratch
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
// Candidate plans
// ---------------------------------------------------------------------------

struct CandidatePlan {
  std::string label;
  std::string strategy_label;
  std::string source;
  /// When true the producing worker also files its own claim of success under
  /// the "worker-self-report" key. That record is complete and it is a PASS,
  /// and it still cannot close a mandatory gate.
  bool worker_self_report{false};
  af::CandidateId candidate;
  af::WorkerOperationAuthority authority;
};

std::string forbidden_token_source() {
  std::string source = af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);
  source.append("\n// This line is the reason the static source policy rejects the "
                "candidate: the rule is a textual scan and it finds the token system( here.\n");
  return source;
}

std::vector<CandidatePlan> make_plans() {
  std::vector<CandidatePlan> plans;

  CandidatePlan closed_form;
  closed_form.label = "closed-form";
  closed_form.strategy_label = "reference-closed-form";
  closed_form.source = af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);
  plans.push_back(closed_form);

  CandidatePlan iterative;
  iterative.label = "iterative";
  iterative.strategy_label = "reference-iterative";
  iterative.source = af::generate_reference_solution(af::ReferenceStrategy::Iterative);
  plans.push_back(iterative);

  CandidatePlan self_report;
  self_report.label = "self-report";
  self_report.strategy_label = "reference-self-reported-pass";
  self_report.source = af::generate_reference_solution(af::ReferenceStrategy::SelfReportedPass);
  self_report.worker_self_report = true;
  plans.push_back(self_report);

  CandidatePlan off_by_one;
  off_by_one.label = "off-by-one";
  off_by_one.strategy_label = "reference-off-by-one";
  off_by_one.source = af::generate_reference_solution(af::ReferenceStrategy::OffByOne);
  plans.push_back(off_by_one);

  CandidatePlan forbidden;
  forbidden.label = "forbidden-token";
  forbidden.strategy_label = "reference-closed-form-with-forbidden-token";
  forbidden.source = forbidden_token_source();
  plans.push_back(forbidden);

  return plans;
}

// ---------------------------------------------------------------------------
// Production
// ---------------------------------------------------------------------------

bool dispatch_candidate(Report& report, af::FoundryCore& core,
                        const af::WorkerSessionAuthority& session, CandidatePlan& plan) {
  const af::Result<af::PendingDispatch> pending = core.authorize_attempt(session);
  if (!report.expect_status(pending.status(), plan.label + ": authorize_attempt")) {
    return false;
  }
  const af::PendingDispatch dispatch = pending.value();
  const af::Result<af::AttemptPackage> package =
      core.confirm_dispatch(session, dispatch.attempt.id, dispatch.attempt.generation);
  if (!report.expect_status(package.status(), plan.label + ": confirm_dispatch")) {
    return false;
  }

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
  if (!report.expect_status(core.acknowledge_attempt(authority),
                            plan.label + ": acknowledge_attempt")) {
    return false;
  }
  plan.candidate = dispatch.attempt.candidate;
  plan.authority = authority;
  return true;
}

bool publish_candidate(Report& report, af::FoundryCore& core, const CandidatePlan& plan) {
  af::ArtifactRef artifact;
  artifact.name = std::string(af::kReferenceTaskSourceArtifact);
  artifact.size_bytes = static_cast<std::uint64_t>(plan.source.size());
  artifact.content_digest = af::sha256_hex(plan.source);

  std::vector<af::ArtifactRef> artifacts;
  artifacts.push_back(artifact);
  const af::Result<af::PublicationOutcome> published =
      core.publish_candidate(plan.authority, artifacts, plan.strategy_label);
  return report.expect_status(published.status(), plan.label + ": publish_candidate");
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

af::EvaluatorId evaluator_identity(Report& report, af::IdAllocator& allocator,
                                    std::map<std::string, af::EvaluatorId>& known,
                                    const std::string& key) {
  af::EvaluatorId& slot = known[key];
  if (slot.valid()) {
    return slot;
  }
  const af::Result<af::EvaluatorId> minted = allocator.next<af::EvaluatorIdTag>();
  if (!report.expect_status(minted.status(), "evaluator identity minted for '" + key + "'")) {
    return af::EvaluatorId();
  }
  slot = minted.value();
  return slot;
}

bool file_record(Report& report, af::FoundryCore& core, const af::CandidateRecord& candidate,
                 af::PopulationId population, const af::TaskSpec& task,
                 const af::EvaluationRequirement& requirement, af::EvaluatorId evaluator,
                 af::EvaluatorKind kind, const af::EvaluationResult& result,
                 const std::string& label) {
  af::EvaluationRecord record;
  record.candidate = candidate.id;
  record.candidate_generation = candidate.generation;
  record.task = task.id;
  record.task_generation = task.generation;
  record.population = population;
  record.population_generation = core.population(population).value().generation;
  record.evaluator = evaluator;
  record.evaluator_key = requirement.evaluator_key;
  record.kind = kind;
  record.requirement_class = requirement.requirement_class;
  record.outcome = result.outcome;
  record.complete = true;
  record.has_score = result.has_score;
  record.score = result.score;
  record.decided_epoch = core.epoch();
  record.diagnostics = result.diagnostics;
  record.evidence_digest = result.evidence_digest;
  record.duration_micros = result.duration_micros;
  return report.expect_status(core.record_evaluation(record), label);
}

bool evaluate_plans(Report& report, af::FoundryCore& core, const af::TaskSpec& task,
                    af::PopulationId population, af::IdAllocator& allocator,
                    const std::filesystem::path& scratch_root, const ToolchainProbe& probe,
                    std::vector<CandidatePlan>& plans) {
  const af::EvaluatorRegistry registry = af::EvaluatorRegistry::make_reference();
  std::map<std::string, af::EvaluatorId> evaluator_ids;

  for (CandidatePlan& plan : plans) {
    const af::Result<af::CandidateRecord> read = core.candidate(plan.candidate);
    if (!report.expect_status(read.status(), plan.label + ": candidate readable")) {
      return false;
    }
    const af::CandidateRecord candidate = read.value();
    if (!report.expect_status(core.begin_evaluation(candidate.id),
                              plan.label + ": begin_evaluation")) {
      return false;
    }

    const std::filesystem::path scratch =
        scratch_root / ("candidate-" + std::to_string(candidate.id.counter()));
    std::error_code directory_error;
    std::filesystem::create_directories(scratch, directory_error);
    if (!report.expect(!directory_error, plan.label + ": scratch directory created")) {
      return false;
    }

    for (const af::EvaluationRequirement& requirement : task.requirements) {
      const std::shared_ptr<af::Evaluator> evaluator = registry.find(requirement.evaluator_key);
      if (evaluator == nullptr) {
        report.expect(false, plan.label + ": registry exposes '" + requirement.evaluator_key + "'");
        return false;
      }
      const af::EvaluatorId evaluator_id =
          evaluator_identity(report, allocator, evaluator_ids, requirement.evaluator_key);
      if (!evaluator_id.valid()) {
        return false;
      }

      af::EvaluationRequest request;
      request.candidate = candidate.id;
      request.candidate_generation = candidate.generation;
      request.task = task.id;
      request.task_generation = task.generation;
      request.population = population;
      request.population_generation = core.population(population).value().generation;
      request.artifacts.emplace(std::string(af::kReferenceTaskSourceArtifact), plan.source);
      for (const af::InputFile& input : task.inputs) {
        request.inputs.emplace(input.name, input.content);
      }
      request.evaluator_key = requirement.evaluator_key;
      request.requirement_class = requirement.requirement_class;
      request.scratch_directory = scratch;
      request.toolchain = probe.toolchain;

      const af::EvaluationResult result = evaluator->evaluate(request);
      const std::string label = plan.label + ": record '" + requirement.evaluator_key + "'=" +
                                std::string(af::evaluation_outcome_name(result.outcome));
      if (!file_record(report, core, candidate, population, task, requirement, evaluator_id,
                       evaluator->kind(), result, label)) {
        return false;
      }
    }

    if (plan.worker_self_report) {
      // The producing worker claims unconditional success. The record is filed
      // under its own key with the WorkerSelfReport kind, so it is durable
      // evidence about the worker and never authority over a declared gate.
      af::EvaluationRequirement claim;
      claim.requirement_class = af::RequirementClass::Mandatory;
      claim.evaluator_key = "worker-self-report";
      claim.description = "the producing worker's own claim about its output";

      af::EvaluationResult result;
      result.outcome = af::EvaluationOutcome::Pass;
      result.diagnostics = "worker claims the conformance harness exits with status 0";
      result.evidence_digest = af::sha256_hex(plan.source + "|worker-self-report");
      const af::EvaluatorId evaluator_id =
          evaluator_identity(report, allocator, evaluator_ids, claim.evaluator_key);
      if (!evaluator_id.valid()) {
        return false;
      }
      if (!file_record(report, core, candidate, population, task, claim, evaluator_id,
                       af::EvaluatorKind::WorkerSelfReport, result,
                       plan.label + ": record 'worker-self-report'=Pass")) {
        return false;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

void print_evidence_summary(af::FoundryCore& core, const std::vector<CandidatePlan>& plans,
                            const af::TaskSpec& task) {
  Report::section("evidence per candidate");
  for (const CandidatePlan& plan : plans) {
    const af::CandidateRecord candidate = core.candidate(plan.candidate).value();
    Report::item(plan.label + "  candidate=" + candidate.id.to_string() +
                 "  state=" + std::string(af::candidate_state_name(candidate.state)));
    for (const af::EvaluationRequirement& requirement : task.requirements) {
      const std::vector<af::EvaluationRecord> records = core.candidate_evaluations(candidate.id);
      const af::EvaluationRecord* found = nullptr;
      for (const af::EvaluationRecord& record : records) {
        if (record.evaluator_key == requirement.evaluator_key && record.complete) {
          found = &record;
        }
      }
      std::string text = requirement.evaluator_key;
      text.append(" (");
      text.append(af::requirement_class_name(requirement.requirement_class));
      text.append("): ");
      if (found == nullptr) {
        text.append("no complete record");
      } else {
        text.append(af::evaluation_outcome_name(found->outcome));
        text.append("  evaluator_kind=");
        text.append(af::evaluator_kind_name(found->kind));
        text.append("  authoritative=");
        text.append(af::evaluator_kind_is_authoritative(found->kind) ? "yes" : "no");
        if (found->has_score) {
          text.append("  score=");
          text.append(number(found->score));
        }
      }
      Report::note(text);
    }
    const std::vector<af::EvaluationRecord> records = core.candidate_evaluations(candidate.id);
    for (const af::EvaluationRecord& record : records) {
      if (record.evaluator_key == "worker-self-report") {
        Report::note("worker-self-report: " + std::string(af::evaluation_outcome_name(record.outcome)) +
                     "  evaluator_kind=" + std::string(af::evaluator_kind_name(record.kind)) +
                     "  authoritative=" +
                     (af::evaluator_kind_is_authoritative(record.kind) ? "yes" : "no"));
      }
    }
  }
}

void print_ranking(const af::SelectionDecision& decision) {
  Report::section("ranking");
  if (decision.ranking.empty()) {
    Report::note("ranking is empty: " + decision.rationale);
    return;
  }
  for (const af::RankingEntry& entry : decision.ranking) {
    std::string header = "rank ";
    header.append(std::to_string(entry.rank));
    header.append("  candidate=");
    header.append(entry.candidate.to_string());
    header.append("  total_score=");
    header.append(number(entry.total_score));
    header.append("  lineage_depth=");
    header.append(std::to_string(entry.lineage_depth));
    header.append("  artifact_bytes=");
    header.append(std::to_string(entry.artifact_bytes));
    Report::item(header);
    for (const af::RankingFactorValue& factor : entry.factors) {
      std::string text = "factor '";
      text.append(factor.key);
      text.append("' kind=");
      text.append(af::ranking_factor_name(factor.kind));
      text.append(" direction=");
      text.append(direction_name(factor.direction));
      text.append(" weight=");
      text.append(number(factor.weight));
      text.append(" raw=");
      text.append(factor.available ? number(factor.raw_value) : std::string("unavailable"));
      text.append(" available=");
      text.append(factor.available ? "yes" : "no");
      text.append(" contribution=");
      text.append(number(factor.contribution));
      Report::note(text);
    }
  }
}

void print_exclusions(const af::SelectionDecision& decision) {
  Report::section("exclusions");
  if (decision.excluded.empty()) {
    Report::note("no candidate was excluded before ranking");
    return;
  }
  for (const af::ExclusionEntry& entry : decision.excluded) {
    std::string text = "candidate=";
    text.append(entry.candidate.to_string());
    text.append("  stage=");
    text.append(af::selection_stage_name(entry.stage));
    text.append("  reason=");
    text.append(af::exclusion_reason_name(entry.reason));
    Report::item(text);
    Report::note(entry.detail);
  }
}

void print_decision(const af::SelectionDecision& decision) {
  Report::section("committed selection decision");
  Report::item("state=" + std::string(af::selection_decision_state_name(decision.state)) +
               " generation=" + decision.generation.to_string());
  Report::item("population=" + decision.population.to_string() +
               " population_generation=" + decision.population_generation.to_string() +
               " policy_generation=" + decision.policy_generation.to_string());
  Report::item("ranked=" + std::to_string(decision.ranking.size()) +
               " excluded=" + std::to_string(decision.excluded.size()) +
               " has_winner=" + (decision.has_winner() ? "yes" : "no"));
  if (decision.has_winner()) {
    Report::item("selected=" + decision.selected.to_string() +
                 " generation=" + decision.selected_generation.to_string());
  }
  Report::item("canonical_state_digest=" + decision.canonical_state_digest.substr(0, 32));
  Report::note(decision.rationale);
}

void print_retention(const af::RetentionDecision& retention) {
  Report::section("committed retention decision");
  Report::item("selection_generation=" + retention.selection_generation.to_string() +
               " committed=" + (retention.committed ? "yes" : "no"));
  for (const af::RetentionDecisionEntry& entry : retention.entries) {
    std::string text = "candidate=";
    text.append(entry.candidate.to_string());
    text.append("  outcome=");
    text.append(af::retention_outcome_name(entry.outcome));
    if (entry.rank != 0) {
      text.append("  rank=");
      text.append(std::to_string(entry.rank));
    }
    Report::item(text);
    Report::note(entry.detail);
  }
  Report::item("retained=" + std::to_string(retention.retained.size()) +
               " retired=" + std::to_string(retention.retired.size()));
  Report::item("canonical_state_digest=" + retention.canonical_state_digest.substr(0, 32));
}

void print_candidate_flags(af::FoundryCore& core, const std::vector<CandidatePlan>& plans) {
  Report::section("candidate flags after retention");
  for (const CandidatePlan& plan : plans) {
    const af::CandidateRecord candidate = core.candidate(plan.candidate).value();
    Report::item(plan.label + "  state=" + std::string(af::candidate_state_name(candidate.state)) +
                 "  selected=" + (candidate.selected ? "yes" : "no") +
                 "  retained=" + (candidate.retained ? "yes" : "no"));
  }
}

}  // namespace

int main() {
  Report report;
  Report::line("Autonomous Foundry reference example: selection and retention");
  Report::line("selection decides who may advance; retention decides what stays worth keeping");

  OperatorContext context;
  if (!build_operator(report, context)) {
    return 1;
  }
  std::unique_ptr<af::FoundryCore> core = make_core(context);

  Report::section("policy and task");
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
  Report::item("policy " + policy.name +
               " ranking_factors=" + std::to_string(policy.selection.factors.size()) +
               " retain_top_k=" + std::to_string(policy.retention.retain_top_k) +
               " max_retained=" + std::to_string(policy.retention.max_retained) +
               " max_per_lineage=" + std::to_string(policy.retention.max_per_lineage));
  Report::item("require_complete_mandatory=" +
               std::string(policy.selection.require_complete_mandatory ? "true" : "false") +
               " (an incomplete evidence set never enters ranking)");

  af::TaskId task_id;
  {
    const af::Result<af::TaskId> minted = context.allocator.next<af::TaskIdTag>();
    if (!report.expect_status(minted.status(), "task identity minted")) {
      return 1;
    }
    const af::Result<af::TaskSpec> spec =
        af::make_reference_task(minted.value(), policy.id, policy.generation, 5u, af::BudgetLimits{});
    if (!report.expect_status(spec.status(), "reference task built")) {
      return 1;
    }
    const af::Result<af::TaskId> task = core->define_task(spec.value());
    if (!report.expect_status(task.status(), "reference task defined")) {
      return 1;
    }
    task_id = task.value();
  }
  const af::TaskSpec task = core->task(task_id).value();
  Report::item("task " + task.name + " requirements=" + std::to_string(task.requirements.size()) +
               " mandatory=" + std::to_string(task.mandatory_requirement_count()));
  for (const af::EvaluationRequirement& requirement : task.requirements) {
    Report::note(std::string(af::requirement_class_name(requirement.requirement_class)) + ": " +
                 requirement.evaluator_key);
  }

  Report::section("population and worker slots");
  af::PopulationSpec population_spec;
  population_spec.name = "P1-select-retain";
  population_spec.task = task.id;
  population_spec.task_generation = task.generation;
  population_spec.policy = policy.id;
  population_spec.policy_generation = policy.generation;
  population_spec.candidate_budget = 5;
  population_spec.worker_budget = 5;
  population_spec.population_index = 1;
  const af::Result<af::PopulationId> created = core->create_population(population_spec);
  if (!report.expect_status(created.status(), "population created")) {
    return 1;
  }
  const af::PopulationId population = created.value();
  if (!report.expect_status(
          core->start_population(population, core->population(population).value().generation),
          "population started")) {
    return 1;
  }

  std::vector<af::WorkerSessionAuthority> sessions;
  for (std::size_t index = 0; index < 5; ++index) {
    const af::Result<af::WorkerId> worker = context.allocator.next<af::WorkerIdTag>();
    if (!report.expect_status(worker.status(), "worker identity minted")) {
      return 1;
    }
    af::WorkerRegistrationRequest request;
    request.worker = worker.value();
    request.boot = af::make_worker_boot_id();
    request.label = "slot-" + std::to_string(index + 1);
    request.capability = "reference-cpp20";
    request.process_id = 0;
    const af::Result<af::WorkerSessionAuthority> session = core->register_worker(request);
    if (!report.expect_status(session.status(), request.label + ": register_worker")) {
      return 1;
    }
    if (!report.expect_status(core->worker_ready(session.value(), "reference-cpp20"),
                              request.label + ": worker_ready")) {
      return 1;
    }
    sessions.push_back(session.value());
  }
  Report::item("worker sessions ready=" + std::to_string(sessions.size()));

  Report::section("production");
  std::vector<CandidatePlan> plans = make_plans();
  for (std::size_t index = 0; index < plans.size(); ++index) {
    if (!dispatch_candidate(report, *core, sessions[index], plans[index])) {
      return 1;
    }
  }
  for (CandidatePlan& plan : plans) {
    if (!publish_candidate(report, *core, plan)) {
      return 1;
    }
  }
  const af::PopulationRecord producing = core->population(population).value();
  Report::item("population state=" + std::string(af::population_state_name(producing.state)) +
               " candidates=" + std::to_string(producing.candidates.size()));
  report.expect(producing.candidates.size() == plans.size(),
                "every planned candidate slot was registered and published");

  const ToolchainProbe probe = probe_toolchain();
  Report::section("evaluation");
  Report::item("toolchain: " + probe.description);

  const af::Result<std::filesystem::path> scratch =
      af::make_transient_directory("af-select-retain");
  if (!report.expect_status(scratch.status(), "evaluation scratch directory created")) {
    return 1;
  }
  const ScratchGuard scratch_guard(scratch.value());

  if (!evaluate_plans(report, *core, task, population, context.allocator, scratch.value(), probe,
                      plans)) {
    return 1;
  }
  print_evidence_summary(*core, plans, task);

  for (const CandidatePlan& plan : plans) {
    const af::CandidateRecord candidate = core->candidate(plan.candidate).value();
    report.expect(candidate.state == af::CandidateState::Evaluated,
                  plan.label + ": candidate is Evaluated (every declared requirement has a "
                              "complete record)");
  }

  Report::section("selection");
  const af::Result<af::SelectionDecision> prepared = core->prepare_selection(population);
  if (!report.expect_status(prepared.status(), "prepare_selection")) {
    return 1;
  }
  print_ranking(prepared.value());
  print_exclusions(prepared.value());

  if (prepared.value().state != af::SelectionDecisionState::Prepared ||
      !prepared.value().has_winner()) {
    Report::note("no candidate satisfied every mandatory gate, so ranking was not entered and no "
                 "decision can be committed");
    Report::note("the compile-backed gate is declared mandatory by the reference task; set "
                 "AF_REFERENCE_CXX to a C++20 compiler to exercise the full ranking path");
    Report::section("self-consistency audit");
    const std::vector<std::string> violations = core->audit();
    report.expect(violations.empty(), "audit reports no violations");
    for (const std::string& violation : violations) {
      Report::note(violation);
    }
    Report::line("\nRESULT: PASS (partial: the mandatory execution gate is UNSUPPORTED in this "
                 "environment)");
    return report.failures == 0 ? 0 : 1;
  }

  const af::Result<af::SelectionDecision> committed =
      core->commit_selection(prepared.value());
  if (!report.expect_status(committed.status(), "commit_selection")) {
    return 1;
  }
  print_decision(committed.value());
  const af::SelectionDecision selection = committed.value();
  report.expect(selection.state == af::SelectionDecisionState::Committed,
                "selection decision is Committed, not merely Prepared");
  report.expect(selection.ranking.size() == 2,
                "exactly the two candidates that passed every mandatory gate were ranked");
  report.expect(selection.excluded.size() == 3, "three candidates were excluded before ranking");

  const af::Result<af::RetentionDecision> prepared_retention =
      core->prepare_retention(population);
  if (!report.expect_status(prepared_retention.status(), "prepare_retention")) {
    return 1;
  }
  const af::RetentionDecision retention = prepared_retention.value();

  const af::Result<af::RetentionDecision> committed_retention =
      core->commit_retention(retention);
  if (!report.expect_status(committed_retention.status(), "commit_retention")) {
    return 1;
  }
  print_retention(committed_retention.value());
  print_candidate_flags(*core, plans);

  Report::section("selection and retention are different decisions");
  std::size_t selected_count = 0;
  std::size_t retained_count = 0;
  for (const CandidatePlan& plan : plans) {
    const af::CandidateRecord candidate = core->candidate(plan.candidate).value();
    if (candidate.selected) {
      ++selected_count;
    }
    if (candidate.retained) {
      ++retained_count;
    }
  }
  Report::item("selection named " + std::to_string(selected_count) +
               " candidate allowed to advance");
  Report::item("retention kept " + std::to_string(retained_count) +
               " candidates worth keeping and released " +
               std::to_string(committed_retention.value().retired.size()));
  report.expect(selected_count == 1, "exactly one candidate is the selection winner");
  report.expect(retained_count == 2,
                "retention kept the winner plus one alternative, which selection did not name");
  report.expect(committed_retention.value().entries.size() == plans.size(),
                "retention classified every candidate in the population, including the ones "
                "selection never ranked");
  report.expect(committed_retention.value().entries.size() > selection.ranking.size(),
                "retention classified more candidates than selection ranked: selection ranks only "
                "eligible candidates, retention classifies the whole population");
  report.expect(selection.excluded.size() != selection.ranking.size() &&
                    committed_retention.value().retained.size() < plans.size(),
                "the two decisions do not answer the same question: selection excluded three "
                "candidates from ranking while retention kept two of them");

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
