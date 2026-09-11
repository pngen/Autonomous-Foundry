// examples/ex_promotion_handoff.cpp
//
// Promotion handoff.
//
// Autonomous Foundry does not promote artifacts. It determines that a selected
// candidate is eligible to be handed to an Artifact Promotion boundary, emits a
// PromotionRequest with the evidence the receiving system needs to re-verify it
// independently, and records whatever that system answers.
//
// The example:
//
//   1. produces a winner through the real production, evaluation, selection and
//      retention path
//   2. emits a PromotionRequest and prints its evidence, artifacts, ancestry and
//      eligibility
//   3. hands it to a PromotionSink implemented here, a local recorder
//   4. prints the handoff state: Requested, and explicitly not promoted
//   5. revises the task so the winner's task generation is no longer current,
//      emits a second request, and shows the concrete eligibility reason and a
//      sink that declines it
//
// The mandatory gate of this task contract is a deterministic static rule over
// the candidate source, so the example never depends on an installed compiler.
//
// Exit code 0 means both requests were produced, both receipts were recorded,
// and the foundry never claimed a promotion it did not observe.

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
#include "autonomous_foundry/promotion.hpp"
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

std::string number(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return std::string(buffer);
}

std::string join(const std::vector<std::string>& values) {
  if (values.empty()) {
    return "[]";
  }
  std::string text = "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      text.append("; ");
    }
    text.append(values[index]);
  }
  text.push_back(']');
  return text;
}

std::string join_ids(const std::vector<af::CandidateId>& values) {
  if (values.empty()) {
    return "[]";
  }
  std::string text = "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      text.append(", ");
    }
    text.append(values[index].to_string());
  }
  text.push_back(']');
  return text;
}

// ---------------------------------------------------------------------------
// A PromotionSink implemented locally.
//
// The sink owns its own decision. It records the request, echoes the digest it
// was asked about, and answers with a handoff state. It never claims that an
// artifact was promoted, because it has no authority to make that claim either.
// ---------------------------------------------------------------------------

class LocalRecorderSink final : public af::PromotionSink {
 public:
  LocalRecorderSink(std::string name, af::PromotionHandoffState response)
      : name_(std::move(name)), response_(response) {}

  [[nodiscard]] std::string name() const override { return name_; }

  [[nodiscard]] af::Result<af::PromotionReceipt> submit(
      const af::PromotionRequest& request) override {
    ++submissions_;
    last_request_ = request.id;
    last_eligibility_ = request.eligibility;

    af::PromotionReceipt receipt;
    receipt.handoff = response_;
    receipt.sink_name = name_;
    receipt.request_digest = request.canonical_state_digest;
    if (response_ == af::PromotionHandoffState::DeclinedBySink) {
      receipt.detail = "declined; outstanding requirements: " +
                       join(request.outstanding_requirements);
    } else {
      receipt.sink_reference = "local-record-" + std::to_string(request.id.counter());
      receipt.detail = "recorded; the promotion decision stays with " + name_;
    }
    return receipt;
  }

  [[nodiscard]] std::uint64_t submissions() const noexcept { return submissions_; }
  [[nodiscard]] af::PromotionRequestId last_request() const noexcept { return last_request_; }
  [[nodiscard]] af::PromotionEligibilityState last_eligibility() const noexcept {
    return last_eligibility_;
  }

 private:
  std::string name_;
  af::PromotionHandoffState response_{af::PromotionHandoffState::NotRequested};
  std::uint64_t submissions_{0};
  af::PromotionRequestId last_request_;
  af::PromotionEligibilityState last_eligibility_{af::PromotionEligibilityState::NotEvaluated};
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
  task.name = "promotion-reference";
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
  task.closure_criteria.push_back("the selected candidate has a committed promotion request");

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
  if (!report.expect_status(core.acknowledge_attempt(produced.authority),
                            label + ": acknowledge_attempt")) {
    return false;
  }
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
// Presentation
// ---------------------------------------------------------------------------

void print_request(const af::PromotionRequest& request) {
  Report::section("promotion request");
  Report::item("id=" + request.id.to_string() +
               " foundry=" + request.foundry.to_string() +
               " run=" + request.run.to_string() +
               " coordinator_epoch=" + request.coordinator_epoch.to_string());
  Report::item("population=" + request.population.to_string() +
               " population_generation=" + request.population_generation.to_string());
  Report::item("candidate=" + request.candidate.to_string() +
               " candidate_generation=" + request.candidate_generation.to_string());
  Report::item("task=" + request.task.to_string() +
               " task_generation=" + request.task_generation.to_string() +
               " policy_generation=" + request.policy_generation.to_string());
  Report::item("lineage=" + request.lineage.to_string() +
               " depth=" + std::to_string(request.lineage_depth) +
               " selection_generation=" + request.selection_generation.to_string() +
               " evidence_generation=" + request.evidence_generation.to_string());
  Report::item("artifacts=" + std::to_string(request.artifacts.size()) +
               " total_artifact_bytes=" + std::to_string(request.total_artifact_bytes));
  for (const af::ArtifactRef& artifact : request.artifacts) {
    Report::note("artifact name=" + artifact.name + " bytes=" +
                 std::to_string(artifact.size_bytes) + " digest=" +
                 artifact.content_digest.substr(0, 32));
  }
  Report::item("artifact_set_digest=" + request.artifact_set_digest.substr(0, 32));
  Report::item("canonical_state_digest=" + request.canonical_state_digest.substr(0, 32));
  Report::item("ancestry(root first)=" + join_ids(request.ancestry));
  Report::item("eligibility=" +
               std::string(af::promotion_eligibility_state_name(request.eligibility)));
  Report::item("outstanding requirements: " + join(request.outstanding_requirements));
  Report::item("evidence summaries:");
  for (const af::PromotionEvidenceSummary& summary : request.mandatory_evidence) {
    std::string text = summary.evaluator_key;
    text.append("  class=");
    text.append(af::requirement_class_name(summary.requirement_class));
    text.append("  kind=");
    text.append(af::evaluator_kind_name(summary.kind));
    text.append("  outcome=");
    text.append(af::evaluation_outcome_name(summary.outcome));
    text.append("  complete=");
    text.append(summary.complete ? "yes" : "no");
    if (summary.has_score) {
      text.append("  score=");
      text.append(number(summary.score));
    }
    text.append("  evidence_digest=");
    text.append(summary.evidence_digest.substr(0, 16));
    Report::note(text);
  }
}

void print_receipt(const af::PromotionReceipt& receipt) {
  Report::item("handoff=" + std::string(af::promotion_handoff_state_name(receipt.handoff)) +
               " sink=" + receipt.sink_name +
               " sink_reference=" +
               (receipt.sink_reference.empty() ? std::string("none") : receipt.sink_reference));
  Report::note("detail: " + receipt.detail);
  Report::note("request_digest echoed=" + receipt.request_digest.substr(0, 32));
}

void print_handoff_vocabulary() {
  Report::section("every handoff state this runtime can represent");
  for (std::uint32_t ordinal = 0; ordinal < 5; ++ordinal) {
    Report::item(std::string(af::promotion_handoff_state_name(
        static_cast<af::PromotionHandoffState>(ordinal))));
  }
  Report::note("there is no Promoted state: only an external promotion system may claim that, and "
               "the foundry never records it on that system's behalf");
}

}  // namespace

int main() {
  Report report;
  Report::line("Autonomous Foundry reference example: promotion handoff");
  Report::line("the foundry determines eligibility and emits a request; it never promotes");

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
  af::TaskSpec task = core->task(task_id).value();
  Report::item("task " + task.name + " generation=" + task.generation.to_string() +
               " mandatory=" + std::to_string(task.mandatory_requirement_count()));

  Report::section("population, production, evaluation");
  af::PopulationSpec population_spec;
  population_spec.name = "P1-promotion";
  population_spec.task = task.id;
  population_spec.task_generation = task.generation;
  population_spec.policy = policy.id;
  population_spec.policy_generation = policy.generation;
  population_spec.candidate_budget = 2;
  population_spec.worker_budget = 2;
  population_spec.population_index = 1;
  population_spec.require_promotion_request = true;
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

  std::vector<Produced> produced;
  const std::string sources[] = {af::generate_reference_solution(af::ReferenceStrategy::ClosedForm),
                                 forbidden_token_source()};
  const std::string strategies[] = {"reference-closed-form",
                                    "reference-closed-form-forbidden"};
  for (std::size_t index = 0; index < 2; ++index) {
    const af::Result<af::WorkerSessionAuthority> session =
        open_worker(*core, context.allocator, "slot-" + std::to_string(index + 1));
    if (!report.expect_status(session.status(), "worker session opened")) {
      return 1;
    }
    Produced entry;
    if (!produce(report, *core, session.value(), "slot-" + std::to_string(index + 1),
                 sources[index], strategies[index], entry)) {
      return 1;
    }
    produced.push_back(entry);
  }
  for (std::size_t index = 0; index < produced.size(); ++index) {
    if (!publish(report, *core, produced[index], "slot-" + std::to_string(index + 1))) {
      return 1;
    }
  }

  std::map<std::string, af::EvaluatorId> evaluator_ids;
  if (!evaluate(report, *core, task, population, context.allocator, produced, evaluator_ids)) {
    return 1;
  }
  for (const Produced& entry : produced) {
    const af::CandidateRecord candidate = core->candidate(entry.candidate).value();
    Report::item("candidate " + entry.candidate.to_string() +
                 " state=" + std::string(af::candidate_state_name(candidate.state)));
  }

  const af::Result<af::SelectionDecision> prepared = core->prepare_selection(population);
  if (!report.expect_status(prepared.status(), "prepare_selection")) {
    return 1;
  }
  const af::Result<af::SelectionDecision> committed = core->commit_selection(prepared.value());
  if (!report.expect_status(committed.status(), "commit_selection")) {
    return 1;
  }
  const af::CandidateId winner = committed.value().selected;
  Report::item("winner=" + winner.to_string() +
               " ranked=" + std::to_string(committed.value().ranking.size()) +
               " excluded=" + std::to_string(committed.value().excluded.size()));

  const af::Result<af::RetentionDecision> prepared_retention = core->prepare_retention(population);
  if (!report.expect_status(prepared_retention.status(), "prepare_retention")) {
    return 1;
  }
  const af::Result<af::RetentionDecision> committed_retention =
      core->commit_retention(prepared_retention.value());
  if (!report.expect_status(committed_retention.status(), "commit_retention")) {
    return 1;
  }

  Report::section("eligible request");
  const af::Result<af::PromotionRequest> eligible = core->prepare_promotion_request(population);
  if (!report.expect_status(eligible.status(), "prepare_promotion_request")) {
    return 1;
  }
  report.expect(eligible.value().candidate == winner,
                "the request names the committed selection winner");
  report.expect(eligible.value().eligibility == af::PromotionEligibilityState::Eligible,
                "the winner is Eligible: every mandatory gate has a complete authoritative pass");
  report.expect(eligible.value().outstanding_requirements.empty(),
                "an eligible request carries no outstanding requirement");
  print_request(eligible.value());

  Report::section("handoff to a local recorder");
  LocalRecorderSink recorder("local-recorder", af::PromotionHandoffState::Requested);
  const af::Result<af::PromotionReceipt> receipt = recorder.submit(eligible.value());
  if (!report.expect_status(receipt.status(), "the sink answered the request")) {
    return 1;
  }
  report.expect(recorder.submissions() == 1, "the sink received exactly one request");
  report.expect(receipt.value().handoff == af::PromotionHandoffState::Requested,
                "the handoff state is Requested");
  report.expect(receipt.value().request_digest == eligible.value().canonical_state_digest,
                "the receipt echoes the request digest it answers");
  print_receipt(receipt.value());
  Report::item("the artifact was NOT promoted: the foundry recorded a request and nothing more");

  if (!report.expect_status(core->record_promotion_receipt(eligible.value().id, receipt.value()),
                            "record_promotion_receipt")) {
    return 1;
  }

  Report::section("ineligible request: the task moved on");
  const af::Result<af::TaskGeneration> revised =
      core->revise_task(task_id, core->task(task_id).value());
  if (!report.expect_status(revised.status(), "task revised to a new generation")) {
    return 1;
  }
  task = core->task(task_id).value();
  Report::item("task generation is now " + task.generation.to_string() +
               " while the winner was produced under generation " +
               eligible.value().task_generation.to_string());

  const af::Result<af::PromotionRequest> stale = core->prepare_promotion_request(population);
  if (!report.expect_status(stale.status(), "second prepare_promotion_request")) {
    return 1;
  }
  report.expect(stale.value().eligibility == af::PromotionEligibilityState::NotEligible,
                "the same winner is NotEligible once its task generation is no longer current");
  report.expect(!stale.value().outstanding_requirements.empty(),
                "the ineligible request lists the concrete reason");
  print_request(stale.value());

  Report::section("handoff to a sink that declines");
  LocalRecorderSink declining("declining-recorder", af::PromotionHandoffState::DeclinedBySink);
  const af::Result<af::PromotionReceipt> declined = declining.submit(stale.value());
  if (!report.expect_status(declined.status(), "the declining sink answered the request")) {
    return 1;
  }
  report.expect(declined.value().handoff == af::PromotionHandoffState::DeclinedBySink,
                "the handoff state is DeclinedBySink");
  print_receipt(declined.value());
  if (!report.expect_status(core->record_promotion_receipt(stale.value().id, declined.value()),
                            "record_promotion_receipt for the declined request")) {
    return 1;
  }

  const af::Result<af::PromotionRequest> reread =
      core->promotion_request(stale.value().id);
  if (!report.expect_status(reread.status(), "the request is durable and readable")) {
    return 1;
  }
  report.expect(reread.value().eligibility == af::PromotionEligibilityState::NotEligible,
                "the durable request still carries the eligibility it was produced with");

  print_handoff_vocabulary();

  Report::section("population promotion accounting");
  const af::PopulationRecord record = core->population(population).value();
  Report::item("promotion_requests=" + std::to_string(record.promotion_requests) +
               " committed_promotion_request=" + record.committed_promotion_request.to_string());
  Report::item("promotion_request_ids=" +
               std::to_string(core->promotion_request_ids().size()));
  const af::FoundryStatistics statistics = core->statistics();
  Report::item("statistics.promotion_requests=" +
               std::to_string(statistics.promotion_requests));

  const std::shared_ptr<af::PromotionSink> as_sink = std::make_shared<LocalRecorderSink>(
      "polymorphic-recorder", af::PromotionHandoffState::Requested);
  const af::Result<af::PromotionReceipt> polymorphic = as_sink->submit(eligible.value());
  if (!report.expect_status(polymorphic.status(), "the request can be handed to any sink")) {
    return 1;
  }
  Report::item("sink name through the interface=" + as_sink->name());

  Report::section("self-consistency audit");
  const std::vector<std::string> violations = core->audit();
  report.expect(violations.empty(), "audit reports no violations");
  for (const std::string& violation : violations) {
    Report::note(violation);
  }

  Report::line("\nRESULT: " + std::string(report.failures == 0 ? "PASS" : "FAIL") +
               " (the foundry emitted a request and claimed no promotion)");
  return report.failures == 0 ? 0 : 1;
}
