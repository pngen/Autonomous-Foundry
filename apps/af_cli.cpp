// apps/af_cli.cpp
//
// The operator CLI.
//
// Two sources, one vocabulary:
//
//   --state <path>            an OFFLINE snapshot file, opened read-only
//   --coordinator host:port   a RUNNING coordinator, reached over the control
//                             plane
//
// Read-only commands print a stable, scriptable, human-readable report. The CLI
// never mutates a snapshot: it loads one, adopts it into an in-process core to
// answer queries, and writes nothing back. The epoch of that in-process core is
// advanced in memory only, exactly as a recovered coordinator would, and the
// file on disk is untouched.
//
// Exit codes:
//   0  the command succeeded
//   2  the command was understood and the operation was rejected
//   3  the command line was wrong
//
// A question that the selected source cannot answer is reported as rejected
// rather than answered with a plausible guess.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "autonomous_foundry/evaluator.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/protocol.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/transport.hpp"
#include "autonomous_foundry/workspace.hpp"
#include "cli_impl.hpp"

namespace autonomous_foundry {
namespace cli {
namespace {

constexpr std::string_view kCliControllerLabel = "af_cli";

/// Command handlers return an exit code, not a Status, so the library's
/// propagate-and-return macro cannot be used inside them. These two local
/// macros reproduce that shape while turning a failure into the correct exit
/// code: a malformed operand is a usage error, and a refused decode is a
/// rejected operation.
#define AF_CLI_TRY(expr)                                                              \
  do {                                                                                \
    const ::autonomous_foundry::Status af_cli_status_probe__ = (expr);                \
    if (!af_cli_status_probe__.ok()) {                                                \
      std::cerr << "af_cli: " << af_cli_status_probe__.to_string() << "\n";           \
      return kExitUsage;                                                              \
    }                                                                                 \
  } while (false)

#define AF_CLI_ASSIGN(target, expr)                                                   \
  do {                                                                                \
    auto af_cli_result_probe__ = (expr);                                              \
    if (!af_cli_result_probe__.ok()) {                                                \
      std::cerr << "af_cli: " << af_cli_result_probe__.status().to_string() << "\n";  \
      return kExitRejected;                                                           \
    }                                                                                 \
    (target) = std::move(af_cli_result_probe__).value();                              \
  } while (false)

/// Findings directory used by the reference demo. It is below the platform
/// temporary root, and it is created and removed by the demo itself.
[[nodiscard]] std::filesystem::path demo_root() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    return std::filesystem::path("af_cli_demo");
  }
  return base / "af-cli-reference-demo";
}

[[nodiscard]] std::string yes_no(bool value) { return value ? "yes" : "no"; }

[[nodiscard]] std::string generation_text(std::uint32_t value) {
  return value == 0 ? std::string("none") : std::to_string(value);
}

void print_identity(std::ostream& out, std::string_view label, std::uint64_t raw) {
  if (raw == 0) {
    out << "  " << label << ": none\n";
    return;
  }
  out << "  " << label << ": " << raw << "\n";
}

void print_generation(std::ostream& out, std::string_view label, std::uint32_t value) {
  out << "  " << label << ": " << generation_text(value) << "\n";
}

void print_evidence(std::ostream& out, const EvaluationRecord& record) {
  out << "  - key=" << record.evaluator_key << " kind=" << evaluator_kind_name(record.kind)
      << " class=" << requirement_class_name(record.requirement_class)
      << " outcome=" << evaluation_outcome_name(record.outcome)
      << " complete=" << yes_no(record.complete)
      << " generation=" << generation_text(record.generation.value())
      << " decided_epoch=" << record.decided_epoch.value();
  if (record.has_score) {
    out << " score=" << record.score;
  }
  out << " authoritative_for_mandatory=" << yes_no(record.authoritative_for_mandatory()) << "\n";
  if (!record.diagnostics.empty()) {
    out << "      diagnostics: " << record.diagnostics << "\n";
  }
  if (!record.evidence_digest.empty()) {
    out << "      evidence_digest: " << record.evidence_digest << "\n";
  }
}

/// Adopt an offline snapshot into a queryable core. Nothing is written back.
[[nodiscard]] Result<std::unique_ptr<FoundryCore>> adopt_snapshot(const Options& options) {
  SnapshotStore store(options.state_path);
  if (!store.exists()) {
    return Status(ErrorCode::UnknownIdentity,
                  "no snapshot exists at '" + path_to_utf8(options.state_path) + "'");
  }
  Result<FoundrySnapshot> loaded = store.load();
  if (!loaded.ok()) {
    return loaded.status().with_context("loading '" + path_to_utf8(options.state_path) + "'");
  }
  FoundrySnapshot snapshot = std::move(loaded).value();

  FoundryConfig config;
  config.foundry = snapshot.foundry;
  config.run = snapshot.run;
  config.epoch = snapshot.epoch;
  config.id_salt = snapshot.id_salt;
  config.workspace_root = std::filesystem::path();
  config.default_budgets = BudgetLimits();

  std::unique_ptr<FoundryCore> core = std::make_unique<FoundryCore>(config);
  // recover() advances the in-memory epoch, which is what makes every authority
  // recorded by the previous incumbent observably stale. The snapshot file is
  // not written, so the durable epoch is unchanged.
  AF_TRY(core->recover(std::move(snapshot)));
  return core;
}

[[nodiscard]] int report_rejected(std::string_view what, const Status& status) {
  std::cerr << "af_cli: " << what << ": " << status.to_string() << "\n";
  return kExitRejected;
}

[[nodiscard]] int report_usage(std::string_view what) {
  std::cerr << "af_cli: " << what << "\n";
  return kExitUsage;
}

}  // namespace

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

void print_usage(std::ostream& out) {
  out << "usage: af_cli (--state <snapshot> | --coordinator <host:port>) <command> [arguments]\n"
         "\n"
         "offline commands (need --state):\n"
         "  list-populations                 every population, in identity order\n"
         "  show-population <raw-id>         one population and its closure blockers\n"
         "  show-candidate <raw-id>          one candidate and its evidence\n"
         "  show-evaluations <raw-id>        the evidence set of one candidate\n"
         "  show-retained <raw-id>           the retention decision of one population\n"
         "  explain-selection <raw-id>       the committed decision of one population\n"
         "  show-lineage [raw-id]            the lineage graph, or one candidate's ancestry\n"
         "  snapshot                         summarise the durable snapshot\n"
         "  check                            verify the durable snapshot and audit it\n"
         "\n"
         "live commands (need --coordinator):\n"
         "  show-population <raw-id>\n"
         "  show-candidate <raw-id>\n"
         "  show-evaluations <raw-id>\n"
         "  show-lineage <raw-id>\n"
         "  show-retained <raw-id>\n"
         "  explain-selection <raw-id>\n"
         "  check                            run the coordinator's own audit\n"
         "\n"
         "either source:\n"
         "  run-reference-demo               run one complete in-process population\n";
}

Status parse_options(int argc, char** argv, Options* out) {
  Options options;
  bool have_command = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (!have_command && (argument == "--state" || argument == "--coordinator")) {
      if (index + 1 >= argc) {
        return Status(ErrorCode::InvalidArgument,
                      std::string(argument) + " requires a value");
      }
      ++index;
      if (argument == "--state") {
        options.state_path = std::filesystem::path(std::string(argv[index]));
      } else {
        options.coordinator = std::string(argv[index]);
      }
      continue;
    }
    if (!have_command && (argument == "--help" || argument == "-h")) {
      options.command = "help";
      have_command = true;
      continue;
    }
    if (!have_command) {
      if (!argument.empty() && argument.front() == '-') {
        return Status(ErrorCode::InvalidArgument,
                      "unknown option '" + std::string(argument) +
                          "'; expected --state or --coordinator");
      }
      options.command = std::string(argument);
      have_command = true;
      continue;
    }
    options.operands.emplace_back(argument);
  }
  if (options.command.empty()) {
    return Status(ErrorCode::InvalidArgument, "no command was given");
  }
  if (!options.coordinator.empty() && !options.state_path.empty()) {
    return Status(ErrorCode::InvalidArgument,
                  "--state and --coordinator select different sources; choose one");
  }
  *out = std::move(options);
  return Status();
}

Status parse_identity_operand(std::string_view text, std::uint64_t* out) {
  const Result<std::uint64_t> parsed = parse_raw_identity(text);
  if (!parsed.ok()) {
    return parsed.status();
  }
  *out = parsed.value();
  return Status();
}

bool command_is_live(std::string_view command) {
  return command == "check" || command == "show-population" || command == "show-candidate" ||
         command == "show-evaluations" || command == "show-lineage" ||
         command == "show-retained" || command == "explain-selection";
}

bool command_is_known(std::string_view command) {
  return command_is_live(command) || command == "list-populations" || command == "snapshot" ||
         command == "run-reference-demo" || command == "help";
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

void print_population_report(std::ostream& out, const FoundryCore& core,
                             const PopulationRecord& population) {
  out << "population\n";
  print_identity(out, "id", population.id.raw());
  print_generation(out, "generation", population.generation.value());
  out << "  name: " << population.name << "\n";
  out << "  state: " << population_state_name(population.state) << "\n";
  print_identity(out, "task", population.task.raw());
  print_generation(out, "task_generation", population.task_generation.value());
  print_identity(out, "policy", population.policy.raw());
  print_generation(out, "policy_generation", population.policy_generation.value());
  out << "  candidate_budget: " << population.candidate_budget << "\n";
  out << "  worker_budget: " << population.worker_budget << "\n";
  out << "  candidate_count: " << population.candidates.size() << "\n";
  print_generation(out, "committed_selection", population.committed_selection.value());
  out << "  retention_committed: " << yes_no(population.retention_committed) << "\n";
  out << "  promotion_requests: " << population.promotion_requests << "\n";
  print_identity(out, "committed_promotion_request", population.committed_promotion_request.raw());
  out << "  created_epoch: " << population.created_epoch.value() << "\n";
  out << "  state_epoch: " << population.state_epoch.value() << "\n";
  if (!population.status_detail.empty()) {
    out << "  status_detail: " << population.status_detail << "\n";
  }
  const std::vector<std::string> blockers = core.closure_blockers(population.id);
  out << "  closure_blockers: " << blockers.size() << "\n";
  for (const std::string& blocker : blockers) {
    out << "    - " << blocker << "\n";
  }
  out << "  candidates:\n";
  for (const CandidateId candidate : population.candidates) {
    const Result<CandidateRecord> record = core.candidate(candidate);
    if (!record.ok()) {
      out << "    - " << candidate.raw() << " (missing record)\n";
      continue;
    }
    out << "    - " << candidate.raw() << " state=" << candidate_state_name(record.value().state)
        << " selected=" << yes_no(record.value().selected)
        << " retained=" << yes_no(record.value().retained) << "\n";
  }
}

void print_candidate_report(std::ostream& out, const FoundryCore& core,
                            const CandidateRecord& candidate) {
  out << "candidate\n";
  print_identity(out, "id", candidate.id.raw());
  print_generation(out, "generation", candidate.generation.value());
  out << "  state: " << candidate_state_name(candidate.state) << "\n";
  print_identity(out, "population", candidate.population.raw());
  print_generation(out, "population_generation", candidate.population_generation.value());
  print_identity(out, "task", candidate.task.raw());
  print_generation(out, "task_generation", candidate.task_generation.value());
  print_identity(out, "lineage", candidate.lineage.raw());
  out << "  depth: " << candidate.depth << "\n";
  out << "  parents:";
  if (candidate.parents.empty()) {
    out << " none";
  }
  for (const CandidateId parent : candidate.parents) {
    out << " " << parent.raw();
  }
  out << "\n";
  print_identity(out, "producer_worker", candidate.producer_worker.raw());
  print_identity(out, "attempt", candidate.attempt.raw());
  print_generation(out, "attempt_generation", candidate.attempt_generation.value());
  print_identity(out, "assignment", candidate.assignment.raw());
  out << "  attempt_count: " << candidate.attempt_count << "\n";
  print_generation(out, "evidence_generation", candidate.evidence_generation.value());
  print_generation(out, "evaluation_generation", candidate.evaluation_generation.value());
  out << "  selected: " << yes_no(candidate.selected) << "\n";
  out << "  retained: " << yes_no(candidate.retained) << "\n";
  out << "  promotion_eligible: " << yes_no(candidate.promotion_eligible) << "\n";
  out << "  promotion_requested: " << yes_no(candidate.promotion_requested) << "\n";
  out << "  diversity_key: " << candidate.diversity_key << "\n";
  out << "  artifacts: " << candidate.artifacts.size() << "\n";
  for (const ArtifactRef& artifact : candidate.artifacts) {
    out << "    - " << artifact.name << " bytes=" << artifact.size_bytes
        << " digest=" << artifact.content_digest << "\n";
  }
  out << "  artifact_set_digest: " << candidate.artifact_set_digest << "\n";
  if (!candidate.failure_reason.empty()) {
    out << "  failure_reason: " << candidate.failure_reason << "\n";
  }
  if (!candidate.superseded_by_reason.empty()) {
    out << "  superseded_by_reason: " << candidate.superseded_by_reason << "\n";
  }
  const std::vector<EvaluationRecord> records = core.candidate_evaluations(candidate.id);
  out << "  evidence_records: " << records.size() << "\n";
  for (const EvaluationRecord& record : records) {
    print_evidence(out, record);
  }
}

void print_evaluation_report(std::ostream& out, const FoundryCore& core, CandidateId candidate) {
  const Result<CandidateRecord> record = core.candidate(candidate);
  if (!record.ok()) {
    out << "candidate " << candidate.raw() << ": record not found\n";
    return;
  }
  out << "evidence\n";
  print_identity(out, "candidate", record.value().id.raw());
  print_generation(out, "candidate_generation", record.value().generation.value());
  print_generation(out, "evidence_generation", record.value().evidence_generation.value());
  const std::vector<EvaluationRecord> records = core.candidate_evaluations(candidate);
  out << "  records: " << records.size() << "\n";
  for (const EvaluationRecord& evidence : records) {
    print_evidence(out, evidence);
  }
  const Result<TaskSpec> task = core.task(record.value().task);
  if (task.ok()) {
    out << "  declared requirements: " << task.value().requirements.size() << "\n";
    for (const EvaluationRequirement& requirement : task.value().requirements) {
      bool satisfied = false;
      for (const EvaluationRecord& evidence : records) {
        if (evidence.evaluator_key == requirement.evaluator_key &&
            evidence.authoritative_for_mandatory()) {
          satisfied = true;
          break;
        }
      }
      out << "    - key=" << requirement.evaluator_key
          << " class=" << requirement_class_name(requirement.requirement_class)
          << " satisfied_by_authoritative_pass=" << yes_no(satisfied) << "\n";
    }
  }
}

void print_retention_report(std::ostream& out, const FoundryCore& core, PopulationId population) {
  const Result<RetentionDecision> decision = core.retention(population);
  if (!decision.ok()) {
    out << "retention: no decision is recorded for population " << population.raw() << "\n";
    return;
  }
  const RetentionDecision& value = decision.value();
  out << "retention\n";
  print_identity(out, "population", value.population.raw());
  print_generation(out, "population_generation", value.population_generation.value());
  print_generation(out, "selection_generation", value.selection_generation.value());
  print_identity(out, "policy", value.policy.raw());
  out << "  committed: " << yes_no(value.committed) << "\n";
  out << "  canonical_state_digest: " << value.canonical_state_digest << "\n";
  out << "  entries: " << value.entries.size() << "\n";
  for (const RetentionDecisionEntry& entry : value.entries) {
    out << "    - candidate=" << entry.candidate.raw() << " rank=" << entry.rank
        << " outcome=" << retention_outcome_name(entry.outcome);
    if (!entry.lineage_key.empty()) {
      out << " lineage=" << entry.lineage_key;
    }
    if (!entry.detail.empty()) {
      out << " detail=" << entry.detail;
    }
    out << "\n";
  }
  out << "  retained:";
  for (const CandidateId id : value.retained) {
    out << " " << id.raw();
  }
  out << "\n  retired:";
  for (const CandidateId id : value.retired) {
    out << " " << id.raw();
  }
  out << "\n";
}

void print_selection_report(std::ostream& out, const FoundryCore& core, PopulationId population) {
  const Result<SelectionDecision> decision = core.selection(population);
  if (!decision.ok()) {
    out << "selection: no decision is recorded for population " << population.raw() << "\n";
    return;
  }
  const SelectionDecision& value = decision.value();
  out << "selection\n";
  print_identity(out, "population", value.population.raw());
  print_generation(out, "population_generation", value.population_generation.value());
  print_generation(out, "selection_generation", value.generation.value());
  out << "  state: " << selection_decision_state_name(value.state) << "\n";
  print_identity(out, "task", value.task.raw());
  print_generation(out, "task_generation", value.task_generation.value());
  print_identity(out, "policy", value.policy.raw());
  print_generation(out, "policy_generation", value.policy_generation.value());
  out << "  coordinator_epoch: " << value.coordinator_epoch.value() << "\n";
  print_identity(out, "winner", value.selected.raw());
  print_generation(out, "winner_generation", value.selected_generation.value());
  out << "  canonical_state_digest: " << value.canonical_state_digest << "\n";
  if (!value.rationale.empty()) {
    out << "  rationale: " << value.rationale << "\n";
  }
  out << "  ranking: " << value.ranking.size() << "\n";
  for (const RankingEntry& entry : value.ranking) {
    out << "    - rank=" << entry.rank << " candidate=" << entry.candidate.raw()
        << " total_score=" << entry.total_score << " artifact_bytes=" << entry.artifact_bytes
        << " lineage_depth=" << entry.lineage_depth;
    if (!entry.diversity_key.empty()) {
      out << " diversity=" << entry.diversity_key;
    }
    out << "\n";
    for (const RankingFactorValue& factor : entry.factors) {
      out << "        factor=" << factor.key << " kind=" << ranking_factor_name(factor.kind)
          << " weight=" << factor.weight << " raw=" << factor.raw_value
          << " available=" << yes_no(factor.available)
          << " contribution=" << factor.contribution << "\n";
    }
  }
  out << "  excluded: " << value.excluded.size() << "\n";
  for (const ExclusionEntry& entry : value.excluded) {
    out << "    - candidate=" << entry.candidate.raw()
        << " stage=" << selection_stage_name(entry.stage)
        << " reason=" << exclusion_reason_name(entry.reason);
    if (!entry.detail.empty()) {
      out << " detail=" << entry.detail;
    }
    out << "\n";
  }
}

void print_lineage_report(std::ostream& out, const FoundryCore& core, CandidateId root) {
  std::vector<LineageNode> nodes = core.lineage_nodes();
  if (root.valid()) {
    const Result<CandidateRecord> candidate = core.candidate(root);
    if (!candidate.ok()) {
      out << "lineage: " << candidate.status().to_string() << "\n";
      return;
    }
    std::vector<CandidateId> path;
    CandidateId cursor = root;
    for (std::size_t guard = 0; guard <= kMaxLineageDepth; ++guard) {
      path.push_back(cursor);
      const auto node = std::find_if(nodes.begin(), nodes.end(),
                                     [cursor](const LineageNode& value) {
                                       return value.candidate == cursor;
                                     });
      if (node == nodes.end() || node->parents.empty()) {
        break;
      }
      cursor = node->parents.front();
    }
    std::reverse(path.begin(), path.end());
    out << "lineage (root first)\n";
    for (const CandidateId id : path) {
      const auto node = std::find_if(nodes.begin(), nodes.end(),
                                     [id](const LineageNode& value) {
                                       return value.candidate == id;
                                     });
      if (node == nodes.end()) {
        out << "  candidate=" << id.raw() << " (no lineage node)\n";
        continue;
      }
      out << "  candidate=" << node->candidate.raw() << " depth=" << node->depth
          << " lineage=" << node->lineage.raw()
          << " retired=" << yes_no(node->retired) << "\n";
    }
    return;
  }
  out << "lineage\n";
  out << "  nodes: " << nodes.size() << "\n";
  std::sort(nodes.begin(), nodes.end(), [](const LineageNode& a, const LineageNode& b) {
    return a.candidate < b.candidate;
  });
  for (const LineageNode& node : nodes) {
    out << "  candidate=" << node.candidate.raw() << " depth=" << node.depth
        << " lineage=" << node.lineage.raw() << " retired=" << yes_no(node.retired)
        << " parents:";
    if (node.parents.empty()) {
      out << " none";
    }
    for (const CandidateId parent : node.parents) {
      out << " " << parent.raw();
    }
    out << "\n";
  }
}

// ---------------------------------------------------------------------------
// Offline source
// ---------------------------------------------------------------------------

namespace {

/// Read one identity operand, refusing an absent or malformed one.
[[nodiscard]] Status identity_operand(const Options& options, std::size_t index,
                                      std::string_view name, std::uint64_t* out) {
  if (index >= options.operands.size()) {
    return Status(ErrorCode::InvalidArgument, std::string(name) + " requires a raw identity");
  }
  return parse_identity_operand(options.operands[index], out);
}

[[nodiscard]] int run_offline_command(const Options& options) {
  Result<std::unique_ptr<FoundryCore>> adopted = adopt_snapshot(options);
  if (!adopted.ok()) {
    return report_rejected("offline snapshot", adopted.status());
  }
  const std::unique_ptr<FoundryCore>& core = adopted.value();
  std::uint64_t raw = 0;

  if (options.command == "list-populations") {
    const std::vector<PopulationId> ids = core->population_ids();
    std::cout << "populations: " << ids.size() << "\n";
    for (const PopulationId id : ids) {
      const PopulationRecord record = core->population(id).value();
      std::cout << "  population=" << id.raw() << " generation=" << record.generation.value()
                << " name=" << record.name << " state=" << population_state_name(record.state)
                << " candidates=" << record.candidates.size()
                << " selection=" << generation_text(record.committed_selection.value())
                << " retention=" << yes_no(record.retention_committed) << "\n";
    }
    if (ids.empty()) {
      // An empty answer is an answer, not a failure: the snapshot is readable
      // and it holds no populations.
      std::cout << "  (the snapshot holds no populations)\n";
    }
    return kExitOk;
  }
  if (options.command == "snapshot") {
    const SnapshotStore store(options.state_path);
    Result<FoundrySnapshot> loaded = store.load();
    if (!loaded.ok()) {
      return report_rejected("snapshot", loaded.status());
    }
    const FoundrySnapshot& snapshot = loaded.value();
    std::cout << "snapshot\n";
    std::cout << "  path: " << path_to_utf8(options.state_path) << "\n";
    std::cout << "  foundry: " << snapshot.foundry.raw() << "\n";
    std::cout << "  run: " << snapshot.run.raw() << "\n";
    std::cout << "  epoch: " << snapshot.epoch.value() << "\n";
    std::cout << "  revision: " << snapshot.revision << "\n";
    std::cout << "  tasks: " << snapshot.tasks.size() << "\n";
    std::cout << "  policies: " << snapshot.policies.size() << "\n";
    std::cout << "  populations: " << snapshot.populations.size() << "\n";
    std::cout << "  candidates: " << snapshot.candidates.size() << "\n";
    std::cout << "  workers: " << snapshot.workers.size() << "\n";
    std::cout << "  attempts: " << snapshot.attempts.size() << "\n";
    std::cout << "  assignments: " << snapshot.assignments.size() << "\n";
    std::cout << "  evaluations: " << snapshot.evaluations.size() << "\n";
    std::cout << "  selections: " << snapshot.selections.size() << "\n";
    std::cout << "  retentions: " << snapshot.retentions.size() << "\n";
    std::cout << "  promotion_requests: " << snapshot.promotion_requests.size() << "\n";
    std::cout << "  lineage_nodes: " << snapshot.lineage.size() << "\n";
    std::cout << "  candidates_published: " << snapshot.statistics.candidates_published << "\n";
    std::cout << "  selections_committed: " << snapshot.statistics.selections_committed << "\n";
    std::cout << "  retentions_committed: " << snapshot.statistics.retentions_committed << "\n";
    std::cout << "  stale_authority_rejections: "
              << snapshot.statistics.stale_authority_rejections << "\n";
    return kExitOk;
  }
  if (options.command == "check") {
    // The snapshot has already been parsed and semantically validated by load(),
    // so reaching this point is the integrity verdict. What remains is the
    // foundry's own self-consistency audit of the state it describes.
    const SnapshotStore store(options.state_path);
    Result<FoundrySnapshot> loaded = store.load();
    if (!loaded.ok()) {
      std::cout << "snapshot_check=FAILED reason=" << loaded.status().to_string() << "\n";
      return kExitRejected;
    }
    const std::vector<std::string> violations = core->audit();
    std::cout << "snapshot_check=OK path=" << path_to_utf8(options.state_path) << "\n";
    std::cout << "audit_violations=" << violations.size() << "\n";
    for (const std::string& violation : violations) {
      std::cout << "  - " << violation << "\n";
    }
    return violations.empty() ? kExitOk : kExitRejected;
  }
  if (options.command == "show-population") {
    AF_CLI_TRY(identity_operand(options, 0, "show-population", &raw));
    Result<PopulationRecord> record = core->population(StrongId<PopulationIdTag>::from_raw(raw));
    if (!record.ok()) {
      return report_rejected("show-population", record.status());
    }
    print_population_report(std::cout, *core, record.value());
    return kExitOk;
  }
  if (options.command == "show-candidate") {
    AF_CLI_TRY(identity_operand(options, 0, "show-candidate", &raw));
    Result<CandidateRecord> record = core->candidate(StrongId<CandidateIdTag>::from_raw(raw));
    if (!record.ok()) {
      return report_rejected("show-candidate", record.status());
    }
    print_candidate_report(std::cout, *core, record.value());
    return kExitOk;
  }
  if (options.command == "show-evaluations") {
    AF_CLI_TRY(identity_operand(options, 0, "show-evaluations", &raw));
    print_evaluation_report(std::cout, *core, StrongId<CandidateIdTag>::from_raw(raw));
    return kExitOk;
  }
  if (options.command == "show-retained") {
    AF_CLI_TRY(identity_operand(options, 0, "show-retained", &raw));
    Result<PopulationRecord> record = core->population(StrongId<PopulationIdTag>::from_raw(raw));
    if (!record.ok()) {
      return report_rejected("show-retained", record.status());
    }
    if (!record.value().retention_committed) {
      return report_rejected("show-retained",
                             Status(ErrorCode::NotReady,
                                    "population " + std::to_string(raw) +
                                        " has no committed retention decision"));
    }
    print_retention_report(std::cout, *core, record.value().id);
    return kExitOk;
  }
  if (options.command == "explain-selection") {
    AF_CLI_TRY(identity_operand(options, 0, "explain-selection", &raw));
    Result<PopulationRecord> record = core->population(StrongId<PopulationIdTag>::from_raw(raw));
    if (!record.ok()) {
      return report_rejected("explain-selection", record.status());
    }
    if (!record.value().committed_selection.valid()) {
      return report_rejected("explain-selection",
                             Status(ErrorCode::NotReady,
                                    "population " + std::to_string(raw) +
                                        " has no committed selection decision"));
    }
    print_selection_report(std::cout, *core, record.value().id);
    return kExitOk;
  }
  if (options.command == "show-lineage") {
    CandidateId root;
    if (!options.operands.empty()) {
      AF_CLI_TRY(identity_operand(options, 0, "show-lineage", &raw));
      root = StrongId<CandidateIdTag>::from_raw(raw);
    }
    print_lineage_report(std::cout, *core, root);
    return kExitOk;
  }
  return report_usage("command '" + options.command + "' is not an offline command");
}

// ---------------------------------------------------------------------------
// Live source
// ---------------------------------------------------------------------------

/// One controller connection to a running coordinator.
///
/// The reader thread hands whole frames to a queue; every request waits for the
/// next frame that is not a heartbeat acknowledgement. The CLI keeps exactly one
/// request outstanding at a time, so a single queue is enough and no request
/// identifier is needed to match a reply.
class ControllerLink {
 public:
  ControllerLink() = default;
  ControllerLink(const ControllerLink&) = delete;
  ControllerLink& operator=(const ControllerLink&) = delete;
  ~ControllerLink() { close(); }

  Status connect(const Endpoint& endpoint) {
    AF_TRY(SocketSubsystem::ensure_initialized());
    SocketHandle socket;
    AF_TRY_ASSIGN(socket, connect_to(endpoint));
    ConnectionCallbacks callbacks;
    callbacks.on_frame = [this](const Frame& frame) { enqueue(frame); };
    callbacks.on_closed = [this](const Status& reason) { mark_down(reason); };
    std::shared_ptr<Connection> created =
        std::make_shared<Connection>(std::move(socket), endpoint.to_string(), std::move(callbacks));
    AF_TRY(created->start());
    connection_ = std::move(created);

    HelloMessage hello;
    hello.role = SessionRole::Controller;
    hello.run = FoundryRunId();
    hello.controller = ControllerId();
    hello.label = std::string(kCliControllerLabel);
    hello.capability = "operator-console";
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(hello));
    AF_TRY(connection_->send(MessageType::Hello, std::move(payload)));

    Frame frame;
    AF_TRY(next_frame(&frame));
    if (frame.header.type != MessageType::HelloAck) {
      if (frame.header.type == MessageType::ErrorResponse) {
        ErrorResponseMessage error;
        AF_TRY_ASSIGN(error, decode_error_response(frame.payload));
        return Status(error.code, "the coordinator refused the session: " + error.detail);
      }
      return Status(ErrorCode::ProtocolViolation,
                    "expected a session acknowledgement but received '" +
                        std::string(message_type_name(frame.header.type)) + "'");
    }
    HelloAckMessage ack;
    AF_TRY_ASSIGN(ack, decode_hello_ack(frame.payload));
    epoch_ = ack.coordinator_epoch;
    run_ = ack.run;
    session_.coordinator_epoch = ack.coordinator_epoch;
    session_.run = ack.run;
    session_.session = ack.session;
    session_.session_generation = ack.session_generation;
    return Status();
  }

  /// Send one request and return the reply frame.
  template <typename Request>
  [[nodiscard]] Result<Frame> request(MessageType type, const Request& message) {
    std::string payload;
    AF_TRY_ASSIGN(payload, encode_payload(message));
    AF_TRY(connection_->send(type, std::move(payload)));
    Frame frame;
    AF_TRY(next_frame(&frame));
    if (frame.header.type == MessageType::ErrorResponse) {
      ErrorResponseMessage error;
      AF_TRY_ASSIGN(error, decode_error_response(frame.payload));
      return Status(error.code, error.detail);
    }
    return frame;
  }

  [[nodiscard]] CoordinatorEpoch epoch() const { return epoch_; }
  [[nodiscard]] FoundryRunId run() const { return run_; }
  [[nodiscard]] const ControllerSessionAuthority& session() const { return session_; }

  void close() {
    if (connection_) {
      connection_->request_close(Status(ErrorCode::ConnectionClosed, "cli is finished"));
      connection_.reset();
    }
  }

 private:
  void enqueue(Frame frame) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      frames_.push_back(std::move(frame));
    }
    wake_.notify_all();
  }

  void mark_down(const Status& reason) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      down_ = true;
      reason_ = reason;
    }
    wake_.notify_all();
  }

  [[nodiscard]] Status next_frame(Frame* frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [this] { return down_ || !frames_.empty(); });
    if (!frames_.empty()) {
      *frame = std::move(frames_.front());
      frames_.pop_front();
      return Status();
    }
    return Status(ErrorCode::ConnectionClosed,
                  reason_.ok() ? std::string("the coordinator closed the connection")
                               : reason_.to_string());
  }

  std::shared_ptr<Connection> connection_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Frame> frames_;
  bool down_{false};
  Status reason_;
  CoordinatorEpoch epoch_;
  FoundryRunId run_;
  ControllerSessionAuthority session_;
};

[[nodiscard]] int run_live_command(const Options& options) {
  const Result<Endpoint> endpoint = parse_endpoint(options.coordinator);
  if (!endpoint.ok()) {
    return report_usage(endpoint.status().message());
  }
  ControllerLink link;
  const Status connected = link.connect(endpoint.value());
  if (!connected.ok()) {
    return report_rejected("connecting to " + options.coordinator, connected);
  }
  std::cout << "coordinator\n";
  std::cout << "  endpoint: " << endpoint.value().to_string() << "\n";
  std::cout << "  epoch: " << link.epoch().value() << "\n";
  std::cout << "  run: " << link.run().raw() << "\n";

  std::uint64_t raw = 0;

  if (options.command == "list-populations") {
    // The protocol exposes no "enumerate every population" query, so a live
    // listing cannot be produced. Saying so is the honest answer; inventing a
    // list from incomplete information would not be.
    return report_rejected("list-populations",
                           Status(ErrorCode::Unsupported,
                                  "the control plane has no enumerate-populations query; use "
                                  "--state with an offline snapshot to list every population"));
  }
  if (options.command == "check") {
    QueryAuditMessage query;
    query.session = link.session();
    Result<Frame> reply = link.request(MessageType::QueryAudit, query);
    if (!reply.ok()) {
      return report_rejected("check", reply.status());
    }
    AuditDetailMessage detail;
    AF_CLI_ASSIGN(detail, decode_audit_detail(reply.value().payload));
    std::cout << "audit_violations=" << detail.violations.size() << "\n";
    for (const std::string& violation : detail.violations) {
      std::cout << "  - " << violation << "\n";
    }
    return detail.violations.empty() ? kExitOk : kExitRejected;
  }
  if (options.command == "show-population") {
    AF_CLI_TRY(identity_operand(options, 0, "show-population", &raw));
    QueryPopulationMessage query;
    query.session = link.session();
    query.population = StrongId<PopulationIdTag>::from_raw(raw);
    Result<Frame> reply = link.request(MessageType::QueryPopulation, query);
    if (!reply.ok()) {
      return report_rejected("show-population", reply.status());
    }
    PopulationDetailMessage detail;
    AF_CLI_ASSIGN(detail, decode_population_detail(reply.value().payload));
    std::cout << "population\n";
    print_identity(std::cout, "id", detail.population.id.raw());
    print_generation(std::cout, "generation", detail.population.generation.value());
    std::cout << "  name: " << detail.population.name << "\n";
    std::cout << "  state: " << population_state_name(detail.population.state) << "\n";
    std::cout << "  candidate_count: " << detail.candidate_count << "\n";
    print_generation(std::cout, "committed_selection",
                     detail.committed_selection_generation);
    std::cout << "  retention_committed: " << yes_no(detail.retention_committed) << "\n";
    std::cout << "  closure_blockers: " << detail.closure_blockers.size() << "\n";
    for (const std::string& blocker : detail.closure_blockers) {
      std::cout << "    - " << blocker << "\n";
    }
    std::cout << "  candidates:";
    if (detail.population.candidates.empty()) {
      std::cout << " none";
    }
    for (const CandidateId candidate : detail.population.candidates) {
      std::cout << " " << candidate.raw();
    }
    std::cout << "\n";
    return kExitOk;
  }
  if (options.command == "show-candidate" || options.command == "show-evaluations") {
    AF_CLI_TRY(identity_operand(options, 0, options.command.c_str(), &raw));
    QueryCandidateMessage query;
    query.session = link.session();
    query.candidate = StrongId<CandidateIdTag>::from_raw(raw);
    Result<Frame> reply = link.request(MessageType::QueryCandidate, query);
    if (!reply.ok()) {
      return report_rejected(options.command, reply.status());
    }
    CandidateDetailMessage detail;
    AF_CLI_ASSIGN(detail, decode_candidate_detail(reply.value().payload));
    if (options.command == "show-evaluations") {
      std::cout << "evidence\n";
      print_identity(std::cout, "candidate", detail.candidate.id.raw());
      print_generation(std::cout, "candidate_generation", detail.candidate.generation.value());
      std::cout << "  records: " << detail.evaluations.size() << "\n";
      for (const EvaluationRecord& record : detail.evaluations) {
        print_evidence(std::cout, record);
      }
      return kExitOk;
    }
    std::cout << "candidate\n";
    print_identity(std::cout, "id", detail.candidate.id.raw());
    print_generation(std::cout, "generation", detail.candidate.generation.value());
    std::cout << "  state: " << candidate_state_name(detail.candidate.state) << "\n";
    print_identity(std::cout, "population", detail.candidate.population.raw());
    print_identity(std::cout, "task", detail.candidate.task.raw());
    print_identity(std::cout, "lineage", detail.candidate.lineage.raw());
    std::cout << "  depth: " << detail.candidate.depth << "\n";
    std::cout << "  selected: " << yes_no(detail.candidate.selected) << "\n";
    std::cout << "  retained: " << yes_no(detail.candidate.retained) << "\n";
    std::cout << "  promotion_eligible: " << yes_no(detail.candidate.promotion_eligible) << "\n";
    std::cout << "  promotion_requested: " << yes_no(detail.candidate.promotion_requested) << "\n";
    std::cout << "  artifacts: " << detail.candidate.artifacts.size() << "\n";
    for (const ArtifactRef& artifact : detail.candidate.artifacts) {
      std::cout << "    - " << artifact.name << " bytes=" << artifact.size_bytes
                << " digest=" << artifact.content_digest << "\n";
    }
    std::cout << "  evidence_records: " << detail.evaluations.size() << "\n";
    for (const EvaluationRecord& record : detail.evaluations) {
      print_evidence(std::cout, record);
    }
    return kExitOk;
  }
  if (options.command == "show-lineage") {
    AF_CLI_TRY(identity_operand(options, 0, "show-lineage", &raw));
    QueryLineageMessage query;
    query.session = link.session();
    query.root = StrongId<CandidateIdTag>::from_raw(raw);
    Result<Frame> reply = link.request(MessageType::QueryLineage, query);
    if (!reply.ok()) {
      return report_rejected("show-lineage", reply.status());
    }
    LineageDetailMessage detail;
    AF_CLI_ASSIGN(detail, decode_lineage_detail(reply.value().payload));
    std::cout << "lineage (root first)\n";
    for (const LineageNode& node : detail.nodes) {
      std::cout << "  candidate=" << node.candidate.raw() << " depth=" << node.depth
                << " lineage=" << node.lineage.raw() << " retired=" << yes_no(node.retired)
                << "\n";
    }
    return kExitOk;
  }
  if (options.command == "show-retained" || options.command == "explain-selection") {
    // Retention and selection decisions are durable records, and this protocol
    // version exposes them only through the snapshot, not through a query. The
    // live answer therefore states what the live state does carry - who won and
    // whether the population is retained - and points at the snapshot for the
    // decision record itself.
    AF_CLI_TRY(identity_operand(options, 0, options.command.c_str(), &raw));
    QueryPopulationMessage query;
    query.session = link.session();
    query.population = StrongId<PopulationIdTag>::from_raw(raw);
    Result<Frame> reply = link.request(MessageType::QueryPopulation, query);
    if (!reply.ok()) {
      return report_rejected(options.command, reply.status());
    }
    PopulationDetailMessage detail;
    AF_CLI_ASSIGN(detail, decode_population_detail(reply.value().payload));
    std::cout << (options.command == "show-retained" ? "retention\n" : "selection\n");
    print_identity(std::cout, "population", detail.population.id.raw());
    print_generation(std::cout, "committed_selection", detail.committed_selection_generation);
    std::cout << "  retention_committed: " << yes_no(detail.retention_committed) << "\n";
    std::cout << "  candidates:\n";
    for (const CandidateId candidate : detail.population.candidates) {
      QueryCandidateMessage candidate_query;
      candidate_query.session = link.session();
      candidate_query.candidate = candidate;
      Result<Frame> candidate_reply =
          link.request(MessageType::QueryCandidate, candidate_query);
      if (!candidate_reply.ok()) {
        continue;
      }
      CandidateDetailMessage candidate_detail;
      AF_CLI_ASSIGN(candidate_detail,
                    decode_candidate_detail(candidate_reply.value().payload));
      std::cout << "    - candidate=" << candidate.raw()
                << " state=" << candidate_state_name(candidate_detail.candidate.state)
                << " selected=" << yes_no(candidate_detail.candidate.selected)
                << " retained=" << yes_no(candidate_detail.candidate.retained) << "\n";
    }
    std::cout << "  note: the full decision record (ranking, exclusions and retention "
                 "outcomes) is durable state; read it with --state against the snapshot\n";
    return kExitOk;
  }
  return report_usage("command '" + options.command + "' is not a live command");
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

int run_offline(const Options& options) {
  if (!command_is_known(options.command)) {
    return report_usage("unknown command '" + options.command + "'");
  }
  try {
    return run_offline_command(options);
  } catch (const std::exception& error) {
    return report_rejected("offline command", Status(ErrorCode::Internal, error.what()));
  }
}

int run_live(const Options& options) {
  if (!command_is_known(options.command)) {
    return report_usage("unknown command '" + options.command + "'");
  }
  try {
    return run_live_command(options);
  } catch (const std::exception& error) {
    return report_rejected("live command", Status(ErrorCode::Internal, error.what()));
  }
}

namespace {

/// Identity of a named evaluator. The same stable derivation the coordinator
/// uses, so a CLI-produced record and a coordinator-produced record name the
/// same evaluator the same way.
[[nodiscard]] EvaluatorId demo_evaluator_identity(std::string_view key, std::uint32_t salt) {
  const std::uint64_t mixed =
      fnv1a64(key) ^ (static_cast<std::uint64_t>(salt) * 0x9E3779B97F4A7C15ull);
  std::uint64_t counter = (mixed >> 40) & detail::kIdCounterMask;
  if (counter == 0) {
    counter = 1;
  }
  return StrongId<EvaluatorIdTag>::from_raw(
      (static_cast<std::uint64_t>(IdKind::Evaluator) << detail::kIdKindShift) |
      ((static_cast<std::uint64_t>(salt) & detail::kIdSaltMask) << detail::kIdSaltShift) | counter);
}

struct DemoWorker {
  ReferenceStrategy strategy{ReferenceStrategy::ClosedForm};
  WorkerId worker;
  WorkerSessionAuthority session;
  std::string source;
  ArtifactRef artifact;
  CandidateId candidate;
  bool published{false};
};

}  // namespace

int run_reference_demo(const Options& options) {
  (void)options;
  const std::filesystem::path root = demo_root();
  const Status removed = remove_tree_bounded(root);
  if (!removed.ok()) {
    return report_rejected("reference demo", removed.with_context("clearing the demo root"));
  }
  std::error_code error;
  std::filesystem::create_directories(root / "workspace", error);
  if (error) {
    return report_rejected(
        "reference demo",
        Status(ErrorCode::IoFailure, "demo root could not be created: " + error.message()));
  }

  const auto finish = [&root](int code) -> int {
    // The demo owns everything it created, so it removes all of it, including
    // on the failure paths.
    (void)remove_tree_bounded(root);
    return code;
  };

  FoundryConfig config;
  config.foundry = FoundryId();
  config.run = make_foundry_run_id();
  config.epoch = CoordinatorEpoch::from_value(1);
  config.id_salt = make_id_salt();
  config.workspace_root = root / "workspace";
  {
    IdAllocator identity(config.id_salt);
    FoundryId foundry;
    const Result<FoundryId> minted = identity.next<FoundryIdTag>();
    if (!minted.ok()) {
      return finish(report_rejected("reference demo", minted.status()));
    }
    foundry = minted.value();
    config.foundry = foundry;
  }

  FoundryCore core(config);

  // The policy identity is allocated by the core, so the demo never invents an
  // identity the runtime would not have issued itself.
  const FoundryPolicy policy_definition = make_reference_policy(PolicyId(), PolicyGeneration());
  PolicyId policy;
  {
    const Result<PolicyId> defined = core.define_policy(policy_definition);
    if (!defined.ok()) {
      return finish(report_rejected("reference demo", defined.status()));
    }
    policy = defined.value();
  }

  TaskId task_identity;
  {
    IdAllocator allocator(config.id_salt);
    const Result<TaskId> minted = allocator.next<TaskIdTag>();
    if (!minted.ok()) {
      return finish(report_rejected("reference demo", minted.status()));
    }
    task_identity = minted.value();
  }

  const std::uint32_t candidate_budget = 4;
  Result<TaskSpec> reference_task = make_reference_task(task_identity, policy,
                                                        PolicyGeneration::first(), candidate_budget,
                                                        config.default_budgets);
  if (!reference_task.ok()) {
    return finish(report_rejected("reference demo", reference_task.status()));
  }
  TaskId task;
  {
    const Result<TaskId> defined = core.define_task(reference_task.value());
    if (!defined.ok()) {
      return finish(report_rejected("reference demo", defined.status()));
    }
    task = defined.value();
  }

  PopulationSpec spec;
  spec.name = "reference-population";
  spec.task = task;
  spec.task_generation = TaskGeneration::first();
  spec.policy = policy;
  spec.policy_generation = PolicyGeneration::first();
  spec.candidate_budget = candidate_budget;
  spec.worker_budget = candidate_budget;
  spec.population_index = 1;
  spec.require_promotion_request = false;

  PopulationId population;
  {
    const Result<PopulationId> created = core.create_population(spec);
    if (!created.ok()) {
      return finish(report_rejected("reference demo", created.status()));
    }
    population = created.value();
  }
  {
    const Status started = core.start_population(population, PopulationGeneration::first());
    if (!started.ok()) {
      return finish(report_rejected("reference demo", started));
    }
  }

  const std::vector<ReferenceStrategy> strategies{ReferenceStrategy::ClosedForm,
                                                  ReferenceStrategy::Iterative,
                                                  ReferenceStrategy::OffByOne,
                                                  ReferenceStrategy::SelfReportedPass};
  IdAllocator worker_identities(make_id_salt());
  std::vector<DemoWorker> workers;
  for (const ReferenceStrategy strategy : strategies) {
    DemoWorker worker;
    worker.strategy = strategy;
    const Result<WorkerId> identity = worker_identities.next<WorkerIdTag>();
    if (!identity.ok()) {
      return finish(report_rejected("reference demo", identity.status()));
    }
    worker.worker = identity.value();

    WorkerRegistrationRequest request;
    request.worker = worker.worker;
    request.boot = make_worker_boot_id();
    request.label = "demo-" + std::string(reference_strategy_name(strategy));
    request.capability = "reference-cpp20";
    request.process_id = 0;
    const Result<WorkerSessionAuthority> session = core.register_worker(request);
    if (!session.ok()) {
      return finish(report_rejected("reference demo", session.status()));
    }
    worker.session = session.value();
    const Status ready = core.worker_ready(worker.session, "reference-cpp20");
    if (!ready.ok()) {
      return finish(report_rejected("reference demo", ready));
    }
    workers.push_back(std::move(worker));
  }

  // One attempt per worker, in deterministic order: the strategy that produces
  // the correct answer is not special-cased anywhere, and the foundry discovers
  // which candidate is correct from execution and evaluation alone.
  for (DemoWorker& worker : workers) {
    const Result<PendingDispatch> pending = core.authorize_attempt(worker.session);
    if (!pending.ok()) {
      std::cout << "  worker=" << worker.worker.raw()
                << " strategy=" << reference_strategy_name(worker.strategy)
                << " no dispatchable work: " << pending.status().message() << "\n";
      continue;
    }
    const Result<AttemptPackage> package =
        core.confirm_dispatch(worker.session, pending.value().attempt.id,
                              pending.value().attempt.generation);
    if (!package.ok()) {
      return finish(report_rejected("reference demo", package.status()));
    }
    const AttemptPackage& dispatched = package.value();

    WorkerOperationAuthority authority;
    authority.session = worker.session;
    authority.population = dispatched.population;
    authority.population_generation = dispatched.population_generation;
    authority.task = dispatched.task;
    authority.task_generation = dispatched.task_generation;
    authority.candidate = dispatched.candidate;
    authority.candidate_generation = dispatched.candidate_generation;
    authority.attempt = dispatched.attempt;
    authority.attempt_generation = dispatched.attempt_generation;
    authority.assignment = dispatched.assignment;

    const Status acknowledged = core.acknowledge_attempt(authority);
    if (!acknowledged.ok()) {
      return finish(report_rejected("reference demo", acknowledged));
    }

    worker.source = generate_reference_solution(worker.strategy);
    worker.artifact.name = std::string(kReferenceTaskSourceArtifact);
    worker.artifact.size_bytes = static_cast<std::uint64_t>(worker.source.size());
    worker.artifact.content_digest = sha256_hex(worker.source);

    std::vector<ArtifactRef> artifacts{worker.artifact};
    const Result<PublicationOutcome> outcome =
        core.publish_candidate(authority, std::move(artifacts),
                               std::string(reference_strategy_name(worker.strategy)));
    if (!outcome.ok()) {
      return finish(report_rejected("reference demo", outcome.status()));
    }
    worker.candidate = outcome.value().candidate;
    worker.published = true;
  }

  // Evaluation. A toolchain that is not present is not a failure of the demo:
  // the evaluator reports UNSUPPORTED, the mandatory gate stays unsatisfied, and
  // the demo says so instead of inventing a winner.
  const Result<ReferenceToolchain> resolved = resolve_reference_toolchain();
  const bool toolchain_available = resolved.ok();
  const ReferenceToolchain toolchain = toolchain_available ? resolved.value() : ReferenceToolchain();
  const EvaluatorRegistry evaluators = EvaluatorRegistry::make_reference();
  std::uint64_t scratch_index = 0;

  // Starting the population advanced its generation, so every evaluation record
  // and every evaluation request must be bound to the generation the population
  // holds now rather than to a generation this program assumed.
  const PopulationGeneration demo_generation = core.population(population).value().generation;

  for (const DemoWorker& worker : workers) {
    if (!worker.published) {
      continue;
    }
    const Status begun = core.begin_evaluation(worker.candidate);
    if (!begun.ok()) {
      return finish(report_rejected("reference demo", begun));
    }
    const CandidateRecord candidate = core.candidate(worker.candidate).value();
    const TaskSpec task_spec = core.task(task).value();
    for (const EvaluationRequirement& requirement : task_spec.requirements) {
      EvaluationRecord record;
      record.candidate = candidate.id;
      record.candidate_generation = candidate.generation;
      record.task = task_spec.id;
      record.task_generation = task_spec.generation;
      record.population = population;
      record.population_generation = demo_generation;
      record.evaluator = demo_evaluator_identity(requirement.evaluator_key, config.id_salt);
      record.evaluator_key = requirement.evaluator_key;
      record.requirement_class = requirement.requirement_class;
      record.kind = EvaluatorKind::PortableReference;
      record.complete = true;
      record.decided_epoch = core.epoch();

      const std::shared_ptr<Evaluator> evaluator = evaluators.find(requirement.evaluator_key);
      if (!evaluator) {
        record.outcome = EvaluationOutcome::Unsupported;
        record.diagnostics = "no evaluator is registered under key '" + requirement.evaluator_key +
                             "' in this deployment";
      } else if (!toolchain_available) {
        record.kind = evaluator->kind();
        record.outcome = EvaluationOutcome::Unsupported;
        record.diagnostics =
            "no C++ toolchain resolved, so compiler-backed evaluation is UNSUPPORTED in this "
            "environment; this is reported honestly and is never treated as a pass";
      } else {
        record.kind = evaluator->kind();
        EvaluationRequest request;
        request.candidate = candidate.id;
        request.candidate_generation = candidate.generation;
        request.task = task_spec.id;
        request.task_generation = task_spec.generation;
        request.population = population;
        request.population_generation = demo_generation;
        request.artifacts.emplace(std::string(kReferenceTaskSourceArtifact), worker.source);
        for (const InputFile& input : task_spec.inputs) {
          request.inputs.emplace(input.name, input.content);
        }
        request.evaluator_key = requirement.evaluator_key;
        request.requirement_class = requirement.requirement_class;
        request.toolchain = toolchain;
        const std::filesystem::path scratch =
            root / "scratch" / ("candidate-" + std::to_string(scratch_index));
        ++scratch_index;
        std::filesystem::create_directories(scratch, error);
        request.scratch_directory = scratch;
        try {
          const EvaluationResult result = evaluator->evaluate(request);
          record.outcome = result.outcome;
          record.has_score = result.has_score;
          record.score = result.score;
          record.diagnostics = result.diagnostics;
          record.evidence_digest = result.evidence_digest;
          record.duration_micros = result.duration_micros;
        } catch (const std::exception& failure) {
          record.outcome = EvaluationOutcome::Error;
          record.diagnostics = std::string("evaluator raised: ") + failure.what();
        }
        (void)remove_tree_bounded(scratch);
      }
      const Status recorded = core.record_evaluation(std::move(record));
      if (!recorded.ok()) {
        return finish(report_rejected("reference demo", recorded));
      }
    }
  }

  std::cout << "reference_demo\n";
  std::cout << "  toolchain: " << (toolchain_available ? toolchain.describe()
                                                       : std::string("UNSUPPORTED"))
            << "\n";
  std::cout << "  population: " << population.raw() << "\n";
  std::cout << "  task: " << task.raw() << "\n";
  std::cout << "  policy: " << policy.raw() << "\n";
  std::cout << "  candidates:\n";
  for (const DemoWorker& worker : workers) {
    if (!worker.published) {
      std::cout << "    - strategy=" << reference_strategy_name(worker.strategy)
                << " no candidate was produced\n";
      continue;
    }
    const CandidateRecord record = core.candidate(worker.candidate).value();
    std::cout << "    - strategy=" << reference_strategy_name(worker.strategy)
              << " candidate=" << worker.candidate.raw()
              << " state=" << candidate_state_name(record.state)
              << " artifacts=" << record.artifacts.size() << "\n";
  }

  // Selection and retention are authoritative transitions, and they are
  // prepared then committed exactly as the control plane does it.
  const Result<SelectionDecision> prepared = core.prepare_selection(population);
  if (!prepared.ok()) {
    return finish(report_rejected("reference demo: selection could not be prepared",
                                  prepared.status()));
  }
  const Result<SelectionDecision> committed = core.commit_selection(prepared.value());
  if (!committed.ok()) {
    return finish(report_rejected("reference demo: selection was rejected", committed.status()));
  }
  const Result<RetentionDecision> retention = core.prepare_retention(population);
  if (!retention.ok()) {
    return finish(report_rejected("reference demo: retention could not be prepared",
                                  retention.status()));
  }
  const Result<RetentionDecision> retained = core.commit_retention(retention.value());
  if (!retained.ok()) {
    return finish(report_rejected("reference demo: retention was rejected", retained.status()));
  }

  print_selection_report(std::cout, core, population);
  print_retention_report(std::cout, core, population);

  if (committed.value().has_winner()) {
    const CandidateRecord winner = core.candidate(committed.value().selected).value();
    std::cout << "winner\n";
    std::cout << "  candidate: " << winner.id.raw() << "\n";
    std::cout << "  state: " << candidate_state_name(winner.state) << "\n";
    std::cout << "  diversity_key: " << winner.diversity_key << "\n";
    std::cout << "  artifact_set_digest: " << winner.artifact_set_digest << "\n";
    std::cout << "  why: the candidate carries a complete authoritative pass for every "
                 "mandatory requirement, and its weighted ranking score is the highest among "
                 "the candidates that survived the gates\n";
  } else {
    std::cout << "winner: none\n";
    std::cout << "  why: no candidate satisfied every mandatory requirement"
              << (toolchain_available ? "" : " (compiler-backed evaluation was UNSUPPORTED here)")
              << "\n";
  }

  const std::vector<std::string> blockers = core.closure_blockers(population);
  std::cout << "closure_blockers: " << blockers.size() << "\n";
  for (const std::string& blocker : blockers) {
    std::cout << "  - " << blocker << "\n";
  }

  const Status closed = core.close_population(population);
  if (!closed.ok()) {
    return finish(report_rejected("reference demo: the population could not close", closed));
  }
  std::cout << "population_state: "
            << population_state_name(core.population(population).value().state) << "\n";
  std::cout << "durable_closed: yes\n";

  // The demo was asked for a state file, so it leaves one behind: a run that
  // exercised the whole decision path is only useful to the offline commands if
  // the durable image it produced is actually on disk.
  if (!options.state_path.empty()) {
    SnapshotStore store(options.state_path);
    const Status written = store.save_and_verify(core.snapshot());
    if (!written.ok()) {
      return finish(report_rejected("reference demo: the durable snapshot could not be written",
                                    written));
    }
    std::cout << "durable_snapshot: " << path_to_utf8(options.state_path) << "\n";
  }

  return finish(kExitOk);
}

}  // namespace cli
}  // namespace autonomous_foundry

int main(int argc, char** argv) {
  using namespace autonomous_foundry;
  using namespace autonomous_foundry::cli;

  Options options;
  const Status parsed = parse_options(argc, argv, &options);
  if (!parsed.ok()) {
    std::cerr << "af_cli: " << parsed.message() << "\n";
    print_usage(std::cerr);
    return kExitUsage;
  }
  if (options.command == "help") {
    print_usage(std::cout);
    return kExitOk;
  }
  if (options.command == "run-reference-demo") {
    return run_reference_demo(options);
  }
  if (!command_is_known(options.command)) {
    std::cerr << "af_cli: unknown command '" << options.command << "'\n";
    print_usage(std::cerr);
    return kExitUsage;
  }
  if (!options.state_path.empty()) {
    return run_offline(options);
  }
  if (!options.coordinator.empty()) {
    return run_live(options);
  }
  std::cerr << "af_cli: either --state <snapshot> or --coordinator <host:port> is required\n";
  print_usage(std::cerr);
  return kExitUsage;
}
