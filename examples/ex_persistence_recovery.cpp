// examples/ex_persistence_recovery.cpp
//
// Durable state, a new coordinator incarnation, and what survives.
//
// The example builds real state, writes it with SnapshotStore, reads it back,
// and adopts it in a fresh FoundryCore through recover(). It then prints,
// concretely:
//
//   * the coordinator epoch advanced, so every authority issued by the previous
//     incarnation is dead
//   * history survived: the task, both populations, the candidates, the
//     evidence, the committed selection and retention, the lineage graph and
//     the revision number
//   * live worker authority did not survive: the recovered worker is offline,
//     the pre-restart session and operation authority are both refused, and an
//     assignment that was in flight becomes OutcomeUnknown rather than being
//     invented as a success or a failure
//   * the recovered foundry can continue: the worker reconnects, explicitly
//     revalidates, the recovered population is revalidated, and the slot whose
//     outcome was ambiguous is retried to completion
//
// It also shows that a damaged image is refused rather than partially trusted.
//
// The mandatory gate of this task contract is a deterministic static rule over
// the candidate source, so the example never depends on an installed compiler.
//
// Exit code 0 means the image round-tripped, the recovered state was consistent
// with the state it was written from, and the recovered foundry did real work
// afterwards.

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
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/version.hpp"
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

// ---------------------------------------------------------------------------
// Operator identities, foundry, scratch tree
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
// Task contract with an in-process mandatory gate
// ---------------------------------------------------------------------------

af::Result<af::TaskSpec> make_in_process_task(af::TaskId id, af::PolicyId policy,
                                              af::PolicyGeneration policy_generation) {
  af::TaskSpec task;
  task.id = id;
  task.generation = af::TaskGeneration::first();
  task.name = "recovery-reference";
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
  task.closure_criteria.push_back("a selection decision is committed for the population");

  task.policy = policy;
  task.policy_generation = policy_generation;

  std::string digest;
  AF_TRY_ASSIGN(digest, af::compute_task_digest(task));
  task.content_digest = std::move(digest);
  AF_TRY(af::validate_task(task));
  return task;
}

// ---------------------------------------------------------------------------
// Production and evaluation plumbing
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

struct Produced {
  af::CandidateId candidate;
  af::WorkerOperationAuthority authority;
  std::string source;
  std::string strategy;
};

bool produce(Report& report, af::FoundryCore& core,
             const af::WorkerSessionAuthority& session, const std::string& label,
             std::string source, std::string strategy, Produced& produced) {
  const af::Result<af::PendingDispatch> pending = core.authorize_attempt(session);
  if (!report.expect_status(pending.status(), label + ": authorize_attempt")) {
    return false;
  }
  if (!report.expect_status(
          core.confirm_dispatch(session, pending.value().attempt.id,
                                pending.value().attempt.generation)
              .status(),
          label + ": confirm_dispatch")) {
    return false;
  }
  produced.candidate = pending.value().attempt.candidate;
  produced.authority = operation_authority(pending.value());
  produced.source = std::move(source);
  produced.strategy = std::move(strategy);
  return true;
}

bool publish(Report& report, af::FoundryCore& core, const Produced& produced,
             const std::string& label) {
  af::ArtifactRef artifact;
  artifact.name = std::string(af::kReferenceTaskSourceArtifact);
  artifact.size_bytes = static_cast<std::uint64_t>(produced.source.size());
  artifact.content_digest = af::sha256_hex(produced.source);
  std::vector<af::ArtifactRef> artifacts;
  artifacts.push_back(artifact);
  return report.expect_status(
      core.publish_candidate(produced.authority, artifacts, produced.strategy).status(),
      label + ": publish_candidate");
}

bool evaluate(Report& report, af::FoundryCore& core, const af::TaskSpec& task,
              af::PopulationId population, af::IdAllocator& allocator,
              const std::vector<Produced>& produced,
              std::map<std::string, af::EvaluatorId>& evaluator_ids) {
  const af::EvaluatorRegistry registry = af::EvaluatorRegistry::make_reference();
  for (const Produced& entry : produced) {
    const af::CandidateRecord candidate = core.candidate(entry.candidate).value();
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
      request.artifacts.emplace(std::string(af::kReferenceTaskSourceArtifact), entry.source);
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
      if (!report.expect_status(core.record_evaluation(record),
                                candidate.id.to_string() + ": record '" +
                                    requirement.evaluator_key + "'")) {
        return false;
      }
    }
  }
  return true;
}

std::string forbidden_token_source() {
  std::string source = af::generate_reference_solution(af::ReferenceStrategy::ClosedForm);
  source.append("\n// The static rule is a textual scan, and this comment contains the token "
                "system( that it forbids.\n");
  return source;
}

// ---------------------------------------------------------------------------
// State summary
// ---------------------------------------------------------------------------

struct StateSummary {
  std::uint64_t epoch{0};
  std::uint64_t revision{0};
  std::size_t populations{0};
  std::size_t candidates{0};
  std::size_t evaluations{0};
  std::size_t lineage_nodes{0};
  std::uint64_t selections_committed{0};
  std::uint64_t retentions_committed{0};
  std::uint64_t attempts_completed{0};
  std::uint64_t attempts_outcome_unknown{0};
};

StateSummary summarize(af::FoundryCore& core) {
  StateSummary summary;
  summary.epoch = core.epoch().value();
  summary.revision = core.revision();
  const af::FoundrySnapshot snapshot = core.snapshot();
  summary.populations = snapshot.populations.size();
  summary.candidates = snapshot.candidates.size();
  summary.evaluations = snapshot.evaluations.size();
  summary.lineage_nodes = snapshot.lineage.size();
  const af::FoundryStatistics statistics = core.statistics();
  summary.selections_committed = statistics.selections_committed;
  summary.retentions_committed = statistics.retentions_committed;
  summary.attempts_completed = statistics.attempts_completed;
  summary.attempts_outcome_unknown = statistics.attempts_outcome_unknown;
  return summary;
}

void print_summary(const std::string& label, const StateSummary& summary) {
  Report::item(label + ": epoch=" + std::to_string(summary.epoch) +
               " revision=" + std::to_string(summary.revision) +
               " populations=" + std::to_string(summary.populations) +
               " candidates=" + std::to_string(summary.candidates) +
               " evaluations=" + std::to_string(summary.evaluations) +
               " lineage_nodes=" + std::to_string(summary.lineage_nodes));
  Report::note("selections_committed=" + std::to_string(summary.selections_committed) +
               " retentions_committed=" + std::to_string(summary.retentions_committed) +
               " attempts_completed=" + std::to_string(summary.attempts_completed) +
               " attempts_outcome_unknown=" +
               std::to_string(summary.attempts_outcome_unknown));
}

}  // namespace

int main() {
  Report report;
  Report::line("Autonomous Foundry reference example: persistence and recovery");
  Report::line("durable history survives a new coordinator; live authority does not");

  OperatorContext context;
  if (!build_operator(report, context)) {
    return 1;
  }
  std::unique_ptr<af::FoundryCore> first = make_core(context);

  const af::Result<std::filesystem::path> scratch =
      af::make_transient_directory("af-persistence-recovery");
  if (!report.expect_status(scratch.status(), "transient directory created")) {
    return 1;
  }
  const ScratchGuard scratch_guard(scratch.value());
  const std::filesystem::path snapshot_path = scratch.value() / "foundry-snapshot.afsn";

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
      first->define_policy(af::make_reference_policy(policy_id, af::PolicyGeneration::first()));
  if (!report.expect_status(defined.status(), "reference policy defined")) {
    return 1;
  }
  const af::FoundryPolicy policy = first->policy(defined.value()).value();

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
    const af::Result<af::TaskId> defined_task = first->define_task(spec.value());
    if (!report.expect_status(defined_task.status(), "in-process task defined")) {
      return 1;
    }
    task_id = defined_task.value();
  }
  const af::TaskSpec task = first->task(task_id).value();
  Report::item("task " + task.name + " generation=" + task.generation.to_string());

  Report::section("state before the restart");
  af::PopulationSpec decision_spec;
  decision_spec.name = "P1-decided";
  decision_spec.task = task.id;
  decision_spec.task_generation = task.generation;
  decision_spec.policy = policy.id;
  decision_spec.policy_generation = policy.generation;
  decision_spec.candidate_budget = 2;
  decision_spec.worker_budget = 2;
  decision_spec.population_index = 1;
  const af::Result<af::PopulationId> decided_created =
      first->create_population(decision_spec);
  if (!report.expect_status(decided_created.status(), "the decided population was created")) {
    return 1;
  }
  const af::PopulationId decided = decided_created.value();
  if (!report.expect_status(
          first->start_population(decided, first->population(decided).value().generation),
          "the decided population started")) {
    return 1;
  }

  std::vector<Produced> decided_candidates;
  const std::string sources[] = {af::generate_reference_solution(af::ReferenceStrategy::ClosedForm),
                                 forbidden_token_source()};
  const std::string strategies[] = {"reference-closed-form",
                                    "reference-closed-form-forbidden"};
  af::WorkerSessionAuthority held_session;
  for (std::size_t index = 0; index < 2; ++index) {
    const af::Result<af::WorkerSessionAuthority> session =
        open_worker(*first, context.allocator, "decided-slot-" + std::to_string(index + 1));
    if (!report.expect_status(session.status(), "decided population worker session opened")) {
      return 1;
    }
    if (index == 0) {
      held_session = session.value();
    }
    Produced entry;
    if (!produce(report, *first, session.value(),
                 "decided-slot-" + std::to_string(index + 1), sources[index],
                 strategies[index], entry)) {
      return 1;
    }
    decided_candidates.push_back(entry);
  }
  for (std::size_t index = 0; index < decided_candidates.size(); ++index) {
    if (!publish(report, *first, decided_candidates[index],
                 "decided-slot-" + std::to_string(index + 1))) {
      return 1;
    }
  }

  std::map<std::string, af::EvaluatorId> evaluator_ids;
  if (!evaluate(report, *first, task, decided, context.allocator, decided_candidates,
                evaluator_ids)) {
    return 1;
  }

  const af::Result<af::SelectionDecision> prepared = first->prepare_selection(decided);
  if (!report.expect_status(prepared.status(), "prepare_selection")) {
    return 1;
  }
  const af::Result<af::SelectionDecision> committed =
      first->commit_selection(prepared.value());
  if (!report.expect_status(committed.status(), "commit_selection")) {
    return 1;
  }
  const af::CandidateId winner = committed.value().selected;
  const af::CandidateId rejected =
      decided_candidates[0].candidate == winner ? decided_candidates[1].candidate
                                                : decided_candidates[0].candidate;
  Report::item("committed winner=" + winner.to_string() +
               " ranked=" + std::to_string(committed.value().ranking.size()) +
               " excluded=" + std::to_string(committed.value().excluded.size()));

  const af::Result<af::RetentionDecision> prepared_retention =
      first->prepare_retention(decided);
  if (!report.expect_status(prepared_retention.status(), "prepare_retention")) {
    return 1;
  }
  if (!report.expect_status(first->commit_retention(prepared_retention.value()).status(),
                            "commit_retention")) {
    return 1;
  }
  report.expect(first->candidate(winner).value().retained,
                "the committed winner is retained by the retention decision");
  report.expect(first->candidate(rejected).value().state == af::CandidateState::Retired,
                "the candidate that failed its mandatory gate was retired");

  Report::section("an assignment that is in flight when the coordinator stops");
  af::PopulationSpec in_flight_spec;
  in_flight_spec.name = "P2-in-flight";
  in_flight_spec.task = task.id;
  in_flight_spec.task_generation = task.generation;
  in_flight_spec.policy = policy.id;
  in_flight_spec.policy_generation = policy.generation;
  in_flight_spec.candidate_budget = 1;
  in_flight_spec.worker_budget = 1;
  in_flight_spec.population_index = 1;
  const af::Result<af::PopulationId> in_flight_created =
      first->create_population(in_flight_spec);
  if (!report.expect_status(in_flight_created.status(), "the in-flight population was created")) {
    return 1;
  }
  const af::PopulationId in_flight_population = in_flight_created.value();
  if (!report.expect_status(first->start_population(
                                in_flight_population,
                                first->population(in_flight_population).value().generation),
                            "the in-flight population started")) {
    return 1;
  }
  const af::Result<af::WorkerSessionAuthority> in_flight_session =
      open_worker(*first, context.allocator, "in-flight-slot");
  if (!report.expect_status(in_flight_session.status(), "in-flight worker session opened")) {
    return 1;
  }
  Produced in_flight;
  if (!produce(report, *first, in_flight_session.value(), "in-flight-slot",
               af::generate_reference_solution(af::ReferenceStrategy::Iterative),
               "reference-iterative", in_flight)) {
    return 1;
  }
  Report::item("candidate " + in_flight.candidate.to_string() +
               " is " +
               std::string(af::candidate_state_name(
                   first->candidate(in_flight.candidate).value().state)) +
               " with its assignment dispatched and never published");
  report.expect(first->attempt(in_flight.authority.attempt).value().state ==
                    af::AttemptState::Dispatched,
                "the in-flight attempt is Dispatched at the moment the coordinator stops");

  const af::FoundrySnapshot snapshot = first->snapshot();
  const StateSummary before = summarize(*first);
  print_summary("before", before);

  // A reservation is opened with the attempt and resolved exactly once into
  // consumption or release when that attempt reaches a terminal state; a ledger
  // with no open reservation is the *population closure* condition, not the
  // condition for a population with an assignment still in flight. The state
  // above deliberately leaves one assignment live, so the durable image must
  // carry exactly the reservation that attempt owns - not none, and not one
  // left behind by an attempt that already ended.
  std::uint64_t open_reservations = 0;
  for (const auto& entry : snapshot.ledgers) {
    open_reservations += entry.second.open_reservations();
  }
  std::uint64_t reserved_live_attempts = 0;
  std::uint64_t reserved_terminal_attempts = 0;
  for (const auto& entry : snapshot.attempts) {
    if (!entry.second.holds_reservation) {
      continue;
    }
    if (af::attempt_state_is_terminal(entry.second.state)) {
      ++reserved_terminal_attempts;
    } else {
      ++reserved_live_attempts;
    }
  }
  Report::item("open reservations=" + std::to_string(open_reservations) +
               " held by live attempts=" + std::to_string(reserved_live_attempts) +
               " terminal attempts=" + std::to_string(reserved_terminal_attempts));
  report.expect(reserved_terminal_attempts == 0,
                "no attempt that already reached a terminal state keeps a reservation open");
  report.expect(reserved_live_attempts == 1 && open_reservations == 3 * reserved_live_attempts,
                "the only open reservations are the live attempt's candidate attempt, "
                "worker assignment and active attempt");

  Report::section("durable image");
  const af::Result<std::string> image = af::serialize_snapshot(snapshot);
  if (!report.expect_status(image.status(), "serialize_snapshot")) {
    return 1;
  }
  const af::Result<af::SnapshotHeader> header = af::parse_snapshot_header(image.value());
  if (!report.expect_status(header.status(), "parse_snapshot_header")) {
    return 1;
  }
  Report::item("image bytes=" + std::to_string(image.value().size()) +
               " header bytes=" + std::to_string(af::kSnapshotHeaderBytes));
  Report::item("format_version=" + std::to_string(header.value().format_version) +
               " payload_length=" + std::to_string(header.value().payload_length) +
               " payload_crc32c=" + std::to_string(header.value().payload_crc32c));
  report.expect(header.value().format_version == af::kSnapshotFormatVersion,
                "the image declares the snapshot format version this build understands");

  af::SnapshotStore store(snapshot_path);
  if (!report.expect_status(store.save_and_verify(snapshot),
                            "save_and_verify wrote an image that reloads equal")) {
    return 1;
  }
  report.expect(store.exists(), "the snapshot file exists after the save");
  report.expect(store.save_count() == 1, "the store counted exactly one save");

  const af::Result<af::FoundrySnapshot> loaded = store.load();
  if (!report.expect_status(loaded.status(), "SnapshotStore::load")) {
    return 1;
  }
  if (!report.expect(af::snapshots_are_equal(snapshot, loaded.value()),
                     "the loaded snapshot equals the state that was saved")) {
    Report::note("first difference: " + af::first_snapshot_difference(snapshot, loaded.value()));
  }

  Report::section("a damaged image is refused, not partially trusted");
  std::string corrupted = image.value();
  const std::size_t middle = corrupted.size() / 2;
  corrupted[middle] =
      static_cast<char>(static_cast<unsigned char>(corrupted[middle]) ^ 0x01u);
  const af::Result<af::FoundrySnapshot> corrupt_load = af::deserialize_snapshot(corrupted);
  report.expect(!corrupt_load.ok(), "a payload byte flipped in the middle is refused");
  report.expect(af::is_persistence_rejection(corrupt_load.status().code()),
                "the refusal is a persistence rejection");
  Report::item("corrupted image code=" +
               std::string(af::error_code_name(corrupt_load.status().code())));
  Report::note("corrupted image message: " + corrupt_load.status().message());

  const af::Result<af::FoundrySnapshot> truncated =
      af::deserialize_snapshot(std::string_view(image.value()).substr(0, image.value().size() - 8));
  report.expect(!truncated.ok(), "a truncated image is refused");
  report.expect(af::is_persistence_rejection(truncated.status().code()),
                "the truncation refusal is a persistence rejection");
  Report::item("truncated image code=" +
               std::string(af::error_code_name(truncated.status().code())));

  Report::section("a new coordinator adopts the durable state");
  std::unique_ptr<af::FoundryCore> second = make_core(context);
  if (!report.expect_status(second->recover(loaded.value()), "recover")) {
    return 1;
  }
  const StateSummary after = summarize(*second);
  print_summary("after ", after);
  report.expect(after.epoch == before.epoch + 1,
                "the coordinator epoch advanced by exactly one");
  report.expect(after.populations == before.populations &&
                    after.candidates == before.candidates &&
                    after.evaluations == before.evaluations &&
                    after.lineage_nodes == before.lineage_nodes,
                "history survived: populations, candidates, evidence and lineage are the same");
  report.expect(after.selections_committed == before.selections_committed &&
                    after.retentions_committed == before.retentions_committed,
                "the committed selection and retention decisions survived");
  // The revision is a durable counter of recorded mutations, not a session
  // counter. Recovery adopts the revision carried in the image, so it is never
  // reset, and then records its own reconciliation as one further mutation.
  report.expect(loaded.value().revision == before.revision,
                "the durable image carried the revision the previous coordinator reached");
  report.expect(after.revision == before.revision + 1,
                "recovery adopted that revision and advanced it by exactly one, never reset it");
  report.expect(after.attempts_outcome_unknown == before.attempts_outcome_unknown + 1,
                "the assignment that was in flight became exactly one ambiguous outcome");

  const af::CandidateRecord recovered_winner = second->candidate(winner).value();
  report.expect(recovered_winner.selected, "the recovered winner is still the selected candidate");
  const af::SelectionDecision recovered_selection = second->selection(decided).value();
  report.expect(recovered_selection.state == af::SelectionDecisionState::Committed &&
                    recovered_selection.selected == winner,
                "the durable selection decision is still committed and names the same winner");
  report.expect(second->retention(decided).value().committed,
                "the durable retention decision is still committed");
  report.expect(second->candidate(rejected).value().state == af::CandidateState::Retired,
                "the retired candidate is still retired");
  report.expect(second->task(task_id).value().content_digest == task.content_digest,
                "the task contract survived byte for byte");
  report.expect(second->population(decided).value().state ==
                    af::PopulationState::RevalidationRequired,
                "the recovered population requires explicit revalidation before further decisions");

  const af::CandidateRecord ambiguous_candidate =
      second->candidate(in_flight.candidate).value();
  Report::item("the ambiguous candidate is now " +
               std::string(af::candidate_state_name(ambiguous_candidate.state)));
  report.expect(ambiguous_candidate.state == af::CandidateState::Registered,
                "the ambiguous slot is retryable rather than declared failed or successful");
  const af::AttemptRecord ambiguous_attempt =
      second->attempt(in_flight.authority.attempt).value();
  Report::item("the in-flight attempt is now " +
               std::string(af::attempt_state_name(ambiguous_attempt.state)) + ": " +
               ambiguous_attempt.failure_detail);
  report.expect(ambiguous_attempt.state == af::AttemptState::OutcomeUnknown,
                "the in-flight attempt is OutcomeUnknown, not Failed and not Completed");

  Report::section("live authority did not survive");
  Report::item("recovered worker state=" +
               std::string(af::worker_state_name(second->worker(held_session.worker).value().state)));
  report.expect(second->worker(held_session.worker).value().state == af::WorkerState::Offline,
                "every recovered worker is offline");
  report.expect(!second->worker(held_session.worker).value().active_attempt.valid(),
                "no recovered worker holds an active attempt");

  const af::Result<af::PendingDispatch> replayed_dispatch =
      second->authorize_attempt(held_session);
  report.expect(!replayed_dispatch.ok() &&
                    af::is_stale_authority(replayed_dispatch.status().code()),
                "the pre-restart session is refused as stale authority");
  Report::note("session replay code=" +
               std::string(af::error_code_name(replayed_dispatch.status().code())));

  const af::Result<af::PublicationOutcome> replayed_publish = second->publish_candidate(
      decided_candidates.front().authority, std::vector<af::ArtifactRef>{},
      "replayed-after-restart");
  report.expect(!replayed_publish.ok() &&
                    af::is_stale_authority(replayed_publish.status().code()),
                "the pre-restart publication authority is refused as stale authority");
  Report::note("publication replay code=" +
               std::string(af::error_code_name(replayed_publish.status().code())));

  Report::section("the recovered foundry can continue");
  af::WorkerRegistrationRequest reconnect;
  reconnect.worker = held_session.worker;
  reconnect.boot = held_session.boot;
  reconnect.label = "decided-slot-1";
  reconnect.capability = "reference-static";
  reconnect.process_id = 0;
  const af::Result<af::WorkerSessionAuthority> reconnected = second->register_worker(reconnect);
  if (!report.expect_status(reconnected.status(), "the same incarnation reconnects")) {
    return 1;
  }
  report.expect(reconnected.value().coordinator_epoch == second->epoch(),
                "the reconnection is issued under the current coordinator epoch");
  report.expect(reconnected.value().session != held_session.session,
                "the reconnection issues a fresh session identity");
  report.expect(second->worker(held_session.worker).value().state ==
                    af::WorkerState::RevalidationRequired,
                "a reconnecting incarnation must revalidate before it may act");

  const af::Status too_early = second->worker_ready(reconnected.value(), "reference-static");
  report.expect(!too_early.ok() && too_early.code() == af::ErrorCode::RevalidationRequired,
                "the runtime refuses ready work before revalidation");
  Report::note("pre-revalidation ready code=" +
               std::string(af::error_code_name(too_early.code())));

  const af::Result<af::WorkerSessionAuthority> revalidated =
      second->revalidate_worker(reconnected.value(), "reconnected after a coordinator restart");
  if (!report.expect_status(revalidated.status(), "revalidate_worker")) {
    return 1;
  }
  report.expect(revalidated.value().session != reconnected.value().session &&
                    revalidated.value().session_generation.value() >
                        reconnected.value().session_generation.value(),
                "revalidation issues a fresh session and advances the session generation");
  if (!report.expect_status(second->worker_ready(revalidated.value(), "reference-static"),
                            "the revalidated incarnation reports ready")) {
    return 1;
  }

  const af::PopulationGeneration recovered_generation =
      second->population(in_flight_population).value().generation;
  if (!report.expect_status(second->revalidate_population(in_flight_population,
                                                          recovered_generation,
                                                          "recovered at the new epoch"),
                            "the recovered population is explicitly revalidated")) {
    return 1;
  }
  Report::item("the recovered population is " +
               std::string(af::population_state_name(
                   second->population(in_flight_population).value().state)));

  Produced retry;
  if (!produce(report, *second, revalidated.value(), "retry",
               af::generate_reference_solution(af::ReferenceStrategy::Iterative),
               "reference-iterative", retry)) {
    return 1;
  }
  report.expect(retry.candidate == in_flight.candidate,
                "the retry reuses the slot whose outcome was ambiguous");
  report.expect(retry.authority.population == in_flight_population,
                "the retry was authorized against the recovered population, not the decided one");
  if (!publish(report, *second, retry, "retry")) {
    return 1;
  }
  std::vector<Produced> retried;
  retried.push_back(retry);
  if (!evaluate(report, *second, task, in_flight_population, context.allocator, retried,
                evaluator_ids)) {
    return 1;
  }
  const af::CandidateRecord recovered_slot = second->candidate(in_flight.candidate).value();
  Report::item("the retried slot reached " +
               std::string(af::candidate_state_name(recovered_slot.state)) + " with " +
               std::to_string(recovered_slot.attempt_count) + " attempt(s)");
  report.expect(recovered_slot.state == af::CandidateState::Evaluated,
                "the recovered foundry carried the retried slot to Evaluated");
  report.expect(recovered_slot.attempt_count == 2,
                "the retry was a second attempt rather than a reuse of the ambiguous one");

  Report::section("self-consistency audit");
  const std::vector<std::string> violations = second->audit();
  report.expect(violations.empty(), "audit reports no violations");
  for (const std::string& violation : violations) {
    Report::note(violation);
  }
  report.expect(second->foundry() == context.foundry && second->run() == context.run,
                "the recovered core still owns the same foundry and run identity");

  if (report.failures == 0) {
    Report::line("\nRESULT: PASS");
    return 0;
  }
  Report::line("\nRESULT: FAIL failures=" + std::to_string(report.failures));
  return 1;
}
