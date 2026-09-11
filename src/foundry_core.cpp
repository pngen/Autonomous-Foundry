#include "foundry_core_impl.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/hash.hpp"

namespace autonomous_foundry {

// ---------------------------------------------------------------------------
// Canonical encodings
// ---------------------------------------------------------------------------

std::string canonical_double(double value) {
  if (!std::isfinite(value)) {
    return value != value ? "nan" : (value < 0.0 ? "-inf" : "inf");
  }
  char buffer[64];
  const std::to_chars_result converted =
      std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
  if (converted.ec != std::errc()) {
    return "0";
  }
  return std::string(buffer, converted.ptr);
}

namespace {

void append_field(std::string& target, std::string_view key, std::string_view value) {
  target.append(key);
  target.push_back('=');
  target.append(std::to_string(value.size()));
  target.push_back(':');
  target.append(value);
  target.push_back(';');
}

void append_u64(std::string& target, std::string_view key, std::uint64_t value) {
  target.append(key);
  target.push_back('=');
  target.append(std::to_string(value));
  target.push_back(';');
}

std::string_view state_name(CandidateState state) { return candidate_state_name(state); }

}  // namespace

std::string encode_evaluation_for_digest(const EvaluationRecord& record) {
  std::string encoded;
  append_field(encoded, "key", record.evaluator_key);
  append_u64(encoded, "gen", record.generation.value());
  append_field(encoded, "kind", evaluator_kind_name(record.kind));
  append_field(encoded, "class", requirement_class_name(record.requirement_class));
  append_field(encoded, "outcome", evaluation_outcome_name(record.outcome));
  append_u64(encoded, "complete", record.complete ? 1u : 0u);
  append_u64(encoded, "has_score", record.has_score ? 1u : 0u);
  if (record.has_score) {
    append_field(encoded, "score", canonical_double(record.score));
  }
  append_u64(encoded, "candgen", record.candidate_generation.value());
  append_u64(encoded, "taskgen", record.task_generation.value());
  append_field(encoded, "evidence", record.evidence_digest);
  return encoded;
}

std::string encode_candidate_for_digest(const CandidateRecord& candidate,
                                        const std::vector<EvaluationRecord>& evaluations) {
  std::string encoded;
  append_u64(encoded, "id", candidate.id.raw());
  append_u64(encoded, "gen", candidate.generation.value());
  append_u64(encoded, "popgen", candidate.population_generation.value());
  append_u64(encoded, "taskgen", candidate.task_generation.value());
  append_field(encoded, "state", state_name(candidate.state));
  append_field(encoded, "artifacts", candidate.artifact_set_digest);
  append_u64(encoded, "attempts", candidate.attempt_count);
  append_u64(encoded, "selected", candidate.selected ? 1u : 0u);
  append_u64(encoded, "retained", candidate.retained ? 1u : 0u);
  append_u64(encoded, "depth", candidate.depth);

  std::vector<EvaluationRecord> ordered = evaluations;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const EvaluationRecord& a, const EvaluationRecord& b) {
                     if (a.evaluator_key != b.evaluator_key) {
                       return a.evaluator_key < b.evaluator_key;
                     }
                     return a.generation.value() < b.generation.value();
                   });
  append_u64(encoded, "evidence_count", ordered.size());
  for (const EvaluationRecord& record : ordered) {
    encoded.append(encode_evaluation_for_digest(record));
    encoded.push_back('|');
  }
  return encoded;
}

std::string compute_candidate_state_digest(const CandidateRecord& candidate,
                                           const std::vector<EvaluationRecord>& evaluations) {
  return sha256_hex(encode_candidate_for_digest(candidate, evaluations));
}

// ---------------------------------------------------------------------------
// Impl: deterministic orderings
// ---------------------------------------------------------------------------

std::vector<PopulationId> FoundryCore::Impl::ordered_populations_locked() const {
  std::vector<PopulationId> ids;
  ids.reserve(populations_.size());
  for (const auto& entry : populations_) {
    ids.push_back(entry.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<CandidateId> FoundryCore::Impl::ordered_candidates_locked(PopulationId id) const {
  std::vector<CandidateId> ids;
  const PopulationRecord* population = population_locked(id);
  if (population == nullptr) {
    return ids;
  }
  ids = population->candidates;
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<EvaluationRecord> FoundryCore::Impl::evaluations_for_locked(CandidateId id) const {
  std::vector<EvaluationRecord> result;
  for (const auto& entry : evaluations_) {
    if (entry.second.candidate == id) {
      result.push_back(entry.second);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const EvaluationRecord& a, const EvaluationRecord& b) {
              if (a.evaluator_key != b.evaluator_key) {
                return a.evaluator_key < b.evaluator_key;
              }
              return a.id < b.id;
            });
  return result;
}

FoundryCore::Impl::SelectionCanonicalInputs FoundryCore::Impl::selection_canonical_inputs_locked(
    PopulationId id) const {
  SelectionCanonicalInputs inputs;
  inputs.candidates.reserve(populations_.empty() ? 0 : 8);
  for (const CandidateId candidate_id : ordered_candidates_locked(id)) {
    const CandidateRecord* candidate = candidate_locked(candidate_id);
    if (candidate == nullptr) {
      continue;
    }
    inputs.candidates.push_back(*candidate);
    for (const EvaluationRecord& record : evaluations_for_locked(candidate_id)) {
      inputs.evaluations.push_back(record);
    }
  }
  return inputs;
}

Status FoundryCore::Impl::require_live_population_authority_locked(
    const PopulationRecord& population, std::string_view action) const {
  if (!population_state_is_terminal(population.state)) {
    return Status();
  }
  return Status(ErrorCode::LateCompletion,
                "population " + population.id.to_string() + " is " +
                    std::string(population_state_name(population.state)) + " and cannot " +
                    std::string(action));
}

std::vector<AttemptRecord> FoundryCore::Impl::attempts_for_locked(PopulationId id) const {
  std::vector<AttemptRecord> result;
  for (const auto& entry : attempts_) {
    if (entry.second.population == id) {
      result.push_back(entry.second);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const AttemptRecord& a, const AttemptRecord& b) { return a.id < b.id; });
  return result;
}

std::size_t FoundryCore::Impl::active_attempts_for_locked(PopulationId id) const {
  std::size_t count = 0;
  for (const auto& entry : attempts_) {
    const AttemptRecord& attempt = entry.second;
    if (attempt.population != id) {
      continue;
    }
    if (attempt.state == AttemptState::Authorized || attempt.state == AttemptState::Dispatched ||
        attempt.state == AttemptState::Running) {
      ++count;
    }
  }
  return count;
}

std::uint32_t FoundryCore::Impl::count_candidates_with_artifacts_locked(PopulationId id) const {
  std::uint32_t count = 0;
  const PopulationRecord* population = population_locked(id);
  if (population == nullptr) {
    return 0;
  }
  for (const CandidateId candidate_id : population->candidates) {
    const CandidateRecord* candidate = candidate_locked(candidate_id);
    if (candidate != nullptr && !candidate->artifact_set_digest.empty()) {
      ++count;
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// Impl: transitions
// ---------------------------------------------------------------------------

Status FoundryCore::Impl::set_population_state_locked(PopulationRecord& record,
                                                     PopulationState next, std::string detail) {
  if (record.state == next) {
    record.status_detail = std::move(detail);
    return Status();
  }
  if (!population_transition_is_legal(record.state, next)) {
    ++statistics_.illegal_transition_rejections;
    return Status(ErrorCode::IllegalStateTransition,
                  "population " + record.id.to_string() + " cannot move from " +
                      std::string(population_state_name(record.state)) + " to " +
                      std::string(population_state_name(next)));
  }
  record.state = next;
  record.state_epoch = epoch_;
  record.status_detail = std::move(detail);
  touch();
  return Status();
}

Status FoundryCore::Impl::set_candidate_state_locked(CandidateRecord& record, CandidateState next,
                                                    std::string reason) {
  if (record.state == next) {
    return Status();
  }
  if (!candidate_transition_is_legal(record.state, next)) {
    ++statistics_.illegal_transition_rejections;
    return Status(ErrorCode::IllegalStateTransition,
                  "candidate " + record.id.to_string() + " cannot move from " +
                      std::string(candidate_state_name(record.state)) + " to " +
                      std::string(candidate_state_name(next)) + ": " + reason);
  }
  record.state = next;
  record.state_changed_epoch = epoch_;
  if (next == CandidateState::ProductionFailed || next == CandidateState::ProductionCancelled ||
      next == CandidateState::Disqualified || next == CandidateState::Superseded ||
      next == CandidateState::Retired) {
    record.failure_reason = std::move(reason);
  }
  touch();
  return Status();
}

Status FoundryCore::Impl::set_attempt_state_locked(AttemptRecord& record, AttemptState next,
                                                  std::string detail) {
  if (record.state == next) {
    return Status();
  }
  if (attempt_state_is_terminal(record.state) && !attempt_state_is_terminal(next)) {
    ++statistics_.illegal_transition_rejections;
    return Status(ErrorCode::LateCompletion,
                  "attempt " + record.id.to_string() + " is already terminal in state " +
                      std::string(attempt_state_name(record.state)));
  }
  record.state = next;
  record.failure_detail = std::move(detail);
  if (attempt_state_is_terminal(next)) {
    record.terminal_epoch = epoch_;
  }
  touch();
  return Status();
}

// ---------------------------------------------------------------------------
// Impl: evidence
// ---------------------------------------------------------------------------

const EvaluationRecord* FoundryCore::Impl::requirement_record_locked(
    const CandidateRecord& candidate, std::string_view evaluator_key) const {
  const EvaluationRecord* best = nullptr;
  for (const auto& entry : evaluations_) {
    const EvaluationRecord& record = entry.second;
    if (record.candidate != candidate.id || record.evaluator_key != evaluator_key) {
      continue;
    }
    if (record.candidate_generation != candidate.generation) {
      continue;
    }
    if (!record.complete) {
      continue;
    }
    // A worker's own claim is evidence, not authority: it can never close a
    // declared requirement.
    if (!evaluator_kind_is_authoritative(record.kind)) {
      continue;
    }
    if (best == nullptr || best->generation < record.generation) {
      best = &record;
    }
  }
  return best;
}

bool FoundryCore::Impl::requirement_complete_locked(const CandidateRecord& candidate,
                                                    std::string_view evaluator_key) const {
  return requirement_record_locked(candidate, evaluator_key) != nullptr;
}

std::size_t FoundryCore::Impl::complete_requirement_count_locked(
    const CandidateRecord& candidate) const {
  const TaskSpec* task = task_locked(candidate.task);
  if (task == nullptr) {
    return 0;
  }
  std::size_t count = 0;
  for (const EvaluationRequirement& requirement : task->requirements) {
    if (requirement_complete_locked(candidate, requirement.evaluator_key)) {
      ++count;
    }
  }
  return count;
}

void FoundryCore::Impl::settle_candidate_locked(CandidateRecord& candidate) {
  const TaskSpec* task = task_locked(candidate.task);
  if (task == nullptr) {
    return;
  }
  if (task->requirements.empty()) {
    return;
  }
  for (const EvaluationRequirement& requirement : task->requirements) {
    if (!requirement_complete_locked(candidate, requirement.evaluator_key)) {
      return;
    }
  }
  // Published -> Evaluated is not a declared candidate edge; the declared path runs
  // through Evaluating. Stepping through it also keeps the recorded state history
  // honest, and honouring the rejection stops a candidate whose evidence became
  // complete while it was Published from being stranded there forever.
  if (candidate.state == CandidateState::Published) {
    const Status entered = set_candidate_state_locked(
        candidate, CandidateState::Evaluating, "evidence complete while published");
    if (!entered.ok()) {
      return;
    }
  }
  if (candidate.state == CandidateState::Evaluating) {
    (void)set_candidate_state_locked(candidate, CandidateState::Evaluated,
                                     "every declared requirement has a complete record");
  }
  for (auto& entry : attempts_) {
    AttemptRecord& attempt = entry.second;
    if (attempt.candidate != candidate.id) {
      continue;
    }
    if (attempt.state == AttemptState::Published || attempt.state == AttemptState::Evaluating ||
        attempt.state == AttemptState::Running || attempt.state == AttemptState::Dispatched) {
      set_attempt_state_locked(attempt, AttemptState::Completed,
                               "candidate reached Evaluated");
      if (!attempt.holds_reservation) {
        continue;
      }
      BudgetLedger* ledger = ledger_locked(attempt.population);
      if (ledger != nullptr) {
        release_attempt_reservations_locked(attempt);
      }
    }
  }
  // Settling a candidate records no evidence of its own: the evaluation that
  // completed the last requirement already counted itself in
  // record_evaluation(). Counting here as well made every recorded evaluation
  // appear twice in FoundryStatistics.
}

void FoundryCore::Impl::release_attempt_reservations_locked(AttemptRecord& attempt) {
  if (!attempt.holds_reservation) {
    return;
  }
  BudgetLedger* ledger = ledger_locked(attempt.population);
  if (ledger != nullptr) {
    // authorize_attempt opens every quantity the attempt may spend, so releasing
    // only one of them leaks the rest for the lifetime of the population: every
    // abandonment path (cancellation, failure, disconnect, incarnation
    // replacement, recovery) would leave CandidateAttempts and
    // WorkerAssignments reserved forever and the population could never close.
    // A release resolves one reservation per kind, exactly as publish_candidate
    // consumes them.
    (void)ledger->release(BudgetKind::CandidateAttempts);
    (void)ledger->release(BudgetKind::WorkerAssignments);
    (void)ledger->release(BudgetKind::ActiveAttempts);
    if (attempt.retry_index > 0) {
      (void)ledger->release(BudgetKind::Retries);
    }
  }
  attempt.holds_reservation = false;
}

// ---------------------------------------------------------------------------
// Impl: authority
// ---------------------------------------------------------------------------

Status FoundryCore::Impl::validate_worker_session_locked(const WorkerSessionAuthority& session) {
  auto reject = [this](Status status) {
    ++statistics_.stale_authority_rejections;
    return status;
  };

  const Status epoch_status = check_coordinator_epoch(session.coordinator_epoch, epoch_);
  if (!epoch_status.ok()) {
    return reject(epoch_status);
  }
  const Status run_status = check_run(session.run, run_);
  if (!run_status.ok()) {
    return reject(run_status);
  }
  if (!session.worker.valid()) {
    return reject(Status(ErrorCode::NullIdentity, "worker session carries the null worker identity"));
  }
  const WorkerRecord* worker = worker_locked(session.worker);
  if (worker == nullptr) {
    return reject(Status(ErrorCode::UnknownWorkerIncarnation,
                         "worker " + session.worker.to_string() + " has never registered"));
  }
  const Status boot_status = check_worker_boot(session.boot, worker->boot);
  if (!boot_status.ok()) {
    return reject(boot_status);
  }
  const Status session_status = check_session(session.session, worker->session);
  if (!session_status.ok()) {
    return reject(session_status);
  }
  const Status generation_status =
      check_session_generation(session.session_generation, worker->session_generation);
  if (!generation_status.ok()) {
    return reject(generation_status);
  }
  if (worker->state == WorkerState::Rejected) {
    return reject(Status(ErrorCode::AuthorityWithdrawn,
                         "worker " + session.worker.to_string() + " was rejected"));
  }
  return Status();
}

Status FoundryCore::Impl::validate_worker_operation_locked(
    const WorkerOperationAuthority& authority) {
  AF_TRY(validate_worker_session_locked(authority.session));

  const PopulationRecord* population = population_locked(authority.population);
  if (population == nullptr) {
    ++statistics_.stale_authority_rejections;
    return Status(ErrorCode::UnknownIdentity,
                  "population " + authority.population.to_string() + " does not exist");
  }
  AF_TRY(check_population_generation(authority.population_generation, population->generation));
  AF_TRY(check_identity(authority.task, population->task, "task"));
  AF_TRY(check_task_generation(authority.task_generation, population->task_generation));

  // The population is the authority that authorised this work. Once it has left
  // the live lifecycle, no operation it authorised may still mutate candidate
  // evidence: cancellation is authoritative, not advisory.
  const Status population_authority =
      require_live_population_authority_locked(*population, "accept worker operations");
  if (!population_authority.ok()) {
    ++statistics_.stale_authority_rejections;
    return population_authority;
  }

  const CandidateRecord* candidate = candidate_locked(authority.candidate);
  if (candidate == nullptr) {
    ++statistics_.stale_authority_rejections;
    return Status(ErrorCode::UnknownIdentity,
                  "candidate " + authority.candidate.to_string() + " does not exist");
  }
  AF_TRY(check_candidate_generation(authority.candidate_generation, candidate->generation));
  AF_TRY(check_identity(authority.candidate, candidate->id, "candidate"));
  if (candidate->population != population->id) {
    ++statistics_.stale_authority_rejections;
    return Status(ErrorCode::StaleAuthority, "candidate does not belong to the named population");
  }

  const AttemptRecord* attempt = attempt_locked(authority.attempt);
  if (attempt == nullptr) {
    ++statistics_.stale_authority_rejections;
    return Status(ErrorCode::UnknownIdentity,
                  "attempt " + authority.attempt.to_string() + " does not exist");
  }
  AF_TRY(check_attempt_generation(authority.attempt_generation, attempt->generation));
  AF_TRY(check_identity(authority.assignment, attempt->assignment, "assignment"));
  if (attempt->candidate != candidate->id) {
    ++statistics_.stale_authority_rejections;
    return Status(ErrorCode::StaleAuthority, "attempt does not belong to the named candidate");
  }
  if (attempt->worker != authority.session.worker || attempt->worker_boot != authority.session.boot) {
    ++statistics_.stale_authority_rejections;
    return Status(ErrorCode::StaleWorkerBoot,
                  "attempt was authorized for a different worker incarnation");
  }
  return Status();
}

// ---------------------------------------------------------------------------
// FoundryCore: construction
// ---------------------------------------------------------------------------

FoundryCore::FoundryCore(FoundryConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

FoundryCore::~FoundryCore() = default;

FoundryId FoundryCore::foundry() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->foundry_;
}

FoundryRunId FoundryCore::run() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->run_;
}

CoordinatorEpoch FoundryCore::epoch() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->epoch_;
}

std::uint64_t FoundryCore::revision() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->revision_;
}

// ---------------------------------------------------------------------------
// Read paths
// ---------------------------------------------------------------------------

Result<TaskSpec> FoundryCore::task(TaskId id) const {
  std::shared_lock lock(impl_->mutex);
  const TaskSpec* found = impl_->task_locked(id);
  if (found == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "task " + id.to_string() + " does not exist");
  }
  return *found;
}

Result<FoundryPolicy> FoundryCore::policy(PolicyId id) const {
  std::shared_lock lock(impl_->mutex);
  const FoundryPolicy* found = impl_->policy_locked(id);
  if (found == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "policy " + id.to_string() + " does not exist");
  }
  return *found;
}

Result<PopulationRecord> FoundryCore::population(PopulationId id) const {
  std::shared_lock lock(impl_->mutex);
  const PopulationRecord* found = impl_->population_locked(id);
  if (found == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  return *found;
}

Result<CandidateRecord> FoundryCore::candidate(CandidateId id) const {
  std::shared_lock lock(impl_->mutex);
  const CandidateRecord* found = impl_->candidate_locked(id);
  if (found == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "candidate " + id.to_string() + " does not exist");
  }
  return *found;
}

Result<WorkerRecord> FoundryCore::worker(WorkerId id) const {
  std::shared_lock lock(impl_->mutex);
  const WorkerRecord* found = impl_->worker_locked(id);
  if (found == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "worker " + id.to_string() + " does not exist");
  }
  return *found;
}

Result<AttemptRecord> FoundryCore::attempt(AttemptId id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->attempts_.find(id);
  if (found == impl_->attempts_.end()) {
    return Status(ErrorCode::UnknownIdentity, "attempt " + id.to_string() + " does not exist");
  }
  return found->second;
}

Result<AssignmentRecord> FoundryCore::assignment(AssignmentId id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->assignments_.find(id);
  if (found == impl_->assignments_.end()) {
    return Status(ErrorCode::UnknownIdentity, "assignment " + id.to_string() + " does not exist");
  }
  return found->second;
}

Result<EvaluationRecord> FoundryCore::evaluation(EvaluationId id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->evaluations_.find(id);
  if (found == impl_->evaluations_.end()) {
    return Status(ErrorCode::UnknownIdentity, "evaluation " + id.to_string() + " does not exist");
  }
  return found->second;
}

Result<SelectionDecision> FoundryCore::selection(PopulationId id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->selections_.find(id);
  if (found == impl_->selections_.end()) {
    return Status(ErrorCode::UnknownIdentity,
                  "population " + id.to_string() + " has no selection decision");
  }
  return found->second;
}

Result<RetentionDecision> FoundryCore::retention(PopulationId id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->retentions_.find(id);
  if (found == impl_->retentions_.end()) {
    return Status(ErrorCode::UnknownIdentity,
                  "population " + id.to_string() + " has no retention decision");
  }
  return found->second;
}

Result<PromotionRequest> FoundryCore::promotion_request(PromotionRequestId id) const {
  std::shared_lock lock(impl_->mutex);
  const auto found = impl_->promotion_requests_.find(id);
  if (found == impl_->promotion_requests_.end()) {
    return Status(ErrorCode::UnknownIdentity,
                  "promotion request " + id.to_string() + " does not exist");
  }
  return found->second;
}

std::vector<PopulationId> FoundryCore::population_ids() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->ordered_populations_locked();
}

std::vector<CandidateId> FoundryCore::population_candidates(PopulationId id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->ordered_candidates_locked(id);
}

std::vector<EvaluationRecord> FoundryCore::candidate_evaluations(CandidateId id) const {
  std::shared_lock lock(impl_->mutex);
  return impl_->evaluations_for_locked(id);
}

std::vector<WorkerRecord> FoundryCore::workers() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<WorkerRecord> result;
  result.reserve(impl_->workers_.size());
  for (const auto& entry : impl_->workers_) {
    result.push_back(entry.second);
  }
  std::sort(result.begin(), result.end(),
            [](const WorkerRecord& a, const WorkerRecord& b) { return a.id < b.id; });
  return result;
}

std::vector<AttemptRecord> FoundryCore::attempts() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<AttemptRecord> result;
  result.reserve(impl_->attempts_.size());
  for (const auto& entry : impl_->attempts_) {
    result.push_back(entry.second);
  }
  std::sort(result.begin(), result.end(),
            [](const AttemptRecord& a, const AttemptRecord& b) { return a.id < b.id; });
  return result;
}

std::vector<LineageNode> FoundryCore::lineage_nodes() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<LineageNode> result;
  result.reserve(impl_->lineage_.nodes().size());
  for (const auto& entry : impl_->lineage_.nodes()) {
    result.push_back(entry.second);
  }
  std::sort(result.begin(), result.end(), [](const LineageNode& a, const LineageNode& b) {
    if (a.depth != b.depth) {
      return a.depth < b.depth;
    }
    return a.candidate < b.candidate;
  });
  return result;
}

std::vector<PromotionRequestId> FoundryCore::promotion_request_ids() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<PromotionRequestId> result;
  result.reserve(impl_->promotion_requests_.size());
  for (const auto& entry : impl_->promotion_requests_) {
    result.push_back(entry.first);
  }
  std::sort(result.begin(), result.end());
  return result;
}

FoundryStatistics FoundryCore::statistics() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->statistics_;
}

FoundrySnapshot FoundryCore::snapshot() const {
  std::shared_lock lock(impl_->mutex);
  FoundrySnapshot snap;
  snap.foundry = impl_->foundry_;
  snap.run = impl_->run_;
  snap.epoch = impl_->epoch_;
  snap.revision = impl_->revision_;
  snap.id_salt = impl_->ids_.salt();
  snap.id_counters = impl_->ids_.counters();
  snap.tasks = impl_->tasks_;
  snap.policies = impl_->policies_;
  snap.populations = impl_->populations_;
  snap.candidates = impl_->candidates_;
  snap.workers = impl_->workers_;
  snap.attempts = impl_->attempts_;
  snap.assignments = impl_->assignments_;
  snap.evaluations = impl_->evaluations_;
  snap.selections = impl_->selections_;
  snap.retentions = impl_->retentions_;
  snap.promotion_requests = impl_->promotion_requests_;
  snap.ledgers = impl_->ledgers_;
  snap.lineage = impl_->lineage_;
  snap.statistics = impl_->statistics_;
  // The declared counters are the upper bound of every identity this state
  // holds, not merely what the allocator happens to have reached. A caller may
  // mint an identity itself - define_task with an explicit id, a worker's own
  // worker and boot identities, a promotion request built by an embedder - which
  // advances the state without advancing the allocator at all. Reporting the
  // allocator alone would let a core emit an image that its own validator must
  // refuse, which is a durability hole rather than a cosmetic difference.
  const std::array<std::uint32_t, kIdKindCount> observed = observed_identity_counters(snap);
  for (std::size_t index = 1; index < snap.id_counters.size(); ++index) {
    if (observed[index] > snap.id_counters[index]) {
      snap.id_counters[index] = observed[index];
    }
  }
  return snap;
}

std::vector<std::string> FoundryCore::closure_blockers(PopulationId id) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<std::string> blockers;
  const PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    blockers.push_back("population " + id.to_string() + " does not exist");
    return blockers;
  }
  if (!population->committed_selection.valid()) {
    blockers.push_back("no selection decision has been committed");
  }
  if (!population->retention_committed) {
    blockers.push_back("no retention decision has been committed");
  }
  if (population->require_promotion_request && !population->committed_promotion_request.valid()) {
    blockers.push_back("promotion eligibility is unresolved");
  }

  std::size_t open_attempts = 0;
  for (const auto& entry : impl_->attempts_) {
    const AttemptRecord& attempt = entry.second;
    if (attempt.population != id) {
      continue;
    }
    if (!attempt_state_is_terminal(attempt.state)) {
      ++open_attempts;
    }
  }
  if (open_attempts != 0) {
    blockers.push_back(std::to_string(open_attempts) +
                       " production attempt(s) have not reached a terminal state");
  }

  std::size_t unresolved_candidates = 0;
  for (const CandidateId candidate_id : population->candidates) {
    const CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      blockers.push_back("candidate " + candidate_id.to_string() + " is referenced but missing");
      continue;
    }
    switch (candidate->state) {
      case CandidateState::Registered:
      case CandidateState::Producing:
      case CandidateState::Published:
      case CandidateState::Evaluating:
      case CandidateState::RevalidationRequired:
        ++unresolved_candidates;
        break;
      default:
        break;
    }
  }
  if (unresolved_candidates != 0) {
    blockers.push_back(std::to_string(unresolved_candidates) +
                       " candidate(s) still have unresolved production or evaluation");
  }

  const auto ledger = impl_->ledgers_.find(id);
  if (ledger != impl_->ledgers_.end() && ledger->second.open_reservations() != 0) {
    blockers.push_back(std::to_string(ledger->second.open_reservations()) +
                       " budget reservation(s) are still open");
  }
  return blockers;
}

}  // namespace autonomous_foundry
