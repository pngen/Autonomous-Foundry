// benchmarks/af_benchmarks.cpp
//
// Autonomous Foundry benchmarks at 10, 100, 1000 and 10000 candidates.
//
// Command line:
//
//   af_benchmarks [--sizes 10,100,1000,10000] [--repeat n] [--only name]
//
// Benchmark names:
//
//   register_candidates  ingest_evaluations   select_candidates
//   retention_selection  lineage_traversal    snapshot_serialize
//   snapshot_deserialize recovery
//
// Every benchmark measures COMPLETED work. A run builds a real population
// through the real state machine and asserts a result-integrity property before
// its numbers are reported: a selected winner must satisfy every mandatory
// gate, a retained set must be consistent with the committed ranking, a
// round-tripped snapshot must equal the state it was written from, and a
// recovered core must carry the same durable history. An integrity failure is
// printed and turns the process exit code non-zero.
//
// The mandatory gate of the task contract used here is the runtime's own
// deterministic static rule over the candidate source, so a run needs no C++
// toolchain and measures the runtime's own bookkeeping rather than compiler
// latency. Candidate slots are dispatched to one worker incarnation each, which
// keeps the population in its Running state for the whole production phase: a
// population that has committed a publication is Evaluating and refuses new
// production work until that output has been evaluated.
//
// setup is excluded from every reported duration and is repeated per run.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/version.hpp"
#include "autonomous_foundry/worker.hpp"
#include "autonomous_foundry/workspace.hpp"

#include "bench_support.hpp"

namespace af = autonomous_foundry;

namespace {

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}

  [[nodiscard]] double ms() const {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> delta = now - start_;
    return delta.count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// The scenario every benchmark builds through the real state machine
// ---------------------------------------------------------------------------

struct Scenario {
  af::IdAllocator allocator;
  std::unique_ptr<af::FoundryCore> core;
  af::PolicyId policy_id;
  af::PolicyGeneration policy_generation;
  af::TaskId task_id;
  af::TaskGeneration task_generation;
  af::PopulationId population;
  std::vector<af::WorkerSessionAuthority> sessions;
  std::vector<af::CandidateId> candidates;
  std::vector<af::WorkerOperationAuthority> authorities;
  std::vector<af::EvaluatorId> evaluator_ids;
  std::size_t factor_count{0};
  std::size_t requirement_count{0};
  std::string passing_source;
  std::string failing_source;
  std::string error;
};

af::BudgetLimits budgets_for(std::uint64_t size) {
  af::BudgetLimits budgets;
  const std::uint32_t bounded = static_cast<std::uint32_t>(size + 8u);
  budgets.max_candidate_attempts = bounded;
  budgets.max_worker_assignments = bounded;
  budgets.max_active_attempts = bounded;
  budgets.max_candidates = bounded;
  budgets.max_workers = bounded;
  budgets.max_evaluation_attempts = static_cast<std::uint32_t>((2u * size) + 8u);
  budgets.max_retries = 8;
  budgets.max_retained_candidates = 64;
  budgets.max_evaluation_concurrency = 8;
  return budgets;
}

/// A task contract whose mandatory gate is decidable in process: the runtime's
/// own deterministic static rule over the candidate source, plus its reference
/// structural metric as an optional factor.
af::Result<af::TaskSpec> make_bench_task(af::TaskId id, af::PolicyId policy,
                                         af::PolicyGeneration policy_generation,
                                         const af::BudgetLimits& budgets) {
  af::TaskSpec task;
  task.id = id;
  task.generation = af::TaskGeneration::first();
  task.name = "benchmark-static-gate";
  task.objective =
      "Produce solution.cpp. The mandatory gate is a deterministic static rule over the "
      "candidate source, so the benchmark measures runtime bookkeeping, not compiler latency.";

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
  task.closure_criteria.push_back("a selection decision is committed for the population");
  task.policy = policy;
  task.policy_generation = policy_generation;
  task.budgets = budgets;

  const af::Result<std::string> digest = af::compute_task_digest(task);
  if (!digest.ok()) {
    return digest.status();
  }
  task.content_digest = digest.value();
  AF_TRY(af::validate_task(task));
  return task;
}

std::string forbidden_token_source() {
  std::string source = af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);
  source.append("\n// The static rule is a textual scan, and this comment contains the token "
                "system( that it forbids, so this candidate fails the mandatory gate.\n");
  return source;
}

/// Build the foundry, the policy, the task, the population and one worker
/// incarnation per candidate. No candidate exists yet.
bool build_empty(std::uint64_t size, Scenario& scenario) {
  scenario.allocator = af::IdAllocator(af::make_id_salt());

  af::FoundryConfig config;
  const af::Result<af::FoundryId> foundry = scenario.allocator.next<af::FoundryIdTag>();
  if (!foundry.ok()) {
    scenario.error = foundry.status().to_string();
    return false;
  }
  config.foundry = foundry.value();
  config.run = af::make_foundry_run_id();
  config.epoch = af::CoordinatorEpoch::from_value(1);
  config.id_salt = scenario.allocator.salt();
  scenario.core = std::make_unique<af::FoundryCore>(config);

  const af::BudgetLimits budgets = budgets_for(size);

  const af::Result<af::PolicyId> policy_id = scenario.allocator.next<af::PolicyIdTag>();
  if (!policy_id.ok()) {
    scenario.error = policy_id.status().to_string();
    return false;
  }
  af::FoundryPolicy policy =
      af::make_reference_policy(policy_id.value(), af::PolicyGeneration::first());
  policy.budgets = budgets;
  const af::Result<af::PolicyId> defined = scenario.core->define_policy(policy);
  if (!defined.ok()) {
    scenario.error = defined.status().to_string();
    return false;
  }
  scenario.policy_id = defined.value();
  scenario.policy_generation = scenario.core->policy(scenario.policy_id).value().generation;
  scenario.factor_count = scenario.core->policy(scenario.policy_id).value().selection.factors.size();

  const af::Result<af::TaskId> task_id = scenario.allocator.next<af::TaskIdTag>();
  if (!task_id.ok()) {
    scenario.error = task_id.status().to_string();
    return false;
  }
  const af::Result<af::TaskSpec> spec = make_bench_task(
      task_id.value(), scenario.policy_id, scenario.policy_generation, budgets);
  if (!spec.ok()) {
    scenario.error = spec.status().to_string();
    return false;
  }
  const af::Result<af::TaskId> defined_task = scenario.core->define_task(spec.value());
  if (!defined_task.ok()) {
    scenario.error = defined_task.status().to_string();
    return false;
  }
  scenario.task_id = defined_task.value();
  scenario.task_generation = scenario.core->task(scenario.task_id).value().generation;
  scenario.requirement_count = scenario.core->task(scenario.task_id).value().requirements.size();

  af::PopulationSpec population_spec;
  population_spec.name = "benchmark-population";
  population_spec.task = scenario.task_id;
  population_spec.task_generation = scenario.task_generation;
  population_spec.policy = scenario.policy_id;
  population_spec.policy_generation = scenario.policy_generation;
  population_spec.candidate_budget = static_cast<std::uint32_t>(size);
  population_spec.worker_budget = static_cast<std::uint32_t>(size);
  population_spec.population_index = 1;
  const af::Result<af::PopulationId> created =
      scenario.core->create_population(population_spec);
  if (!created.ok()) {
    scenario.error = created.status().to_string();
    return false;
  }
  scenario.population = created.value();
  const af::Status started = scenario.core->start_population(
      scenario.population, scenario.core->population(scenario.population).value().generation);
  if (!started.ok()) {
    scenario.error = started.to_string();
    return false;
  }

  for (std::uint64_t index = 0; index < size; ++index) {
    const af::Result<af::WorkerId> worker = scenario.allocator.next<af::WorkerIdTag>();
    if (!worker.ok()) {
      scenario.error = worker.status().to_string();
      return false;
    }
    af::WorkerRegistrationRequest request;
    request.worker = worker.value();
    request.boot = af::make_worker_boot_id();
    request.label = "bench-worker-" + std::to_string(index);
    request.capability = "reference-static";
    request.process_id = 0;
    const af::Result<af::WorkerSessionAuthority> session =
        scenario.core->register_worker(request);
    if (!session.ok()) {
      scenario.error = session.status().to_string();
      return false;
    }
    const af::Status ready = scenario.core->worker_ready(session.value(), "reference-static");
    if (!ready.ok()) {
      scenario.error = ready.to_string();
      return false;
    }
    scenario.sessions.push_back(session.value());
  }

  for (std::size_t index = 0; index < scenario.requirement_count; ++index) {
    const af::Result<af::EvaluatorId> evaluator = scenario.allocator.next<af::EvaluatorIdTag>();
    if (!evaluator.ok()) {
      scenario.error = evaluator.status().to_string();
      return false;
    }
    scenario.evaluator_ids.push_back(evaluator.value());
  }

  scenario.passing_source = af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);
  scenario.failing_source = forbidden_token_source();
  return true;
}

/// Register and dispatch one candidate slot on one worker incarnation.
bool dispatch_slot(Scenario& scenario, std::size_t index, bool complete_dispatch) {
  const af::Result<af::PendingDispatch> pending =
      scenario.core->authorize_attempt(scenario.sessions[index]);
  if (!pending.ok()) {
    scenario.error = pending.status().to_string();
    return false;
  }
  if (pending.value().attempt.population != scenario.population) {
    scenario.error = "an assignment went to a population this benchmark did not create";
    return false;
  }
  af::WorkerOperationAuthority authority;
  authority.session = pending.value().session;
  authority.population = pending.value().attempt.population;
  authority.population_generation = pending.value().attempt.population_generation;
  authority.task = pending.value().attempt.task;
  authority.task_generation = pending.value().attempt.task_generation;
  authority.candidate = pending.value().attempt.candidate;
  authority.candidate_generation = pending.value().attempt.candidate_generation;
  authority.attempt = pending.value().attempt.id;
  authority.attempt_generation = pending.value().attempt.generation;
  authority.assignment = pending.value().attempt.assignment;
  scenario.candidates.push_back(authority.candidate);
  scenario.authorities.push_back(authority);
  if (complete_dispatch) {
    const af::Status dispatched =
        scenario.core
            ->confirm_dispatch(scenario.sessions[index], pending.value().attempt.id,
                               pending.value().attempt.generation)
            .status();
    if (!dispatched.ok()) {
      scenario.error = dispatched.to_string();
      return false;
    }
  }
  return true;
}

/// Every candidate published and every declared requirement recorded.
bool publish_slot(Scenario& scenario, std::size_t index) {
  const bool passes = (index % 2u) == 0u;
  const std::string& source = passes ? scenario.passing_source : scenario.failing_source;
  af::ArtifactRef artifact;
  artifact.name = std::string(af::kReferenceTaskSourceArtifact);
  artifact.size_bytes = static_cast<std::uint64_t>(source.size());
  artifact.content_digest = af::sha256_hex(source);
  std::vector<af::ArtifactRef> artifacts;
  artifacts.push_back(artifact);
  const af::Result<af::PublicationOutcome> published = scenario.core->publish_candidate(
      scenario.authorities[index], artifacts,
      passes ? "benchmark-passing" : "benchmark-failing");
  if (!published.ok()) {
    scenario.error = published.status().to_string();
    return false;
  }
  return true;
}

/// Run the declared requirements of one candidate through the real reference
/// evaluators and store the records they produced. Both evaluators are
/// in-process, so no compiler is involved.
bool ingest_slot(Scenario& scenario, std::size_t index, const af::EvaluatorRegistry& registry) {
  const af::TaskSpec task = scenario.core->task(scenario.task_id).value();
  const af::CandidateRecord candidate = scenario.core->candidate(scenario.candidates[index]).value();
  const std::string& source =
      (index % 2u) == 0u ? scenario.passing_source : scenario.failing_source;

  const af::Status evaluating = scenario.core->begin_evaluation(candidate.id);
  if (!evaluating.ok()) {
    scenario.error = evaluating.to_string();
    return false;
  }

  std::size_t requirement_index = 0;
  for (const af::EvaluationRequirement& requirement : task.requirements) {
    const std::shared_ptr<af::Evaluator> evaluator = registry.find(requirement.evaluator_key);
    if (evaluator == nullptr) {
      scenario.error = "the reference registry does not expose '" + requirement.evaluator_key + "'";
      return false;
    }
    af::EvaluationRequest request;
    request.candidate = candidate.id;
    request.candidate_generation = candidate.generation;
    request.task = task.id;
    request.task_generation = task.generation;
    request.population = scenario.population;
    request.population_generation =
        scenario.core->population(scenario.population).value().generation;
    request.artifacts.emplace(std::string(af::kReferenceTaskSourceArtifact), source);
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
    record.population = scenario.population;
    record.population_generation =
        scenario.core->population(scenario.population).value().generation;
    record.evaluator = scenario.evaluator_ids[requirement_index];
    record.evaluator_key = requirement.evaluator_key;
    record.kind = evaluator->kind();
    record.requirement_class = requirement.requirement_class;
    record.outcome = result.outcome;
    record.complete = true;
    record.has_score = result.has_score;
    record.score = result.score;
    record.decided_epoch = scenario.core->epoch();
    record.diagnostics = result.diagnostics;
    record.evidence_digest = result.evidence_digest;
    record.duration_micros = result.duration_micros;
    const af::Status recorded = scenario.core->record_evaluation(record);
    if (!recorded.ok()) {
      scenario.error = recorded.to_string();
      return false;
    }
    ++requirement_index;
  }
  return true;
}

/// Register and dispatch every candidate slot in the population.
bool build_registered(std::uint64_t size, Scenario& scenario, bool complete_dispatch) {
  if (!build_empty(size, scenario)) {
    return false;
  }
  for (std::size_t index = 0; index < static_cast<std::size_t>(size); ++index) {
    if (!dispatch_slot(scenario, index, complete_dispatch)) {
      return false;
    }
  }
  return true;
}

/// Register, dispatch, publish and evaluate every candidate.
bool build_evaluated(std::uint64_t size, Scenario& scenario, double* ingestion_ms,
                     std::uint64_t* ingestion_operations) {
  if (!build_empty(size, scenario)) {
    return false;
  }
  for (std::size_t index = 0; index < static_cast<std::size_t>(size); ++index) {
    if (!dispatch_slot(scenario, index, true)) {
      return false;
    }
  }
  for (std::size_t index = 0; index < static_cast<std::size_t>(size); ++index) {
    if (!publish_slot(scenario, index)) {
      return false;
    }
  }

  const af::EvaluatorRegistry registry = af::EvaluatorRegistry::make_reference();
  Timer timer;
  const bool measured = ingestion_ms != nullptr;
  double elapsed = 0.0;
  for (std::size_t index = 0; index < static_cast<std::size_t>(size); ++index) {
    if (!ingest_slot(scenario, index, registry)) {
      return false;
    }
    if (measured && (index % 64u) == 63u) {
      elapsed += timer.ms();
      timer = Timer();
    }
  }
  if (measured) {
    elapsed += timer.ms();
    *ingestion_ms = elapsed;
    *ingestion_operations = size * static_cast<std::uint64_t>(scenario.requirement_count);
  }
  return true;
}

af_bench::CompletedRun failed_run(const std::string& detail) {
  af_bench::CompletedRun run;
  run.integrity_ok = false;
  run.integrity_detail = detail;
  return run;
}

/// Every mandatory requirement of a candidate has a complete authoritative pass.
bool candidate_satisfies_mandatory_gates(af::FoundryCore& core, const af::TaskSpec& task,
                                         af::CandidateId candidate) {
  const std::vector<af::EvaluationRecord> records = core.candidate_evaluations(candidate);
  for (const af::EvaluationRequirement& requirement : task.requirements) {
    if (requirement.requirement_class != af::RequirementClass::Mandatory) {
      continue;
    }
    bool satisfied = false;
    for (const af::EvaluationRecord& record : records) {
      if (record.evaluator_key == requirement.evaluator_key && record.complete &&
          record.outcome == af::EvaluationOutcome::Pass &&
          af::evaluator_kind_is_authoritative(record.kind)) {
        satisfied = true;
        break;
      }
    }
    if (!satisfied) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

af_bench::CompletedRun benchmark_register_candidates(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  if (!build_empty(size, scenario)) {
    return failed_run("setup failed: " + scenario.error);
  }

  Timer timer;
  for (std::size_t index = 0; index < static_cast<std::size_t>(size); ++index) {
    if (!dispatch_slot(scenario, index, true)) {
      return failed_run("registration failed: " + scenario.error);
    }
  }
  const double elapsed = timer.ms();

  af_bench::CompletedRun run;
  run.elapsed_ms = elapsed;
  run.candidates = scenario.candidates.size();
  run.evaluations = 0;
  run.factors = scenario.factor_count;
  run.operations = size;

  const std::vector<af::CandidateId> listed = scenario.core->population_candidates(scenario.population);
  const std::vector<af::LineageNode> nodes = scenario.core->lineage_nodes();
  bool every_candidate_producing = true;
  for (const af::CandidateId candidate_id : scenario.candidates) {
    const af::CandidateRecord candidate = scenario.core->candidate(candidate_id).value();
    if (candidate.state != af::CandidateState::Producing || !candidate.attempt.valid()) {
      every_candidate_producing = false;
      break;
    }
  }
  const std::vector<std::string> violations = scenario.core->audit();
  run.integrity_ok = listed.size() == static_cast<std::size_t>(size) &&
                     scenario.candidates.size() == static_cast<std::size_t>(size) &&
                     nodes.size() == static_cast<std::size_t>(size) && every_candidate_producing &&
                     violations.empty();
  if (!run.integrity_ok) {
    run.integrity_detail = "registration did not leave " + std::to_string(size) +
                           " dispatchable candidate slots (" +
                           std::to_string(violations.size()) + " audit violations)";
  }
  return run;
}

af_bench::CompletedRun benchmark_ingest_evaluations(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  double measured_ms = 0.0;
  std::uint64_t operations = 0;
  if (!build_evaluated(size, scenario, &measured_ms, &operations)) {
    return failed_run("setup or ingestion failed: " + scenario.error);
  }

  af_bench::CompletedRun run;
  run.elapsed_ms = measured_ms;
  run.candidates = scenario.candidates.size();
  run.operations = operations;
  run.factors = scenario.factor_count;

  const af::TaskSpec task = scenario.core->task(scenario.task_id).value();
  std::uint64_t records = 0;
  bool every_candidate_evaluated = true;
  for (const af::CandidateId candidate_id : scenario.candidates) {
    const af::CandidateRecord candidate = scenario.core->candidate(candidate_id).value();
    if (candidate.state != af::CandidateState::Evaluated) {
      every_candidate_evaluated = false;
      break;
    }
    records += scenario.core->candidate_evaluations(candidate_id).size();
  }
  run.evaluations = records;
  const std::vector<std::string> violations = scenario.core->audit();
  run.integrity_ok = every_candidate_evaluated &&
                     records == size * static_cast<std::uint64_t>(task.requirements.size()) &&
                     violations.empty();
  if (!run.integrity_ok) {
    run.integrity_detail = "ingestion did not leave every candidate Evaluated with one record per "
                           "declared requirement";
  }
  return run;
}

af_bench::CompletedRun benchmark_select_candidates(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  if (!build_evaluated(size, scenario, nullptr, nullptr)) {
    return failed_run("setup failed: " + scenario.error);
  }
  const af::TaskSpec task = scenario.core->task(scenario.task_id).value();

  Timer timer;
  const af::Result<af::SelectionDecision> prepared =
      scenario.core->prepare_selection(scenario.population);
  if (!prepared.ok()) {
    return failed_run("prepare_selection failed: " + prepared.status().to_string());
  }
  const af::Result<af::SelectionDecision> committed =
      scenario.core->commit_selection(prepared.value());
  if (!committed.ok()) {
    return failed_run("commit_selection failed: " + committed.status().to_string());
  }
  const double elapsed = timer.ms();

  af_bench::CompletedRun run;
  run.elapsed_ms = elapsed;
  run.candidates = scenario.candidates.size();
  run.evaluations = 0;
  for (const af::CandidateId candidate_id : scenario.candidates) {
    run.evaluations += scenario.core->candidate_evaluations(candidate_id).size();
  }
  run.factors = scenario.factor_count;
  run.operations = committed.value().ranking.size();

  const bool has_winner = committed.value().has_winner();
  const bool winner_gates =
      has_winner && candidate_satisfies_mandatory_gates(*scenario.core, task,
                                                        committed.value().selected);
  const std::size_t expected_ranked = static_cast<std::size_t>((size + 1u) / 2u);
  run.integrity_ok = committed.value().state == af::SelectionDecisionState::Committed &&
                     has_winner && winner_gates &&
                     committed.value().ranking.size() == expected_ranked &&
                     committed.value().excluded.size() == static_cast<std::size_t>(size / 2u);
  if (!run.integrity_ok) {
    run.integrity_detail =
        "the committed selection does not have the expected shape or its winner does not satisfy "
        "every mandatory gate";
  }
  return run;
}

af_bench::CompletedRun benchmark_retention_selection(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  if (!build_evaluated(size, scenario, nullptr, nullptr)) {
    return failed_run("setup failed: " + scenario.error);
  }
  const af::PolicyId policy_id = scenario.policy_id;
  const af::FoundryPolicy policy = scenario.core->policy(policy_id).value();

  const af::Result<af::SelectionDecision> prepared =
      scenario.core->prepare_selection(scenario.population);
  if (!prepared.ok()) {
    return failed_run("prepare_selection failed: " + prepared.status().to_string());
  }
  const af::Result<af::SelectionDecision> committed =
      scenario.core->commit_selection(prepared.value());
  if (!committed.ok()) {
    return failed_run("commit_selection failed: " + committed.status().to_string());
  }

  Timer timer;
  const af::Result<af::RetentionDecision> prepared_retention =
      scenario.core->prepare_retention(scenario.population);
  if (!prepared_retention.ok()) {
    return failed_run("prepare_retention failed: " + prepared_retention.status().to_string());
  }
  const af::Result<af::RetentionDecision> retained =
      scenario.core->commit_retention(prepared_retention.value());
  if (!retained.ok()) {
    return failed_run("commit_retention failed: " + retained.status().to_string());
  }
  const double elapsed = timer.ms();

  af_bench::CompletedRun run;
  run.elapsed_ms = elapsed;
  run.candidates = scenario.candidates.size();
  for (const af::CandidateId candidate_id : scenario.candidates) {
    run.evaluations += scenario.core->candidate_evaluations(candidate_id).size();
  }
  run.factors = scenario.factor_count;
  run.operations = retained.value().entries.size();

  bool winner_retained = !committed.value().has_winner();
  for (const af::CandidateId candidate_id : retained.value().retained) {
    if (candidate_id == committed.value().selected) {
      winner_retained = true;
    }
  }
  bool disjoint = true;
  for (const af::CandidateId candidate_id : retained.value().retired) {
    if (std::find(retained.value().retained.begin(), retained.value().retained.end(),
                  candidate_id) != retained.value().retained.end()) {
      disjoint = false;
      break;
    }
  }
  std::vector<af::CandidateId> classified;
  classified.reserve(retained.value().entries.size());
  for (const af::RetentionDecisionEntry& entry : retained.value().entries) {
    classified.push_back(entry.candidate);
  }
  std::sort(classified.begin(), classified.end());
  classified.erase(std::unique(classified.begin(), classified.end()), classified.end());
  const bool entries_cover_population =
      retained.value().entries.size() == scenario.candidates.size() &&
      classified.size() == scenario.candidates.size();
  run.integrity_ok = retained.value().committed && winner_retained && disjoint &&
                     entries_cover_population &&
                     retained.value().retained.size() <= policy.retention.max_retained;
  if (!run.integrity_ok) {
    run.integrity_detail =
        "the committed retention decision is not consistent with the committed ranking";
  }
  return run;
}

af_bench::CompletedRun benchmark_lineage_traversal(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  if (!build_registered(size, scenario, true)) {
    return failed_run("setup failed: " + scenario.error);
  }

  std::unordered_map<af::CandidateId, af::LineageNode> index;
  for (const af::LineageNode& node : scenario.core->lineage_nodes()) {
    index.emplace(node.candidate, node);
  }
  af::LineageGraph graph;
  graph.restore(std::move(index));
  const std::vector<af::CandidateId> ordered = scenario.candidates;
  const af::CandidateId first = ordered.empty() ? af::CandidateId() : ordered.front();

  std::uint64_t visited = 0;
  std::uint64_t ancestor_total = 0;
  std::uint64_t path_total = 0;
  af::Status validity;
  std::size_t descendant_count = 0;

  Timer timer;
  for (const auto& entry : graph.nodes()) {
    ++visited;
    ancestor_total += graph.ancestors_of(entry.first).size();
    path_total += graph.path_to_root(entry.first).size();
  }
  if (first.valid()) {
    descendant_count = graph.descendants_of(first).size();
  }
  validity = graph.validate();
  const double elapsed = timer.ms();

  af_bench::CompletedRun run;
  run.elapsed_ms = elapsed;
  run.candidates = scenario.candidates.size();
  run.evaluations = 0;
  run.factors = scenario.factor_count;
  run.operations = visited;

  run.integrity_ok = graph.size() == static_cast<std::size_t>(size) &&
                     visited == static_cast<std::size_t>(size) && ancestor_total == 0 &&
                     path_total == static_cast<std::size_t>(size) && descendant_count == 0 &&
                     validity.ok();
  if (!run.integrity_ok) {
    run.integrity_detail =
        "lineage traversal did not reproduce the expected shape: every candidate of this "
        "population starts its own lineage, so every node is a depth 0 root";
  }
  return run;
}

af_bench::CompletedRun benchmark_snapshot_serialize(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  if (!build_evaluated(size, scenario, nullptr, nullptr)) {
    return failed_run("setup failed: " + scenario.error);
  }

  Timer timer;
  const af::FoundrySnapshot snapshot = scenario.core->snapshot();
  const af::Result<std::string> image = af::serialize_snapshot(snapshot);
  if (!image.ok()) {
    return failed_run("serialize_snapshot failed: " + image.status().to_string());
  }
  const af::Result<af::SnapshotHeader> header = af::parse_snapshot_header(image.value());
  const double elapsed = timer.ms();
  if (!header.ok()) {
    return failed_run("parse_snapshot_header failed: " + header.status().to_string());
  }

  af_bench::CompletedRun run;
  run.elapsed_ms = elapsed;
  run.candidates = snapshot.candidates.size();
  run.evaluations = snapshot.evaluations.size();
  run.factors = scenario.factor_count;
  run.operations = snapshot.candidates.size();

  const af::Result<af::FoundrySnapshot> reloaded = af::deserialize_snapshot(image.value());
  run.integrity_ok = reloaded.ok() && af::snapshots_are_equal(snapshot, reloaded.value()) &&
                     header.value().format_version == af::kSnapshotFormatVersion &&
                     header.value().payload_length ==
                         static_cast<std::uint64_t>(image.value().size() - af::kSnapshotHeaderBytes);
  if (!run.integrity_ok) {
    run.integrity_detail = reloaded.ok()
                               ? "the serialized image did not reload equal to its source state"
                               : "the serialized image did not reload: " +
                                     reloaded.status().to_string();
  }
  return run;
}

af_bench::CompletedRun benchmark_snapshot_deserialize(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  if (!build_evaluated(size, scenario, nullptr, nullptr)) {
    return failed_run("setup failed: " + scenario.error);
  }
  const af::FoundrySnapshot snapshot = scenario.core->snapshot();
  const af::Result<std::string> image = af::serialize_snapshot(snapshot);
  if (!image.ok()) {
    return failed_run("serialize_snapshot failed: " + image.status().to_string());
  }

  Timer timer;
  const af::Result<af::FoundrySnapshot> loaded = af::deserialize_snapshot(image.value());
  const double elapsed = timer.ms();
  if (!loaded.ok()) {
    return failed_run("deserialize_snapshot failed: " + loaded.status().to_string());
  }

  af_bench::CompletedRun run;
  run.elapsed_ms = elapsed;
  run.candidates = loaded.value().candidates.size();
  run.evaluations = loaded.value().evaluations.size();
  run.factors = scenario.factor_count;
  run.operations = loaded.value().candidates.size();
  run.integrity_ok = af::snapshots_are_equal(snapshot, loaded.value());
  if (!run.integrity_ok) {
    run.integrity_detail = "the decoded snapshot differs from the state that was encoded: " +
                           af::first_snapshot_difference(snapshot, loaded.value());
  }
  return run;
}

af_bench::CompletedRun benchmark_recovery(std::uint64_t size) {
  if (size == 0 || size > 2000000u) {
    return failed_run("size out of range");
  }
  Scenario scenario;
  if (!build_evaluated(size, scenario, nullptr, nullptr)) {
    return failed_run("setup failed: " + scenario.error);
  }
  const af::FoundrySnapshot snapshot = scenario.core->snapshot();
  const af::Result<std::string> image = af::serialize_snapshot(snapshot);
  if (!image.ok()) {
    return failed_run("serialize_snapshot failed: " + image.status().to_string());
  }
  const af::Result<af::FoundrySnapshot> loaded = af::deserialize_snapshot(image.value());
  if (!loaded.ok()) {
    return failed_run("deserialize_snapshot failed: " + loaded.status().to_string());
  }
  const af::CoordinatorEpoch epoch_before = snapshot.epoch;

  std::unique_ptr<af::FoundryCore> recovered = std::make_unique<af::FoundryCore>(
      scenario.core->config());
  Timer timer;
  const af::Status status = recovered->recover(loaded.value());
  const double elapsed = timer.ms();
  if (!status.ok()) {
    return failed_run("recover failed: " + status.to_string());
  }

  af_bench::CompletedRun run;
  run.elapsed_ms = elapsed;
  run.candidates = static_cast<std::uint64_t>(recovered->population_candidates(
                                                   scenario.population)
                                                   .size());
  run.evaluations = static_cast<std::uint64_t>(snapshot.evaluations.size());
  run.factors = scenario.factor_count;
  run.operations = run.candidates;

  bool every_worker_offline = true;
  for (const af::WorkerRecord& worker : recovered->workers()) {
    if (worker.state != af::WorkerState::Offline || worker.active_attempt.valid()) {
      every_worker_offline = false;
      break;
    }
  }
  const std::vector<std::string> violations = recovered->audit();
  run.integrity_ok = recovered->epoch().value() == epoch_before.value() + 1 &&
                     recovered->population_candidates(scenario.population).size() ==
                         static_cast<std::size_t>(size) &&
                     every_worker_offline && violations.empty();
  if (!run.integrity_ok) {
    run.integrity_detail =
        "recovery did not advance the coordinator epoch, restore the population, or leave every "
        "worker offline";
  }
  return run;
}

// ---------------------------------------------------------------------------
// Complexity notes
// ---------------------------------------------------------------------------

void print_complexity_notes(af_bench::BenchHarness& harness) {
  harness.header("complexity notes (what the measured operation actually does)");
  harness.line("  register_candidates   each authorization scans the population's candidate slots and");
  harness.line("                        then the open attempts to find a dispatchable slot, so slot");
  harness.line("                        registration is superlinear in the population size");
  harness.line("  ingest_evaluations     every record is compared against every existing record before");
  harness.line("                        it is stored, so ingesting E records costs O(E) per record and");
  harness.line("                        O(E^2) per population");
  harness.line("  select_candidates     selection filters linearly and then sorts, so ranking itself is");
  harness.line("                        O(N log N); per candidate it also rebuilds that candidate's");
  harness.line("                        evidence view by scanning all evidence records, which is the");
  harness.line("                        dominant term when the evidence set is large");
  harness.line("  retention_selection   retention walks the committed ranking once and classifies every");
  harness.line("                        population candidate exactly once: linear plus one sort");
  harness.line("  lineage_traversal     ancestors_of and path_to_root are bounded by lineage depth;");
  harness.line("                        descendants_of walks every node's primary-parent chain");
  harness.line("  snapshot_serialize    linear in durable records; the image header carries the");
  harness.line("                        payload CRC-32C");
  harness.line("  snapshot_deserialize  linear decoding plus full semantic validation of every record");
  harness.line("  recovery              linear in durable records; per candidate it scans the attempt");
  harness.line("                        set to classify ambiguous work, which is superlinear");
  harness.line("");
  harness.line("  A SCALING line compares the smallest and largest measured size and says whether the");
  harness.line("  observed growth is consistent with the expected ratio or slower than it.");
}

}  // namespace

int main(int argc, char** argv) {
  af_bench::BenchOptions options;
  std::string error;
  if (!af_bench::parse_bench_options(argc, argv, options, error)) {
    std::printf("af_benchmarks: %s\n", error.c_str());
    return 2;
  }

  af_bench::BenchHarness harness(options.repeats);
  harness.line("Autonomous Foundry benchmarks");
  harness.line("task contract: mandatory deterministic static rule over the candidate source,");
  harness.line("optional reference structural metric; both evaluators run in process, so these");
  harness.line("numbers measure the runtime, not compiler latency");
  harness.line("every run rebuilds its scenario through the real state machine; setup is excluded");
  harness.line("from the reported durations and repeated per run");
  std::string sizes;
  for (std::size_t index = 0; index < options.sizes.size(); ++index) {
    if (index != 0) {
      sizes.push_back(',');
    }
    sizes.append(std::to_string(options.sizes[index]));
  }
  harness.line("sizes=" + sizes + " repeats=" + std::to_string(options.repeats) +
               (options.only.empty() ? std::string() : (" only=" + options.only)));
  print_complexity_notes(harness);

  struct BenchSpec {
    const char* name;
    const char* unit;
    const char* timed_section;
    af_bench::CompletedRun (*body)(std::uint64_t);
  };
  const BenchSpec benchmarks[] = {
      {"register_candidates", "candidate_registrations",
       "authorize_attempt + confirm_dispatch per candidate slot",
       &benchmark_register_candidates},
      {"ingest_evaluations", "evaluation_records",
       "begin_evaluation + one record per declared requirement; summed over the run",
       &benchmark_ingest_evaluations},
      {"select_candidates", "candidates_ranked",
       "prepare_selection + commit_selection over the whole population",
       &benchmark_select_candidates},
      {"retention_selection", "candidates_classified",
       "prepare_retention + commit_retention over the committed ranking",
       &benchmark_retention_selection},
      {"lineage_traversal", "lineage_nodes_traversed",
       "ancestors_of + path_to_root for every node, one full descendants_of walk, validate",
       &benchmark_lineage_traversal},
      {"snapshot_serialize", "candidate_records_encoded",
       "FoundryCore::snapshot() + serialize_snapshot + parse_snapshot_header",
       &benchmark_snapshot_serialize},
      {"snapshot_deserialize", "candidate_records_decoded", "deserialize_snapshot",
       &benchmark_snapshot_deserialize},
      {"recovery", "candidates_recovered", "FoundryCore::recover on a fresh core",
       &benchmark_recovery},
  };

  std::map<std::string, std::map<std::uint64_t, af_bench::BenchRecord>> records;
  for (const BenchSpec& spec : benchmarks) {
    if (!af_bench::benchmark_selected(options, spec.name)) {
      continue;
    }
    for (const std::uint64_t size : options.sizes) {
      harness.header(std::string(spec.name) + " at " + std::to_string(size) + " candidates");
      const af_bench::BenchRecord record =
          harness.run(spec.name, size, spec.unit, spec.timed_section,
                      [size, &spec]() { return spec.body(size); });
      records[spec.name][size] = record;
    }
  }

  harness.header("observed growth");
  for (const auto& entry : records) {
    const std::map<std::uint64_t, af_bench::BenchRecord>& by_size = entry.second;
    if (by_size.size() < 2) {
      continue;
    }
    const af_bench::BenchRecord& smallest = by_size.begin()->second;
    const af_bench::BenchRecord& largest = by_size.rbegin()->second;
    if (!(smallest.best_ms > 0.0) || !(largest.best_ms > 0.0)) {
      continue;
    }
    const double size_ratio =
        static_cast<double>(largest.size) / static_cast<double>(smallest.size);
    const double expected =
        size_ratio * (std::log(static_cast<double>(largest.size)) /
                      std::log(static_cast<double>(smallest.size)));
    harness.scaling(entry.first, smallest.size, smallest.best_ms, largest.size, largest.best_ms,
                    expected, "linear filtering plus a sort into a total order");
  }

  if (harness.failed()) {
    harness.line("");
    harness.line("RESULT: FAIL (at least one benchmark failed its integrity check)");
    return 1;
  }
  harness.line("");
  harness.line("RESULT: PASS (every benchmark completed its work and passed its integrity check)");
  return 0;
}
