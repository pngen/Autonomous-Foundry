// examples/ex_lineage_evolution.cpp
//
// Evolution across populations, and what an obsolete authority can still do.
//
// The example runs a complete first generation, commits selection and
// retention, advances to a second population seeded by the winner, produces and
// evaluates a fresh generation there, and then prints the durable lineage DAG
// with ancestors, descendants and depth for every node.
//
// It closes by replaying the exact operation authority that published the first
// generation's winner. That authority is stale the moment the population
// advances, and the replay is refused with a stale-authority code. Nothing is
// mutated by the refusal: the winner is still the winner afterwards.
//
// The compile-backed mandatory gate needs a real C++ toolchain. Without one the
// example records honest UNSUPPORTED evidence, reports that a decision cannot be
// committed, and exits successfully.

#include <algorithm>
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
#include "autonomous_foundry/lineage.hpp"
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
// Production helpers
// ---------------------------------------------------------------------------

struct WorkerSession {
  af::WorkerId worker;
  af::WorkerSessionAuthority authority;
};

WorkerSession open_session(Report& report, af::FoundryCore& core, af::IdAllocator& allocator,
                           const std::string& label) {
  WorkerSession session;
  const af::Result<af::WorkerId> worker = allocator.next<af::WorkerIdTag>();
  if (!report.expect_status(worker.status(), label + ": worker identity minted")) {
    return session;
  }
  af::WorkerRegistrationRequest request;
  request.worker = worker.value();
  request.boot = af::make_worker_boot_id();
  request.label = label;
  request.capability = "reference-cpp20";
  request.process_id = 0;
  const af::Result<af::WorkerSessionAuthority> authority = core.register_worker(request);
  if (!report.expect_status(authority.status(), label + ": register_worker")) {
    return session;
  }
  if (!report.expect_status(core.worker_ready(authority.value(), "reference-cpp20"),
                            label + ": worker_ready")) {
    return session;
  }
  session.worker = worker.value();
  session.authority = authority.value();
  return session;
}

struct Production {
  af::CandidateId candidate;
  af::WorkerOperationAuthority authority;
  std::string source;
  std::string strategy;
};

bool produce(Report& report, af::FoundryCore& core, const af::WorkerSessionAuthority& session,
             const std::string& label, const std::string& strategy, std::string source,
             Production& production) {
  const af::Result<af::PendingDispatch> pending = core.authorize_attempt(session);
  if (!report.expect_status(pending.status(), label + ": authorize_attempt")) {
    return false;
  }
  const af::PendingDispatch dispatch = pending.value();
  if (!report.expect_status(
          core.confirm_dispatch(session, dispatch.attempt.id, dispatch.attempt.generation).status(),
          label + ": confirm_dispatch")) {
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
  if (!report.expect_status(core.acknowledge_attempt(authority), label + ": acknowledge_attempt")) {
    return false;
  }

  production.candidate = dispatch.attempt.candidate;
  production.authority = authority;
  production.source = std::move(source);
  production.strategy = strategy;
  return true;
}

bool publish(Report& report, af::FoundryCore& core, Production& production,
             const std::string& label) {
  af::ArtifactRef artifact;
  artifact.name = std::string(af::kReferenceTaskSourceArtifact);
  artifact.size_bytes = static_cast<std::uint64_t>(production.source.size());
  artifact.content_digest = af::sha256_hex(production.source);
  std::vector<af::ArtifactRef> artifacts;
  artifacts.push_back(artifact);
  return report.expect_status(
      core.publish_candidate(production.authority, artifacts, production.strategy).status(),
      label + ": publish_candidate");
}

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

bool evaluate_candidates(Report& report, af::FoundryCore& core, const af::TaskSpec& task,
                         af::PopulationId population, af::IdAllocator& allocator,
                         const std::filesystem::path& scratch_root, const ToolchainProbe& probe,
                         const std::vector<Production>& productions,
                         std::map<std::string, af::EvaluatorId>& evaluator_ids) {
  const af::EvaluatorRegistry registry = af::EvaluatorRegistry::make_reference();
  for (const Production& production : productions) {
    const af::CandidateId candidate_id = production.candidate;
    const af::Result<af::CandidateRecord> read = core.candidate(candidate_id);
    if (!report.expect_status(read.status(), candidate_id.to_string() + ": candidate readable")) {
      return false;
    }
    const af::CandidateRecord candidate = read.value();
    if (!report.expect_status(core.begin_evaluation(candidate.id),
                              candidate.id.to_string() + ": begin_evaluation")) {
      return false;
    }
    const std::filesystem::path scratch =
        scratch_root / ("candidate-" + std::to_string(candidate.id.counter()));
    std::error_code directory_error;
    std::filesystem::create_directories(scratch, directory_error);
    if (!report.expect(!directory_error, candidate.id.to_string() + ": scratch directory created")) {
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
        if (!report.expect_status(minted.status(),
                                  "evaluator identity minted for '" + requirement.evaluator_key +
                                      "'")) {
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
      request.artifacts.emplace(std::string(af::kReferenceTaskSourceArtifact), production.source);
      for (const af::InputFile& input : task.inputs) {
        request.inputs.emplace(input.name, input.content);
      }
      request.evaluator_key = requirement.evaluator_key;
      request.requirement_class = requirement.requirement_class;
      request.scratch_directory = scratch;
      request.toolchain = probe.toolchain;

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
      if (!report.expect_status(core.record_evaluation(record),
                                candidate.id.to_string() + ": record '" +
                                    requirement.evaluator_key + "'")) {
        return false;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Lineage presentation
// ---------------------------------------------------------------------------

std::vector<af::CandidateId> sorted_ids(std::vector<af::CandidateId> ids) {
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::string join_ids(const std::vector<af::CandidateId>& ids) {
  if (ids.empty()) {
    return "[]";
  }
  std::string text = "[";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index != 0) {
      text.append(", ");
    }
    text.append(ids[index].to_string());
  }
  text.push_back(']');
  return text;
}

void print_lineage(const af::FoundrySnapshot& snapshot) {
  Report::section("lineage DAG");
  const std::vector<af::LineageNode> nodes = [&snapshot]() {
    std::vector<af::LineageNode> collected;
    for (const auto& entry : snapshot.lineage.nodes()) {
      collected.push_back(entry.second);
    }
    std::sort(collected.begin(), collected.end(),
              [](const af::LineageNode& a, const af::LineageNode& b) {
                return a.candidate < b.candidate;
              });
    return collected;
  }();

  for (const af::LineageNode& node : nodes) {
    std::string text = "node candidate=";
    text.append(node.candidate.to_string());
    text.append("  lineage=");
    text.append(node.lineage.to_string());
    text.append("  depth=");
    text.append(std::to_string(node.depth));
    text.append("  parents=");
    text.append(join_ids(node.parents));
    text.append("  retired=");
    text.append(node.retired ? "yes" : "no");
    Report::item(text);
    Report::note("ancestors(nearest first)=" +
                 join_ids(sorted_ids(snapshot.lineage.ancestors_of(node.candidate))));
    Report::note("descendants=" +
                 join_ids(sorted_ids(snapshot.lineage.descendants_of(node.candidate))));
    Report::note("path_to_root=" + join_ids(snapshot.lineage.path_to_root(node.candidate)));
  }

  const af::Status validity = snapshot.lineage.validate();
  Report::note("lineage validation: " + (validity.ok() ? std::string("ok") : describe(validity)));
  Report::note("lineage nodes=" + std::to_string(snapshot.lineage.size()));
}

}  // namespace

int main() {
  Report report;
  Report::line("Autonomous Foundry reference example: lineage evolution");
  Report::line("a winner's lineage continues into the next population; its authority does not");

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

  af::TaskId task_id;
  {
    const af::Result<af::TaskId> minted = context.allocator.next<af::TaskIdTag>();
    if (!report.expect_status(minted.status(), "task identity minted")) {
      return 1;
    }
    const af::Result<af::TaskSpec> spec = af::make_reference_task(
        minted.value(), policy.id, policy.generation, 3u, af::BudgetLimits{});
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
  Report::item("policy " + policy.name + " task " + task.name +
               " max_generation_depth=" + std::to_string(policy.max_generation_depth));

  const ToolchainProbe probe = probe_toolchain();
  Report::item("toolchain: " + probe.description);

  const af::Result<std::filesystem::path> scratch =
      af::make_transient_directory("af-lineage-evolution");
  if (!report.expect_status(scratch.status(), "evaluation scratch directory created")) {
    return 1;
  }
  const ScratchGuard scratch_guard(scratch.value());

  Report::section("generation 1: population P1");
  af::PopulationSpec p1_spec;
  p1_spec.name = "P1-lineage";
  p1_spec.task = task.id;
  p1_spec.task_generation = task.generation;
  p1_spec.policy = policy.id;
  p1_spec.policy_generation = policy.generation;
  p1_spec.candidate_budget = 3;
  p1_spec.worker_budget = 3;
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

  std::vector<Production> generation_one;
  const std::string sources_one[] = {
      af::generate_reference_solution(af::ReferenceStrategy::ClosedForm),
      af::generate_reference_solution(af::ReferenceStrategy::Iterative),
      af::generate_reference_solution(af::ReferenceStrategy::OffByOne)};
  const std::string strategies_one[] = {"reference-closed-form", "reference-iterative",
                                        "reference-off-by-one"};
  for (std::size_t index = 0; index < 3; ++index) {
    const std::string label = "P1-slot-" + std::to_string(index + 1);
    const WorkerSession session = open_session(report, *core, context.allocator, label);
    if (!session.worker.valid()) {
      return 1;
    }
    Production production;
    if (!produce(report, *core, session.authority, label, strategies_one[index],
                 sources_one[index], production)) {
      return 1;
    }
    generation_one.push_back(production);
  }
  for (std::size_t index = 0; index < generation_one.size(); ++index) {
    if (!publish(report, *core, generation_one[index],
                 "P1-slot-" + std::to_string(index + 1))) {
      return 1;
    }
  }

  std::map<std::string, af::EvaluatorId> evaluator_ids;
  std::vector<af::CandidateId> candidates_one;
  for (const Production& production : generation_one) {
    candidates_one.push_back(production.candidate);
  }
  if (!evaluate_candidates(report, *core, task, p1, context.allocator, scratch.value(), probe,
                           generation_one, evaluator_ids)) {
    return 1;
  }
  for (const af::CandidateId candidate_id : candidates_one) {
    const af::CandidateRecord candidate = core->candidate(candidate_id).value();
    Report::item("candidate " + candidate_id.to_string() +
                 " state=" + std::string(af::candidate_state_name(candidate.state)) +
                 " depth=" + std::to_string(candidate.depth) +
                 " lineage=" + candidate.lineage.to_string());
  }

  const af::Result<af::SelectionDecision> prepared_one = core->prepare_selection(p1);
  if (!report.expect_status(prepared_one.status(), "P1 prepare_selection")) {
    return 1;
  }
  if (prepared_one.value().state != af::SelectionDecisionState::Prepared ||
      !prepared_one.value().has_winner()) {
    Report::note("no P1 candidate satisfied every mandatory gate: " +
                 prepared_one.value().rationale);
    for (const af::ExclusionEntry& entry : prepared_one.value().excluded) {
      Report::note("excluded " + entry.candidate.to_string() + " at " +
                   std::string(af::selection_stage_name(entry.stage)) + ": " +
                   std::string(af::exclusion_reason_name(entry.reason)) + " (" + entry.detail +
                   ")");
    }
    Report::note("set AF_REFERENCE_CXX to a C++20 compiler to exercise population advance");
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

  const af::Result<af::SelectionDecision> committed_one =
      core->commit_selection(prepared_one.value());
  if (!report.expect_status(committed_one.status(), "P1 commit_selection")) {
    return 1;
  }
  const af::CandidateId winner = committed_one.value().selected;
  Report::item("P1 winner=" + winner.to_string() +
               " ranking=" + std::to_string(committed_one.value().ranking.size()) +
               " excluded=" + std::to_string(committed_one.value().excluded.size()));

  const af::Result<af::RetentionDecision> retention_one = core->prepare_retention(p1);
  if (!report.expect_status(retention_one.status(), "P1 prepare_retention")) {
    return 1;
  }
  const af::Result<af::RetentionDecision> committed_retention_one =
      core->commit_retention(retention_one.value());
  if (!report.expect_status(committed_retention_one.status(), "P1 commit_retention")) {
    return 1;
  }
  Report::item("P1 retained=" + std::to_string(committed_retention_one.value().retained.size()) +
               " retired=" + std::to_string(committed_retention_one.value().retired.size()));

  Report::section("advance to generation 2");
  const af::Result<af::PopulationId> advanced = core->advance_population(p1, "P2-lineage");
  if (!report.expect_status(advanced.status(), "advance_population")) {
    return 1;
  }
  const af::PopulationId p2 = advanced.value();
  const af::PopulationRecord predecessor = core->population(p1).value();
  const af::PopulationRecord successor = core->population(p2).value();
  Report::item("P1 state=" + std::string(af::population_state_name(predecessor.state)) +
               " generation=" + predecessor.generation.to_string());
  Report::item("P2 index=" + std::to_string(successor.population_index) +
               " predecessor=" + successor.predecessor.to_string() +
               " seed_candidate=" + successor.seed_candidate.to_string() +
               " seed_lineage=" + successor.seed_lineage.to_string());
  report.expect(predecessor.state == af::PopulationState::Closed,
                "the predecessor population is Closed once it has been advanced");
  report.expect(successor.seed_candidate == winner,
                "the successor is seeded by the predecessor's committed winner");
  report.expect(successor.seed_lineage.valid(),
                "the successor carries the winner's lineage identity");

  if (!report.expect_status(
          core->start_population(p2, core->population(p2).value().generation), "P2 started")) {
    return 1;
  }

  std::vector<Production> generation_two;
  for (std::size_t index = 0; index < 2; ++index) {
    const std::string label = "P2-slot-" + std::to_string(index + 1);
    const WorkerSession session = open_session(report, *core, context.allocator, label);
    if (!session.worker.valid()) {
      return 1;
    }
    const std::string source = index == 0
                                   ? af::generate_reference_solution(af::ReferenceStrategy::Iterative)
                                   : af::generate_reference_solution(
                                         af::ReferenceStrategy::ClosedForm);
    const std::string strategy = index == 0 ? "reference-iterative" : "reference-closed-form";
    Production production;
    if (!produce(report, *core, session.authority, label, strategy, source, production)) {
      return 1;
    }
    generation_two.push_back(production);
  }
  for (std::size_t index = 0; index < generation_two.size(); ++index) {
    if (!publish(report, *core, generation_two[index], "P2-slot-" + std::to_string(index + 1))) {
      return 1;
    }
  }

  std::vector<af::CandidateId> candidates_two;
  for (const Production& production : generation_two) {
    candidates_two.push_back(production.candidate);
  }
  if (!evaluate_candidates(report, *core, task, p2, context.allocator, scratch.value(), probe,
                           generation_two, evaluator_ids)) {
    return 1;
  }

  Report::section("generation 2 candidates");
  for (const af::CandidateId candidate_id : candidates_two) {
    const af::CandidateRecord candidate = core->candidate(candidate_id).value();
    Report::item("candidate " + candidate_id.to_string() +
                 " state=" + std::string(af::candidate_state_name(candidate.state)) +
                 " depth=" + std::to_string(candidate.depth) +
                 " parents=" + join_ids(candidate.parents) +
                 " lineage=" + candidate.lineage.to_string());
    report.expect(candidate.parents.size() == 1 && candidate.parents.front() == winner,
                  candidate.id.to_string() + " descends from the generation 1 winner");
    report.expect(candidate.depth == 1,
                  candidate.id.to_string() + " has lineage depth 1, one below the winner");
  }

  const af::FoundrySnapshot snapshot = core->snapshot();
  print_lineage(snapshot);
  report.expect(snapshot.lineage.descendants_of(winner).size() == candidates_two.size(),
                "the winner has exactly the two generation 2 candidates as descendants");
  report.expect(snapshot.lineage.ancestors_of(candidates_two.front()).size() == 1,
                "a generation 2 candidate has exactly one ancestor");
  report.expect(snapshot.lineage.path_to_root(candidates_two.front()).size() == 2,
                "the root-to-node path of a generation 2 candidate names the winner and itself");

  Report::section("replay of the generation 1 publication authority");
  const af::CandidateRecord winner_after = core->candidate(winner).value();
  af::ArtifactRef artifact;
  artifact.name = std::string(af::kReferenceTaskSourceArtifact);
  artifact.size_bytes = static_cast<std::uint64_t>(generation_one.front().source.size());
  artifact.content_digest = af::sha256_hex(generation_one.front().source);
  std::vector<af::ArtifactRef> artifacts;
  artifacts.push_back(artifact);

  const af::Result<af::PublicationOutcome> replayed = core->publish_candidate(
      generation_one.front().authority, artifacts, "replayed-generation-one-strategy");
  report.expect(!replayed.ok(), "the replayed operation is rejected");
  report.expect(af::is_stale_authority(replayed.status().code()),
                "the rejection is a stale-authority rejection");
  Report::item("replay code=" +
               std::string(af::error_code_name(replayed.status().code())));
  Report::note("replay message: " + replayed.status().message());

  const af::Status replayed_ack = core->acknowledge_attempt(generation_one.front().authority);
  report.expect(!replayed_ack.ok() && af::is_stale_authority(replayed_ack.code()),
                "acknowledging with the same obsolete authority is also refused as stale");
  Report::note("acknowledge code=" + std::string(af::error_code_name(replayed_ack.code())));

  const af::CandidateRecord winner_now = core->candidate(winner).value();
  report.expect(winner_now.state == winner_after.state && winner_now.selected,
                "the refused replay mutated nothing: the winner is still the winner");

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
