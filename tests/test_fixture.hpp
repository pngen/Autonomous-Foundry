#pragma once

// Shared fixtures for the suites that drive FoundryCore directly.
//
// Every helper here builds its own state: nothing is shared between cases, so a
// case can fail without changing what any other case observes. The helpers are
// inline so that any number of translation units may use them.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/attempt.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/task.hpp"
#include "test_support.hpp"

// The expectations and the fixtures below are written in the runtime's own
// vocabulary, so the domain enums are reachable at the point a case body is
// expanded.
using namespace autonomous_foundry;  // NOLINT(google-build-using-namespace)

namespace af_test {

/// A foundry with one task, one policy and one running population.
struct FoundryFixture {
  explicit FoundryFixture(const FoundryConfig& config) : core(config) {}

  FoundryCore core;
  TaskId task;
  PolicyId policy;
  PopulationId population;
  TaskGeneration task_generation;
  PolicyGeneration policy_generation;
  PopulationGeneration population_generation;
};

/// Configuration with a fresh run identity and a bounded budget.
[[nodiscard]] inline FoundryConfig make_config(std::uint32_t salt) {
  FoundryConfig config;
  config.foundry = FoundryId::from_raw((static_cast<std::uint64_t>(IdKind::Foundry) << 56) |
                                       (static_cast<std::uint64_t>(salt) << 32) | 1ull);
  config.run = FoundryRunId::from_raw((static_cast<std::uint64_t>(IdKind::FoundryRun) << 56) |
                                      (static_cast<std::uint64_t>(salt) << 32) | 1ull);
  config.epoch = CoordinatorEpoch::from_value(1);
  config.id_salt = salt;
  config.default_budgets = BudgetLimits{};
  return config;
}

/// Deterministic but distinct worker identity for one slot.
[[nodiscard]] inline WorkerId worker_identity(std::uint32_t slot) {
  return WorkerId::from_raw((static_cast<std::uint64_t>(IdKind::Worker) << 56) | 0x0000C1ull << 32 |
                            static_cast<std::uint64_t>(slot) + 1ull);
}

/// Deterministic but distinct worker boot identity for one slot.
[[nodiscard]] inline WorkerBootId boot_identity(std::uint32_t slot) {
  return WorkerBootId::from_raw((static_cast<std::uint64_t>(IdKind::WorkerBoot) << 56) |
                                (static_cast<std::uint64_t>(slot) + 1ull) * 0x0102030405ull);
}

/// A task contract with two mandatory gates and two optional factors. The
/// evaluator keys are the ones the reference evaluator registry provides, so
/// the same contract can be used with real evaluators.
[[nodiscard]] inline TaskSpec make_task(TaskId id, PolicyId policy_id,
                                        std::uint32_t candidate_budget) {
  TaskSpec task;
  task.id = id;
  task.generation = TaskGeneration::first();
  task.name = "reference-task";
  task.objective = "produce a translation unit implementing the reference API";
  task.required_outputs = {std::string(kReferenceTaskSourceArtifact)};
  task.policy = policy_id;
  task.policy_generation = PolicyGeneration::first();
  task.budgets = BudgetLimits{};
  task.budgets.max_candidates = candidate_budget;

  EvaluationRequirement compile_and_run;
  compile_and_run.requirement_class = RequirementClass::Mandatory;
  compile_and_run.evaluator_key = std::string(kEvaluatorCompileAndRun);
  compile_and_run.description = "compiles against the fixed harness and exits zero";
  task.requirements.push_back(compile_and_run);

  EvaluationRequirement source_policy;
  source_policy.requirement_class = RequirementClass::Mandatory;
  source_policy.evaluator_key = std::string(kEvaluatorSourcePolicy);
  source_policy.description = "uses no construct the task forbids";
  task.requirements.push_back(source_policy);

  EvaluationRequirement performance;
  performance.requirement_class = RequirementClass::Optional;
  performance.evaluator_key = std::string(kEvaluatorPerformance);
  performance.description = "reference throughput of the bounded workload";
  performance.ranking_weight = 1.0;
  task.requirements.push_back(performance);

  EvaluationRequirement parsimony;
  parsimony.requirement_class = RequirementClass::Optional;
  parsimony.evaluator_key = std::string(kEvaluatorParsimony);
  parsimony.description = "reference token-count parsimony metric";
  parsimony.ranking_weight = 1.0;
  task.requirements.push_back(parsimony);

  task.hard_constraints = {"no external dependencies"};
  task.closure_criteria = {"every mandatory gate satisfies"};
  return task;
}

/// The matching policy: complete mandatory gates required, bounded retention.
[[nodiscard]] inline FoundryPolicy make_policy(PolicyId id) {
  FoundryPolicy policy;
  policy.id = id;
  policy.generation = PolicyGeneration::first();
  policy.name = "reference-policy";

  RankingFactor quality;
  quality.kind = RankingFactorKind::QualityScore;
  quality.direction = RankingDirection::HigherIsBetter;
  quality.weight = 3.0;
  quality.key = "quality";
  policy.selection.factors.push_back(quality);

  RankingFactor completeness;
  completeness.kind = RankingFactorKind::EvaluationCompleteness;
  completeness.direction = RankingDirection::HigherIsBetter;
  completeness.weight = 1.0;
  completeness.key = "completeness";
  policy.selection.factors.push_back(completeness);

  RankingFactor depth;
  depth.kind = RankingFactorKind::LineageDepth;
  depth.direction = RankingDirection::LowerIsBetter;
  depth.weight = 0.5;
  depth.key = "depth";
  policy.selection.factors.push_back(depth);

  RankingFactor tie_break;
  tie_break.kind = RankingFactorKind::CandidateIdTieBreak;
  tie_break.direction = RankingDirection::LowerIsBetter;
  tie_break.weight = 1.0;
  tie_break.key = "candidate_id";
  policy.selection.factors.push_back(tie_break);

  policy.selection.require_complete_mandatory = true;

  policy.retention.retain_top_k = 3;
  policy.retention.max_retained = 8;
  policy.retention.max_per_lineage = 2;
  policy.retention.retain_selected = true;

  policy.budgets = BudgetLimits{};
  policy.budgets.max_candidates = 1024;
  policy.budgets.max_workers = 16;
  policy.max_generation_depth = 16;
  policy.carry_forward_elite = false;
  return policy;
}

/// Build the fixture and start the population.
inline void build_running_fixture(const TestContext& context, FoundryFixture& fixture,
                                  std::uint32_t candidate_budget) {
  TaskSpec task = make_task(TaskId(), PolicyId(), candidate_budget);
  FoundryPolicy policy = make_policy(PolicyId());

  REQUIRE_VALUE(context, PolicyId, policy_id, fixture.core.define_policy(policy));
  fixture.policy = policy_id;
  fixture.policy_generation = PolicyGeneration::first();

  task.policy = policy_id;
  task.policy_generation = fixture.policy_generation;
  REQUIRE_VALUE(context, TaskId, task_id, fixture.core.define_task(task));
  fixture.task = task_id;
  fixture.task_generation = TaskGeneration::first();

  PopulationSpec spec;
  spec.name = "P1";
  spec.task = fixture.task;
  spec.task_generation = fixture.task_generation;
  spec.policy = fixture.policy;
  spec.policy_generation = fixture.policy_generation;
  spec.candidate_budget = candidate_budget;
  spec.worker_budget = 4;
  spec.population_index = 1;
  REQUIRE_VALUE(context, PopulationId, population_id, fixture.core.create_population(spec));
  fixture.population = population_id;
  fixture.population_generation = PopulationGeneration::first();

  REQUIRE_OK(context, fixture.core.start_population(fixture.population,
                                                    fixture.population_generation));
  fixture.population_generation = PopulationGeneration::from_value(2);
}

/// Register a worker incarnation and drive it to Ready.
[[nodiscard]] inline WorkerSessionAuthority connect_worker(const TestContext& context,
                                                           FoundryCore& core, std::uint32_t slot) {
  WorkerRegistrationRequest request;
  request.worker = worker_identity(slot);
  request.boot = boot_identity(slot);
  request.label = "worker";
  request.capability = "reference";
  request.process_id = 1000u + slot;
  const WorkerSessionAuthority session =
      require_value(context, core.register_worker(request), "register_worker", __FILE__, __LINE__);
  const Status ready = core.worker_ready(session, "reference");
  if (!ready.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("worker_ready failed: ") +
                        std::string(error_code_name(ready.code())) + " '" + ready.message() + "'");
  }
  return session;
}

/// Drive one full attempt for the given worker: authorize, confirm dispatch,
/// acknowledge, publish. Returns the candidate identity the attempt produced.
[[nodiscard]] inline CandidateId produce_candidate(const TestContext& context, FoundryCore& core,
                                                   const WorkerSessionAuthority& session,
                                                   std::string_view solution_source,
                                                   std::string_view declared_strategy) {
  const PendingDispatch pending =
      require_value(context, core.authorize_attempt(session), "authorize_attempt", __FILE__,
                    __LINE__);
  const AttemptPackage package =
      require_value(context,
                    core.confirm_dispatch(session, pending.attempt.id, pending.attempt.generation),
                    "confirm_dispatch", __FILE__, __LINE__);

  WorkerOperationAuthority authority;
  authority.session = session;
  authority.population = package.population;
  authority.population_generation = package.population_generation;
  authority.task = package.task;
  authority.task_generation = package.task_generation;
  authority.candidate = package.candidate;
  authority.candidate_generation = package.candidate_generation;
  authority.attempt = package.attempt;
  authority.attempt_generation = package.attempt_generation;
  authority.assignment = package.assignment;

  const Status acknowledged = core.acknowledge_attempt(authority);
  if (!acknowledged.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("acknowledge_attempt failed: ") +
                        std::string(error_code_name(acknowledged.code())) + " '" +
                        acknowledged.message() + "'");
  }

  std::vector<ArtifactRef> artifacts;
  ArtifactRef solution;
  solution.name = std::string(kReferenceTaskSourceArtifact);
  solution.size_bytes = static_cast<std::uint64_t>(solution_source.size());
  solution.content_digest = sha256_hex(solution_source);
  artifacts.push_back(solution);

  const PublicationOutcome outcome = require_value(
      context, core.publish_candidate(authority, artifacts, std::string(declared_strategy)),
      "publish_candidate", __FILE__, __LINE__);
  return outcome.candidate;
}

/// Record one complete evaluation record for a candidate. The record is bound
/// to the candidate's current generation and to the population's current task
/// and population generations.
inline void record_evidence(const TestContext& context, FoundryCore& core,
                            const CandidateRecord& candidate, std::string_view key,
                            EvaluationOutcome outcome, EvaluatorKind kind,
                            RequirementClass requirement_class, bool has_score, double score) {
  const PopulationRecord population =
      require_value(context, core.population(candidate.population), "population", __FILE__,
                    __LINE__);
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
  record.evidence_digest =
      "9f2c4c0c1f5a3d4e5b6c7d8e9f00112233445566778899aabbccddeeff001122";
  const Status recorded = core.record_evaluation(record);
  if (!recorded.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("record_evaluation('") + std::string(key) + "') failed: " +
                        std::string(error_code_name(recorded.code())) + " '" + recorded.message() +
                        "'");
  }
}

/// The reference candidate source for one strategy ordinal, as bytes.
[[nodiscard]] inline std::string reference_solution_source(int strategy_ordinal) {
  return generate_reference_solution(static_cast<ReferenceStrategy>(strategy_ordinal));
}

/// A snapshot of one population's budget ledger, flattened so a case can make
/// plain numeric assertions without holding a reference into core state.
struct LedgerSnapshot {
  std::uint64_t open_reservations{0};
  std::uint64_t consumed{0};
  std::uint64_t reserved{0};
  std::uint64_t released{0};
};

/// Load a candidate record by identity, failing the case when it is missing.
[[nodiscard]] inline CandidateRecord load_candidate(const TestContext& context, FoundryCore& core,
                                                    CandidateId id) {
  return require_value(context, core.candidate(id), "candidate", __FILE__, __LINE__);
}

/// Record a complete evaluation record set for one candidate: both mandatory
/// gates plus both optional ranking factors.
///
/// The mandatory compile-and-run gate outcome is a parameter so a case can
/// manufacture a hard-gate failure, and the evaluator kind is a parameter so a
/// case can manufacture a worker self report that must never satisfy a gate.
inline void record_full_evidence(const TestContext& context, FoundryCore& core,
                                 const CandidateRecord& candidate,
                                 EvaluationOutcome compile_and_run_outcome,
                                 EvaluatorKind compile_and_run_kind, double optional_score) {
  record_evidence(context, core, candidate, kEvaluatorCompileAndRun, compile_and_run_outcome,
                  compile_and_run_kind, RequirementClass::Mandatory, false, 0.0);
  record_evidence(context, core, candidate, kEvaluatorSourcePolicy, EvaluationOutcome::Pass,
                  EvaluatorKind::SourcePolicy, RequirementClass::Mandatory, false, 0.0);
  record_evidence(context, core, candidate, kEvaluatorPerformance, EvaluationOutcome::Pass,
                  EvaluatorKind::ProcessCommand, RequirementClass::Optional, true, optional_score);
  record_evidence(context, core, candidate, kEvaluatorParsimony, EvaluationOutcome::Pass,
                  EvaluatorKind::PortableReference, RequirementClass::Optional, true,
                  optional_score);
}

/// Every operand a single full candidate cycle needs.
struct FullEvidence {
  EvaluationOutcome compile_and_run_outcome{EvaluationOutcome::Pass};
  EvaluatorKind compile_and_run_kind{EvaluatorKind::ProcessCommand};
  double optional_score{1.0};
};

/// Drive one complete candidate cycle inside a population and leave the
/// population ready for the next one.
///
/// The cycle is the one the core actually accepts:
///   authorize -> confirm_dispatch -> acknowledge -> publish
///   -> begin_evaluation        (Published -> Evaluating is the declared edge)
///   -> record a complete record for every declared requirement
///                            (Evaluating -> Evaluated once all are complete)
///   -> request_population_revalidation -> revalidate_population
///                            (Evaluation -> Running so the next attempt dispatches)
///
/// Two facts make the explicit begin_evaluation mandatory rather than
/// cosmetic. First, settle_candidate_locked only settles a candidate that is
/// already Published or Evaluating, and the Published -> Evaluated edge is not
/// legal, so evidence recorded without first entering Evaluating leaves the
/// candidate Published forever. Second, publishing moves the population to
/// Evaluating and authorize_attempt only dispatches from Running, so the
/// population must be revalidated back to Running before the next attempt.
[[nodiscard]] inline CandidateId drive_full_candidate(const TestContext& context,
                                                      FoundryFixture& fixture, std::uint32_t slot,
                                                      const FullEvidence& evidence) {
  const WorkerSessionAuthority session = connect_worker(context, fixture.core, slot);
  const CandidateId produced =
      produce_candidate(context, fixture.core, session,
                        reference_solution_source(static_cast<int>(slot - 1)),
                        "strategy-" + std::to_string(slot));

  const CandidateRecord published = load_candidate(context, fixture.core, produced);
  const Status evaluating = fixture.core.begin_evaluation(produced);
  if (!evaluating.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("begin_evaluation failed for ") + produced.to_string() + ": " +
                        std::string(error_code_name(evaluating.code())) + " '" +
                        evaluating.message() + "'");
  }
  record_full_evidence(context, fixture.core, published, evidence.compile_and_run_outcome,
                       evidence.compile_and_run_kind, evidence.optional_score);
  return produced;
}

/// Return the population to Running so the next attempt can be authorized.
/// The generation advances, so the current one is read back first.
inline void reopen_population_for_dispatch(const TestContext& context, FoundryFixture& fixture) {
  const Status requested = fixture.core.request_population_revalidation(
      fixture.population, "next candidate slot in this population");
  if (!requested.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("request_population_revalidation failed: ") +
                        std::string(error_code_name(requested.code())) + " '" +
                        requested.message() + "'");
  }
  const PopulationRecord current =
      require_value(context, fixture.core.population(fixture.population), "population", __FILE__,
                    __LINE__);
  const Status revalidated = fixture.core.revalidate_population(
      fixture.population, current.generation, "reopened for the next candidate");
  if (!revalidated.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("revalidate_population failed: ") +
                        std::string(error_code_name(revalidated.code())) + " '" +
                        revalidated.message() + "'");
  }
  fixture.population_generation = PopulationGeneration::from_value(current.generation.value() + 1);
}

/// Drive a whole population of candidates, one full cycle each, leaving every
/// candidate in Evaluated and the population in Running.
[[nodiscard]] inline std::vector<CandidateId> populate_population(const TestContext& context,
                                                                 FoundryFixture& fixture,
                                                                 std::uint32_t count,
                                                                 const FullEvidence& evidence) {
  std::vector<CandidateId> produced;
  produced.reserve(count);
  for (std::uint32_t slot = 1; slot <= count; ++slot) {
    produced.push_back(drive_full_candidate(context, fixture, slot, evidence));
    if (slot < count) {
      reopen_population_for_dispatch(context, fixture);
    }
  }
  return produced;
}

/// Read the ledger of a population from a full core snapshot.
[[nodiscard]] inline LedgerSnapshot ledger_snapshot(const TestContext& context, FoundryCore& core,
                                                    PopulationId population) {
  const FoundrySnapshot snapshot = core.snapshot();
  LedgerSnapshot flat;
  const auto found = snapshot.ledgers.find(population);
  if (found == snapshot.ledgers.end()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("population ") + population.to_string() +
                        " has no budget ledger in the snapshot");
  }
  flat.open_reservations = found->second.open_reservations();
  for (const BudgetLedger::Counter& counter : found->second.counters()) {
    flat.consumed += counter.consumed;
    flat.reserved += counter.reserved;
    flat.released += counter.released;
  }
  return flat;
}


}  // namespace af_test