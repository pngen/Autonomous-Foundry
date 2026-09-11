#include "foundry_core_impl.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/workspace.hpp"

namespace autonomous_foundry {
namespace {

std::filesystem::path attempt_workspace_path(const FoundryConfig& config, PopulationId population,
                                             AttemptId attempt) {
  std::filesystem::path root = config.workspace_root;
  root /= "population-" + std::to_string(population.counter());
  root /= "attempt-" + std::to_string(attempt.counter());
  return root;
}

}  // namespace

// ---------------------------------------------------------------------------
// Worker sessions
// ---------------------------------------------------------------------------

Result<WorkerSessionAuthority> FoundryCore::register_worker(
    const WorkerRegistrationRequest& request) {
  std::unique_lock lock(impl_->mutex);

  if (!request.worker.valid()) {
    return Status(ErrorCode::NullIdentity, "worker registration requires a worker identity");
  }
  if (!request.boot.valid()) {
    return Status(ErrorCode::NullIdentity, "worker registration requires a boot identity");
  }
  AF_TRY(validate_worker_text(request.label, "worker label"));
  AF_TRY(validate_worker_text(request.capability, "worker capability"));

  WorkerRecord* record = impl_->worker_locked(request.worker);
  bool revalidation_required = false;
  if (record == nullptr) {
    WorkerRecord created;
    created.id = request.worker;
    created.boot = request.boot;
    created.registered_epoch = impl_->epoch_;
    impl_->workers_.emplace(request.worker, created);
    record = impl_->worker_locked(request.worker);
  } else if (record->boot == request.boot) {
    // The same incarnation reconnecting. A duplicate connection is refused
    // rather than allowed to take over a live incarnation's session.
    if (record->state == WorkerState::Ready || record->state == WorkerState::Busy ||
        record->state == WorkerState::Registered) {
      impl_->statistics_.duplicate_rejections += 1;
      return Status(ErrorCode::AlreadyExists,
                    "worker " + request.worker.to_string() +
                        " is already connected under this boot identity");
    }
    revalidation_required = record->registered_epoch != impl_->epoch_;
  } else {
    // A different incarnation replaces the previous one. Any authority the old
    // incarnation held is gone, and an in-flight attempt becomes ambiguous
    // rather than being invented as a success or a failure.
    for (auto& entry : impl_->attempts_) {
      AttemptRecord& attempt = entry.second;
      if (attempt.worker != request.worker || attempt_state_is_terminal(attempt.state)) {
        continue;
      }
      impl_->release_attempt_reservations_locked(attempt);
      AF_TRY(impl_->set_attempt_state_locked(
          attempt, AttemptState::OutcomeUnknown,
          "worker incarnation replaced while the assignment was in flight"));
      impl_->statistics_.attempts_outcome_unknown += 1;
      CandidateRecord* candidate = impl_->candidate_locked(attempt.candidate);
      if (candidate != nullptr && (candidate->state == CandidateState::Producing ||
                                   candidate->state == CandidateState::Registered)) {
        AF_TRY(impl_->set_candidate_state_locked(
            *candidate, CandidateState::Registered,
            "worker incarnation replaced; the candidate slot may be retried"));
      }
    }
    record->boot = request.boot;
    record->registered_epoch = impl_->epoch_;
    record->active_attempt = AttemptId();
    record->active_assignment = AssignmentId();
  }
  if (record == nullptr) {
    return Status(ErrorCode::Internal, "worker registration could not establish a record");
  }

  SessionId session;
  AF_TRY_ASSIGN(session, impl_->ids_.next<SessionIdTag>());
  const WorkerSessionGeneration previous = record->session_generation;
  record->session = session;
  record->session_generation = previous.valid()
                                   ? WorkerSessionGeneration::from_value(previous.value() + 1)
                                   : WorkerSessionGeneration::first();
  record->label = request.label;
  record->capability = request.capability;
  record->process_id = request.process_id;
  record->state =
      revalidation_required ? WorkerState::RevalidationRequired : WorkerState::Registered;
  record->state_epoch = impl_->epoch_;
  record->last_diagnostic = revalidation_required
                                ? "reconnected after a coordinator restart; revalidation required"
                                : "registered";

  WorkerSessionAuthority authority;
  authority.coordinator_epoch = impl_->epoch_;
  authority.run = impl_->run_;
  authority.worker = record->id;
  authority.boot = record->boot;
  authority.session = record->session;
  authority.session_generation = record->session_generation;
  impl_->touch();
  return authority;
}

Status FoundryCore::worker_ready(const WorkerSessionAuthority& session, std::string capability) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_session_locked(session));
  WorkerRecord* record = impl_->worker_locked(session.worker);
  if (record == nullptr) {
    return Status(ErrorCode::UnknownWorkerIncarnation, "worker record disappeared");
  }
  if (record->state == WorkerState::RevalidationRequired) {
    return Status(ErrorCode::RevalidationRequired,
                  "worker " + session.worker.to_string() +
                      " must revalidate before it may accept work");
  }
  if (record->state != WorkerState::Registered && record->state != WorkerState::Ready) {
    return Status(ErrorCode::IllegalStateTransition,
                  "worker " + session.worker.to_string() + " is " +
                      std::string(worker_state_name(record->state)) + " and cannot report ready");
  }
  AF_TRY(validate_worker_text(capability, "worker capability"));
  record->capability = std::move(capability);
  record->state = WorkerState::Ready;
  record->state_epoch = impl_->epoch_;
  record->last_diagnostic = "ready";
  impl_->touch();
  return Status();
}

Result<WorkerSessionAuthority> FoundryCore::revalidate_worker(const WorkerSessionAuthority& session,
                                                              std::string detail) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_session_locked(session));
  WorkerRecord* record = impl_->worker_locked(session.worker);
  if (record == nullptr) {
    return Status(ErrorCode::UnknownWorkerIncarnation, "worker record disappeared");
  }
  if (record->state != WorkerState::RevalidationRequired &&
      record->state != WorkerState::Registered) {
    return Status(ErrorCode::IllegalStateTransition,
                  "worker " + session.worker.to_string() + " is " +
                      std::string(worker_state_name(record->state)) +
                      " and is not awaiting revalidation");
  }

  // Revalidation issues a fresh session and session generation. Every piece of
  // authority issued to this incarnation before the restart becomes unusable,
  // including authority the worker still believes it holds.
  SessionId fresh;
  AF_TRY_ASSIGN(fresh, impl_->ids_.next<SessionIdTag>());
  const WorkerSessionGeneration previous = record->session_generation;
  record->session = fresh;
  record->session_generation = previous.valid()
                                   ? WorkerSessionGeneration::from_value(previous.value() + 1)
                                   : WorkerSessionGeneration::first();
  record->registered_epoch = impl_->epoch_;
  record->state = WorkerState::Registered;
  record->state_epoch = impl_->epoch_;
  record->last_diagnostic = "revalidated: " + detail;

  WorkerSessionAuthority authority;
  authority.coordinator_epoch = impl_->epoch_;
  authority.run = impl_->run_;
  authority.worker = record->id;
  authority.boot = record->boot;
  authority.session = record->session;
  authority.session_generation = record->session_generation;
  impl_->touch();
  return authority;
}

Status FoundryCore::worker_disconnected(const WorkerSessionAuthority& session,
                                        std::string reason) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_session_locked(session));
  WorkerRecord* record = impl_->worker_locked(session.worker);
  if (record == nullptr) {
    return Status(ErrorCode::UnknownWorkerIncarnation, "worker record disappeared");
  }

  for (auto& entry : impl_->attempts_) {
    AttemptRecord& attempt = entry.second;
    if (attempt.worker != session.worker || attempt.worker_boot != session.boot) {
      continue;
    }
    if (attempt_state_is_terminal(attempt.state)) {
      continue;
    }
    // The worker is gone. Whether the work completed is unknowable from here,
    // so the attempt is recorded as ambiguous rather than as a success or a
    // failure. Replay is explicit: only a fresh attempt identity may be
    // authorized for this slot.
    impl_->release_attempt_reservations_locked(attempt);
    AF_TRY(impl_->set_attempt_state_locked(attempt, AttemptState::OutcomeUnknown, reason));
    impl_->statistics_.attempts_outcome_unknown += 1;
    CandidateRecord* candidate = impl_->candidate_locked(attempt.candidate);
    if (candidate != nullptr && (candidate->state == CandidateState::Producing ||
                                 candidate->state == CandidateState::Registered)) {
      AF_TRY(impl_->set_candidate_state_locked(
          *candidate, CandidateState::Registered,
          "producing worker disconnected with an undetermined outcome; slot is retryable"));
    }
  }

  record->state = WorkerState::Offline;
  record->state_epoch = impl_->epoch_;
  record->active_attempt = AttemptId();
  record->active_attempt_generation = AttemptGeneration();
  record->active_assignment = AssignmentId();
  record->last_diagnostic = std::move(reason);
  impl_->touch();
  return Status();
}

Status FoundryCore::reject_worker_session(const WorkerSessionAuthority& session,
                                          std::string reason) {
  std::unique_lock lock(impl_->mutex);
  WorkerRecord* record = impl_->worker_locked(session.worker);
  if (record == nullptr) {
    return Status(ErrorCode::UnknownWorkerIncarnation, "worker record does not exist");
  }
  if (record->boot != session.boot) {
    return Status(ErrorCode::StaleWorkerBoot,
                  "refusing to reject a worker under a different boot identity");
  }
  record->state = WorkerState::Rejected;
  record->state_epoch = impl_->epoch_;
  record->last_diagnostic = std::move(reason);
  impl_->touch();
  return Status();
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

Result<PendingDispatch> FoundryCore::authorize_attempt(const WorkerSessionAuthority& session) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_session_locked(session));
  WorkerRecord* worker = impl_->worker_locked(session.worker);
  if (worker == nullptr) {
    return Status(ErrorCode::UnknownWorkerIncarnation, "worker record does not exist");
  }
  if (worker->state == WorkerState::RevalidationRequired) {
    return Status(ErrorCode::RevalidationRequired,
                  "worker must revalidate before it may accept work");
  }
  if (worker->state != WorkerState::Ready) {
    return Status(ErrorCode::NotReady,
                  "worker " + session.worker.to_string() + " is " +
                      std::string(worker_state_name(worker->state)) +
                      " and cannot accept an assignment");
  }

  for (const PopulationId population_id : impl_->ordered_populations_locked()) {
    PopulationRecord* population = impl_->population_locked(population_id);
    if (population == nullptr || population->state != PopulationState::Running) {
      continue;
    }
    if (population->worker_budget == 0 ||
        impl_->active_attempts_for_locked(population_id) >= population->worker_budget) {
      continue;
    }
    BudgetLedger* ledger = impl_->ledger_locked(population_id);
    if (ledger == nullptr) {
      continue;
    }

    CandidateId slot;
    for (const CandidateId candidate_id : impl_->ordered_candidates_locked(population_id)) {
      const CandidateRecord* probe = impl_->candidate_locked(candidate_id);
      if (probe == nullptr) {
        continue;
      }
      if (probe->state != CandidateState::Registered &&
          probe->state != CandidateState::Producing) {
        continue;
      }
      if (probe->attempt_count >= kMaxAttemptsPerCandidate) {
        continue;
      }
      bool has_open_attempt = false;
      for (const auto& entry : impl_->attempts_) {
        const AttemptRecord& attempt = entry.second;
        if (attempt.candidate == candidate_id && !attempt_state_is_terminal(attempt.state)) {
          has_open_attempt = true;
          break;
        }
      }
      if (has_open_attempt) {
        continue;
      }
      slot = candidate_id;
      break;
    }

    if (!slot.valid()) {
      if (population->candidates.size() >= population->candidate_budget) {
        continue;
      }
      if (!ledger->has_capacity(BudgetKind::Candidates) ||
          !ledger->has_capacity(BudgetKind::CandidateAttempts) ||
          !ledger->has_capacity(BudgetKind::WorkerAssignments) ||
          !ledger->has_capacity(BudgetKind::ActiveAttempts)) {
        continue;
      }
      AF_TRY(ledger->reserve(BudgetKind::Candidates));
      const Result<CandidateId> allocated = [&]() -> Result<CandidateId> {
        CandidateId id;
        AF_TRY_ASSIGN(id, impl_->ids_.next<CandidateIdTag>());
        CandidateRecord candidate;
        candidate.id = id;
        candidate.generation = CandidateGeneration::first();
        candidate.population = population->id;
        candidate.population_generation = population->generation;
        candidate.task = population->task;
        candidate.task_generation = population->task_generation;
        if (population->seed_candidate.valid() &&
            impl_->lineage_.contains(population->seed_candidate)) {
          const LineageNode* parent = impl_->lineage_.find(population->seed_candidate);
          if (parent == nullptr) {
            return Status(ErrorCode::LineageOrphan, "seed lineage node disappeared");
          }
          candidate.parents.push_back(parent->candidate);
          candidate.lineage = population->seed_lineage.valid() ? population->seed_lineage
                                                               : parent->lineage;
          candidate.depth = parent->depth + 1;
          if (candidate.depth > kMaxLineageDepth) {
            return Status(ErrorCode::GenerationRegression,
                          "the seeded lineage reached the depth bound");
          }
        } else {
          LineageId lineage;
          AF_TRY_ASSIGN(lineage, impl_->ids_.next<LineageIdTag>());
          candidate.lineage = lineage;
          candidate.depth = 0;
        }
        candidate.state = CandidateState::Registered;
        candidate.created_epoch = impl_->epoch_;
        candidate.state_changed_epoch = impl_->epoch_;
        candidate.evidence_generation = EvidenceGeneration::first();

        LineageNode node;
        node.candidate = id;
        node.candidate_generation = candidate.generation;
        node.lineage = candidate.lineage;
        node.parents = candidate.parents;
        node.depth = candidate.depth;
        node.created_epoch = impl_->epoch_;
        AF_TRY(impl_->lineage_.insert(node));

        population->candidates.push_back(id);
        impl_->candidates_.emplace(id, candidate);
        impl_->statistics_.candidate_slots_created += 1;
        return id;
      }();
      if (!allocated.ok()) {
        (void)ledger->release(BudgetKind::Candidates);
        continue;
      }
      slot = allocated.value();
      (void)ledger->consume(BudgetKind::Candidates);
    }

    CandidateRecord* candidate = impl_->candidate_locked(slot);
    if (candidate == nullptr) {
      continue;
    }
    const bool is_retry = candidate->attempt_count > 0;
    if (is_retry && !ledger->has_capacity(BudgetKind::Retries)) {
      continue;
    }

    AF_TRY(ledger->reserve(BudgetKind::CandidateAttempts));
    if (!ledger->reserve(BudgetKind::WorkerAssignments).ok()) {
      (void)ledger->release(BudgetKind::CandidateAttempts);
      continue;
    }
    if (!ledger->reserve(BudgetKind::ActiveAttempts).ok()) {
      (void)ledger->release(BudgetKind::CandidateAttempts);
      (void)ledger->release(BudgetKind::WorkerAssignments);
      continue;
    }
    if (is_retry && !ledger->reserve(BudgetKind::Retries).ok()) {
      (void)ledger->release(BudgetKind::CandidateAttempts);
      (void)ledger->release(BudgetKind::WorkerAssignments);
      (void)ledger->release(BudgetKind::ActiveAttempts);
      continue;
    }

    AttemptId attempt_id;
    AF_TRY_ASSIGN(attempt_id, impl_->ids_.next<AttemptIdTag>());
    AssignmentId assignment_id;
    AF_TRY_ASSIGN(assignment_id, impl_->ids_.next<AssignmentIdTag>());

    AssignmentRecord assignment;
    assignment.id = assignment_id;
    assignment.generation = AssignmentGeneration::first();
    assignment.attempt = attempt_id;
    assignment.attempt_generation = AttemptGeneration::first();
    assignment.worker = session.worker;
    assignment.worker_boot = session.boot;
    assignment.session = session.session;
    assignment.session_generation = session.session_generation;
    assignment.issued_epoch = impl_->epoch_;

    AttemptRecord attempt;
    attempt.id = attempt_id;
    attempt.generation = AttemptGeneration::first();
    attempt.population = population->id;
    attempt.population_generation = population->generation;
    attempt.task = population->task;
    attempt.task_generation = population->task_generation;
    attempt.candidate = candidate->id;
    attempt.candidate_generation = candidate->generation;
    attempt.worker = session.worker;
    attempt.worker_boot = session.boot;
    attempt.session = session.session;
    attempt.assignment = assignment_id;
    attempt.authorized_epoch = impl_->epoch_;
    attempt.state = AttemptState::Authorized;
    attempt.retry_index = candidate->attempt_count;
    attempt.holds_reservation = true;
    if (candidate->attempt.valid()) {
      attempt.previous_attempt = candidate->attempt;
    }

    candidate->attempt = attempt_id;
    candidate->attempt_generation = attempt.generation;
    candidate->assignment = assignment_id;
    candidate->attempt_count += 1;
    AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::Producing,
                                             "production attempt authorized"));

    worker->state = WorkerState::Busy;
    worker->state_epoch = impl_->epoch_;
    worker->active_attempt = attempt_id;
    worker->active_attempt_generation = attempt.generation;
    worker->active_assignment = assignment_id;
    worker->assignments_accepted += 1;

    impl_->assignments_.emplace(assignment_id, assignment);
    impl_->attempts_.emplace(attempt_id, attempt);
    impl_->statistics_.attempts_authorized += 1;
    impl_->touch();

    PendingDispatch pending;
    pending.session = session;
    pending.attempt = attempt;
    pending.assignment = assignment;
    return pending;
  }

  return Status(ErrorCode::Unavailable, "no population currently has dispatchable work");
}

Result<AttemptPackage> FoundryCore::confirm_dispatch(const WorkerSessionAuthority& session,
                                                    AttemptId attempt,
                                                    AttemptGeneration generation) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_session_locked(session));
  AttemptRecord* record = impl_->attempt_locked(attempt);
  if (record == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "attempt " + attempt.to_string() + " does not exist");
  }
  AF_TRY(check_attempt_generation(generation, record->generation));
  if (record->worker != session.worker || record->worker_boot != session.boot) {
    return Status(ErrorCode::StaleWorkerBoot, "attempt belongs to a different worker incarnation");
  }
  if (record->state != AttemptState::Authorized) {
    return Status(ErrorCode::IllegalStateTransition,
                  "attempt " + attempt.to_string() + " is " +
                      std::string(attempt_state_name(record->state)) +
                      " and cannot be dispatched");
  }
  const PopulationRecord* population = impl_->population_locked(record->population);
  const TaskSpec* task = impl_->task_locked(record->task);
  if (population == nullptr || task == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "attempt references a missing population or task");
  }

  AF_TRY(impl_->set_attempt_state_locked(*record, AttemptState::Dispatched,
                                         "assignment frame durably recorded as sent"));
  record->dispatched_epoch = impl_->epoch_;

  AttemptPackage package;
  package.attempt = record->id;
  package.attempt_generation = record->generation;
  package.assignment = record->assignment;
  package.population = population->id;
  package.population_generation = population->generation;
  package.task = task->id;
  package.task_generation = task->generation;
  package.candidate = record->candidate;
  package.candidate_generation = record->candidate_generation;
  package.task_name = task->name;
  package.objective = task->objective;
  package.required_outputs = task->required_outputs;
  for (const InputFile& input : task->inputs) {
    package.input_files.emplace_back(input.name, input.content);
  }
  package.workspace_root =
      path_to_utf8(attempt_workspace_path(impl_->config(), population->id, record->id));
  package.reference_strategy = "reference";
  return package;
}

Status FoundryCore::abandon_dispatch(const WorkerSessionAuthority& session, AttemptId attempt,
                                     std::string reason) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_session_locked(session));
  AttemptRecord* record = impl_->attempt_locked(attempt);
  if (record == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "attempt " + attempt.to_string() + " does not exist");
  }
  if (record->state != AttemptState::Authorized) {
    return Status(ErrorCode::IllegalStateTransition,
                  "only an authorized but unsent attempt can be abandoned");
  }
  impl_->release_attempt_reservations_locked(*record);
  AF_TRY(impl_->set_attempt_state_locked(*record, AttemptState::Cancelled, reason));
  impl_->statistics_.attempts_cancelled += 1;

  CandidateRecord* candidate = impl_->candidate_locked(record->candidate);
  if (candidate != nullptr && candidate->state == CandidateState::Producing) {
    AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::Registered, reason));
  }
  WorkerRecord* worker = impl_->worker_locked(record->worker);
  if (worker != nullptr && worker->active_attempt == attempt) {
    worker->state = WorkerState::Ready;
    worker->state_epoch = impl_->epoch_;
    worker->active_attempt = AttemptId();
    worker->active_assignment = AssignmentId();
  }
  return Status();
}

Status FoundryCore::acknowledge_attempt(const WorkerOperationAuthority& authority) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_operation_locked(authority));
  AttemptRecord* record = impl_->attempt_locked(authority.attempt);
  if (record == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "attempt does not exist");
  }
  if (record->state == AttemptState::Running) {
    return Status();
  }
  if (record->state != AttemptState::Dispatched) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::LateCompletion,
                  "attempt is " + std::string(attempt_state_name(record->state)) +
                      " and cannot be acknowledged");
  }
  AF_TRY(impl_->set_attempt_state_locked(*record, AttemptState::Running,
                                         "worker acknowledged the assignment"));
  const auto assignment = impl_->assignments_.find(record->assignment);
  if (assignment != impl_->assignments_.end()) {
    assignment->second.acknowledged = true;
    assignment->second.acknowledged_epoch = impl_->epoch_;
  }
  impl_->touch();
  return Status();
}

// ---------------------------------------------------------------------------
// Publication
// ---------------------------------------------------------------------------

Result<PublicationOutcome> FoundryCore::publish_candidate(
    const WorkerOperationAuthority& authority, std::vector<ArtifactRef> artifacts,
    std::string declared_strategy) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_operation_locked(authority));

  AttemptRecord* attempt = impl_->attempt_locked(authority.attempt);
  CandidateRecord* candidate = impl_->candidate_locked(authority.candidate);
  PopulationRecord* population = impl_->population_locked(authority.population);
  const TaskSpec* task = impl_->task_locked(authority.task);
  if (attempt == nullptr || candidate == nullptr || population == nullptr || task == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "publication references missing state");
  }

  if (candidate->state == CandidateState::ProductionCancelled ||
      candidate->state == CandidateState::Superseded ||
      candidate->state == CandidateState::Retired ||
      candidate->state == CandidateState::Disqualified ||
      candidate->state == CandidateState::ProductionFailed) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::LateCompletion,
                  "candidate " + candidate->id.to_string() + " is " +
                      std::string(candidate_state_name(candidate->state)) +
                      " and can no longer accept a publication");
  }
  if (attempt_state_is_terminal(attempt->state)) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::LateCompletion,
                  "attempt " + attempt->id.to_string() + " is " +
                      std::string(attempt_state_name(attempt->state)) +
                      " and can no longer publish");
  }
  if (attempt->state != AttemptState::Running && attempt->state != AttemptState::Dispatched) {
    impl_->statistics_.duplicate_rejections += 1;
    return Status(ErrorCode::DuplicateResult,
                  "attempt " + attempt->id.to_string() +
                      " already published a result for this candidate");
  }
  if (!candidate->artifact_set_digest.empty()) {
    impl_->statistics_.duplicate_rejections += 1;
    return Status(ErrorCode::DuplicateResult,
                  "candidate " + candidate->id.to_string() + " already has published output");
  }
  if (artifacts.size() > kMaxArtifactsPerCandidate) {
    return Status(ErrorCode::LengthOutOfRange, "too many artifacts declared");
  }
  for (const ArtifactRef& artifact : artifacts) {
    AF_TRY(validate_artifact_ref(artifact));
  }
  for (std::size_t left = 0; left < artifacts.size(); ++left) {
    for (std::size_t right = left + 1; right < artifacts.size(); ++right) {
      if (artifacts[left].name == artifacts[right].name) {
        return Status(ErrorCode::DuplicateIdentity,
                      "artifact name '" + artifacts[left].name + "' was declared twice");
      }
    }
  }
  for (const std::string& required : task->required_outputs) {
    bool present = false;
    for (const ArtifactRef& artifact : artifacts) {
      if (artifact.name == required) {
        present = true;
        break;
      }
    }
    if (!present) {
      return Status(ErrorCode::MandatoryEvidenceMissing,
                    "required output '" + required + "' was not declared by the candidate");
    }
  }
  AF_TRY(validate_worker_text(declared_strategy, "declared strategy"));

  candidate->artifacts = std::move(artifacts);
  candidate->artifact_set_digest = artifact_set_digest(candidate->artifacts);
  candidate->diversity_key = reference_diversity_key(declared_strategy, candidate->artifacts);
  candidate->producer_worker = attempt->worker;
  candidate->producer_boot = attempt->worker_boot;
  const EvidenceGeneration previous_evidence = candidate->evidence_generation;
  candidate->evidence_generation =
      previous_evidence.valid() ? EvidenceGeneration::from_value(previous_evidence.value() + 1)
                                : EvidenceGeneration::first();
  const bool replaced = candidate->generation.value() > 1;

  AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::Published,
                                           "worker output committed transactionally"));
  AF_TRY(impl_->set_attempt_state_locked(*attempt, AttemptState::Published,
                                         "candidate output committed"));

  BudgetLedger* ledger = impl_->ledger_locked(population->id);
  if (ledger != nullptr) {
    if (attempt->holds_reservation) {
      (void)ledger->consume(BudgetKind::ActiveAttempts);
      attempt->holds_reservation = false;
    }
    (void)ledger->consume(BudgetKind::CandidateAttempts);
    (void)ledger->consume(BudgetKind::WorkerAssignments);
    if (attempt->retry_index > 0) {
      (void)ledger->consume(BudgetKind::Retries);
    }
  }

  WorkerRecord* worker = impl_->worker_locked(authority.session.worker);
  if (worker != nullptr) {
    worker->state = WorkerState::Ready;
    worker->state_epoch = impl_->epoch_;
    worker->active_attempt = AttemptId();
    worker->active_assignment = AssignmentId();
    worker->assignments_completed += 1;
    worker->last_diagnostic = "candidate published";
  }

  impl_->statistics_.candidates_published += 1;
  if (population->state == PopulationState::Running) {
    AF_TRY(impl_->set_population_state_locked(*population, PopulationState::Evaluating,
                                              "candidate published; evaluation required"));
  }

  PublicationOutcome outcome;
  outcome.candidate = candidate->id;
  outcome.candidate_generation = candidate->generation;
  outcome.evidence_generation = candidate->evidence_generation;
  outcome.replaced_previous_output = replaced;
  return outcome;
}

Status FoundryCore::report_attempt_failure(const WorkerOperationAuthority& authority,
                                           std::string reason) {
  std::unique_lock lock(impl_->mutex);
  AF_TRY(impl_->validate_worker_operation_locked(authority));
  AttemptRecord* attempt = impl_->attempt_locked(authority.attempt);
  if (attempt == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "attempt does not exist");
  }
  if (attempt_state_is_terminal(attempt->state)) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::LateCompletion,
                  "attempt is already terminal in state " +
                      std::string(attempt_state_name(attempt->state)));
  }
  impl_->release_attempt_reservations_locked(*attempt);
  AF_TRY(impl_->set_attempt_state_locked(*attempt, AttemptState::Failed, reason));
  impl_->statistics_.attempts_failed += 1;

  CandidateRecord* candidate = impl_->candidate_locked(attempt->candidate);
  if (candidate != nullptr && (candidate->state == CandidateState::Producing ||
                               candidate->state == CandidateState::Registered)) {
    const BudgetLedger* ledger = impl_->ledger_locked(attempt->population);
    const std::uint32_t retries_left =
        ledger == nullptr ? 0u : ledger->remaining(BudgetKind::Retries);
    const bool retryable = candidate->attempt_count < kMaxAttemptsPerCandidate && retries_left > 0;
    AF_TRY(impl_->set_candidate_state_locked(
        *candidate, retryable ? CandidateState::Registered : CandidateState::ProductionFailed,
        retryable ? "attempt failed; the candidate slot remains retryable" : reason));
  }

  WorkerRecord* worker = impl_->worker_locked(authority.session.worker);
  if (worker != nullptr) {
    worker->state = WorkerState::Ready;
    worker->state_epoch = impl_->epoch_;
    worker->active_attempt = AttemptId();
    worker->active_assignment = AssignmentId();
    worker->assignments_failed += 1;
    worker->last_diagnostic = reason;
  }
  return Status();
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

Status FoundryCore::begin_evaluation(CandidateId id) {
  std::unique_lock lock(impl_->mutex);
  CandidateRecord* candidate = impl_->candidate_locked(id);
  if (candidate == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "candidate " + id.to_string() + " does not exist");
  }
  if (candidate->state != CandidateState::Published &&
      candidate->state != CandidateState::Evaluating) {
    return Status(ErrorCode::IllegalStateTransition,
                  "candidate " + id.to_string() + " is " +
                      std::string(candidate_state_name(candidate->state)) +
                      " and is not awaiting evaluation");
  }
  const PopulationRecord* population = impl_->population_locked(candidate->population);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "candidate population does not exist");
  }
  AF_TRY(impl_->require_live_population_authority_locked(*population, "begin an evaluation"));
  AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::Evaluating,
                                           "evaluation started"));
  for (auto& entry : impl_->attempts_) {
    AttemptRecord& attempt = entry.second;
    if (attempt.candidate != id || attempt.state != AttemptState::Published) {
      continue;
    }
    AF_TRY(impl_->set_attempt_state_locked(attempt, AttemptState::Evaluating,
                                           "candidate under evaluation"));
  }
  return Status();
}

Status FoundryCore::record_evaluation(EvaluationRecord record) {
  std::unique_lock lock(impl_->mutex);

  CandidateRecord* candidate = impl_->candidate_locked(record.candidate);
  if (candidate == nullptr) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::UnknownIdentity,
                  "evaluation names candidate " + record.candidate.to_string() +
                      " which does not exist");
  }
  if (record.candidate_generation != candidate->generation) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::StaleCandidateGeneration,
                  "evaluation is bound to candidate generation " +
                      record.candidate_generation.to_string() + " but the candidate is at " +
                      candidate->generation.to_string());
  }
  if (candidate->state == CandidateState::ProductionCancelled ||
      candidate->state == CandidateState::Superseded ||
      candidate->state == CandidateState::Retired ||
      candidate->state == CandidateState::Disqualified ||
      candidate->state == CandidateState::ProductionFailed) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::LateCompletion,
                  "candidate " + candidate->id.to_string() + " is " +
                      std::string(candidate_state_name(candidate->state)) +
                      " and cannot accept new evidence");
  }
  const PopulationRecord* population = impl_->population_locked(record.population);
  if (population == nullptr || population->id != candidate->population) {
    return Status(ErrorCode::StaleAuthority, "evaluation names the wrong population");
  }
  if (record.task_generation != population->task_generation) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::StaleTaskGeneration,
                  "evaluation is bound to task generation " +
                      record.task_generation.to_string() + " but the population task is at " +
                      population->task_generation.to_string());
  }
  if (record.population_generation != population->generation) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::StalePopulationGeneration,
                  "evaluation is bound to population generation " +
                      record.population_generation.to_string() + " but the population is at " +
                      population->generation.to_string());
  }
  // The population owns the authority that makes this evidence meaningful. Once
  // it is cancelled, closed or failed, every later publication path must reject
  // consistently, so a candidate that was already published when the population
  // was cancelled can never accept fresh evidence and reach a state the
  // population is no longer able to select from. This is the backstop: the more
  // specific generation mismatches above are reported as themselves first.
  {
    const Status population_authority =
        impl_->require_live_population_authority_locked(*population, "accept new evidence");
    if (!population_authority.ok()) {
      impl_->statistics_.late_rejections += 1;
      return population_authority;
    }
  }

  for (const auto& entry : impl_->evaluations_) {
    const EvaluationRecord& existing = entry.second;
    if (existing.candidate == record.candidate &&
        existing.candidate_generation == record.candidate_generation &&
        existing.evaluator_key == record.evaluator_key && existing.complete && record.complete) {
      impl_->statistics_.duplicate_rejections += 1;
      return Status(ErrorCode::DuplicateResult,
                    "evaluator '" + record.evaluator_key +
                        "' already produced a complete record for this candidate generation");
    }
  }

  if (!record.id.valid()) {
    EvaluationId allocated;
    AF_TRY_ASSIGN(allocated, impl_->ids_.next<EvaluationIdTag>());
    record.id = allocated;
  }
  if (!record.generation.valid()) {
    record.generation = EvaluationGeneration::first();
  }
  record.population = population->id;
  record.population_generation = population->generation;
  record.task = population->task;
  record.decided_epoch = impl_->epoch_;
  if (!record.complete) {
    record.complete = true;
  }
  AF_TRY(validate_evaluation_record(record));

  impl_->evaluations_.emplace(record.id, record);
  impl_->statistics_.evaluations_recorded += 1;
  impl_->settle_candidate_locked(*candidate);
  impl_->touch();
  return Status();
}

Result<std::vector<EvaluationRequirement>> FoundryCore::pending_evaluations(CandidateId id) const {
  std::shared_lock lock(impl_->mutex);
  const CandidateRecord* candidate = impl_->candidate_locked(id);
  if (candidate == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "candidate " + id.to_string() + " does not exist");
  }
  const TaskSpec* task = impl_->task_locked(candidate->task);
  if (task == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "candidate task does not exist");
  }
  std::vector<EvaluationRequirement> pending;
  for (const EvaluationRequirement& requirement : task->requirements) {
    if (!impl_->requirement_complete_locked(*candidate, requirement.evaluator_key)) {
      pending.push_back(requirement);
    }
  }
  return pending;
}

std::vector<CandidateId> FoundryCore::candidates_awaiting_evaluation() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<CandidateId> result;
  for (const PopulationId population_id : impl_->ordered_populations_locked()) {
    const PopulationRecord* population = impl_->population_locked(population_id);
    if (population == nullptr || population_state_is_terminal(population->state)) {
      continue;
    }
    if (population->state == PopulationState::RevalidationRequired) {
      continue;
    }
    for (const CandidateId candidate_id : impl_->ordered_candidates_locked(population_id)) {
      const CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
      if (candidate == nullptr) {
        continue;
      }
      if (candidate->state != CandidateState::Published &&
          candidate->state != CandidateState::Evaluating) {
        continue;
      }
      const TaskSpec* task = impl_->task_locked(candidate->task);
      if (task == nullptr) {
        continue;
      }
      if (task->requirements.empty()) {
        continue;
      }
      bool complete = true;
      for (const EvaluationRequirement& requirement : task->requirements) {
        if (!impl_->requirement_complete_locked(*candidate, requirement.evaluator_key)) {
          complete = false;
          break;
        }
      }
      if (!complete) {
        result.push_back(candidate_id);
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

Status FoundryCore::cancel_attempt(AttemptId id, std::string reason) {
  std::unique_lock lock(impl_->mutex);
  AttemptRecord* attempt = impl_->attempt_locked(id);
  if (attempt == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "attempt " + id.to_string() + " does not exist");
  }
  if (attempt->state == AttemptState::Cancelled) {
    return Status();
  }
  if (attempt_state_is_terminal(attempt->state)) {
    impl_->statistics_.late_rejections += 1;
    return Status(ErrorCode::LateCompletion,
                  "attempt " + id.to_string() + " is already terminal in state " +
                      std::string(attempt_state_name(attempt->state)));
  }
  impl_->release_attempt_reservations_locked(*attempt);
  AF_TRY(impl_->set_attempt_state_locked(*attempt, AttemptState::Cancelled, reason));
  impl_->statistics_.attempts_cancelled += 1;

  CandidateRecord* candidate = impl_->candidate_locked(attempt->candidate);
  if (candidate != nullptr && (candidate->state == CandidateState::Producing ||
                               candidate->state == CandidateState::Registered ||
                               candidate->state == CandidateState::Published ||
                               candidate->state == CandidateState::Evaluating)) {
    // An explicit cancellation is final for the candidate, and it is final
    // wherever the candidate happens to be: a candidate that is already under
    // evaluation must not be able to reach Evaluated and enter ranking on the
    // strength of an attempt that was cancelled underneath it. This is
    // deliberately different from the ambiguous outcome of a worker death,
    // which leaves the slot retryable rather than deciding for the worker.
    AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::ProductionCancelled,
                                             "attempt cancelled: " + reason));
  }
  WorkerRecord* worker = impl_->worker_locked(attempt->worker);
  if (worker != nullptr && worker->active_attempt == id) {
    worker->state = WorkerState::Ready;
    worker->state_epoch = impl_->epoch_;
    worker->active_attempt = AttemptId();
    worker->active_assignment = AssignmentId();
  }
  return Status();
}

Status FoundryCore::force_attempt_outcome_unknown(AttemptId id, std::string reason) {
  std::unique_lock lock(impl_->mutex);
  AttemptRecord* attempt = impl_->attempt_locked(id);
  if (attempt == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "attempt " + id.to_string() + " does not exist");
  }
  if (attempt_state_is_terminal(attempt->state)) {
    return Status(ErrorCode::LateCompletion,
                  "attempt " + id.to_string() + " is already terminal");
  }
  impl_->release_attempt_reservations_locked(*attempt);
  AF_TRY(impl_->set_attempt_state_locked(*attempt, AttemptState::OutcomeUnknown, reason));
  impl_->statistics_.attempts_outcome_unknown += 1;
  CandidateRecord* candidate = impl_->candidate_locked(attempt->candidate);
  if (candidate != nullptr && (candidate->state == CandidateState::Producing ||
                               candidate->state == CandidateState::Registered)) {
    AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::Registered, reason));
  }
  return Status();
}

}  // namespace autonomous_foundry
