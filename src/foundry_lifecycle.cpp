#include "foundry_core_impl.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/hash.hpp"
#include "autonomous_foundry/workspace.hpp"

namespace autonomous_foundry {
namespace {

/// Build the isolated workspace root for one attempt. The path is derived only
/// from coordinator-minted decimal counters, so no worker-supplied text can
/// influence it.
std::filesystem::path attempt_workspace(const FoundryConfig& config, PopulationId population,
                                        AttemptId attempt) {
  std::filesystem::path root = config.workspace_root;
  root /= "population-" + std::to_string(population.counter());
  root /= "attempt-" + std::to_string(attempt.counter());
  return root;
}

}  // namespace

// ---------------------------------------------------------------------------
// Population construction
// ---------------------------------------------------------------------------

Result<PopulationId> FoundryCore::Impl::create_population_locked(const PopulationSpec& spec) {
  AF_TRY(validate_worker_text(spec.name, "population name"));

  const TaskSpec* task = task_locked(spec.task);
  if (task == nullptr) {
    return Status(ErrorCode::UnknownIdentity,
                  "task " + spec.task.to_string() + " does not exist");
  }
  AF_TRY(check_task_generation(spec.task_generation, task->generation));
  const FoundryPolicy* policy = policy_locked(spec.policy);
  if (policy == nullptr) {
    return Status(ErrorCode::UnknownIdentity,
                  "policy " + spec.policy.to_string() + " does not exist");
  }
  AF_TRY(check_policy_generation(spec.policy_generation, policy->generation));

  if (spec.candidate_budget == 0 || spec.candidate_budget > kMaxCandidatesPerPopulation) {
    return Status(ErrorCode::LengthOutOfRange,
                  "candidate budget must be between 1 and " +
                      std::to_string(kMaxCandidatesPerPopulation));
  }
  if (spec.worker_budget == 0 || spec.worker_budget > policy->budgets.max_workers) {
    return Status(ErrorCode::LengthOutOfRange,
                  "worker budget must be between 1 and the policy worker limit " +
                      std::to_string(policy->budgets.max_workers));
  }
  if (spec.candidate_budget > policy->budgets.max_candidates) {
    return Status(ErrorCode::BudgetExhausted,
                  "candidate budget exceeds the policy limit " +
                      std::to_string(policy->budgets.max_candidates));
  }

  if (spec.population_index > 1) {
    const PopulationRecord* predecessor = population_locked(spec.predecessor);
    if (predecessor == nullptr) {
      return Status(ErrorCode::UnknownIdentity, "predecessor population does not exist");
    }
    if (predecessor->state != PopulationState::Closed) {
      return Status(ErrorCode::IllegalStateTransition,
                    "predecessor population must be closed before it can be advanced");
    }
    if (predecessor->population_index + 1 != spec.population_index) {
      return Status(ErrorCode::GenerationRegression,
                    "population index " + std::to_string(spec.population_index) +
                        " does not directly follow the predecessor index " +
                        std::to_string(predecessor->population_index));
    }
    const auto selection = selections_.find(predecessor->id);
    if (selection == selections_.end() ||
        selection->second.state != SelectionDecisionState::Committed ||
        !selection->second.selected.valid()) {
      return Status(ErrorCode::SelectionImpossible,
                    "predecessor population has no committed selected winner to seed from");
    }
    if (selection->second.selected != spec.seed_candidate) {
      return Status(ErrorCode::StaleAuthority,
                    "seed candidate does not match the predecessor's committed winner");
    }
    const CandidateRecord* seed = candidate_locked(spec.seed_candidate);
    if (seed == nullptr) {
      return Status(ErrorCode::UnknownIdentity, "seed candidate does not exist");
    }
    if (seed->generation != spec.seed_candidate_generation) {
      return Status(ErrorCode::StaleCandidateGeneration,
                    "seed candidate generation does not match current state");
    }
    if (spec.seed_lineage != seed->lineage) {
      return Status(ErrorCode::StaleAuthority, "seed lineage does not match the winner lineage");
    }
  } else if (spec.predecessor.valid() || spec.seed_candidate.valid()) {
    return Status(ErrorCode::InvalidArgument,
                  "the first population of a run cannot name a predecessor or seed candidate");
  }

  PopulationId id;
  AF_TRY_ASSIGN(id, ids_.next<PopulationIdTag>());

  PopulationRecord record;
  record.id = id;
  record.generation = PopulationGeneration::first();
  record.run = run_;
  record.name = spec.name;
  record.task = spec.task;
  record.task_generation = spec.task_generation;
  record.policy = spec.policy;
  record.policy_generation = spec.policy_generation;
  record.candidate_budget = spec.candidate_budget;
  record.worker_budget = spec.worker_budget;
  record.population_index = spec.population_index;
  record.predecessor = spec.predecessor;
  record.seed_candidate = spec.seed_candidate;
  record.seed_candidate_generation = spec.seed_candidate_generation;
  record.seed_lineage = spec.seed_lineage;
  record.require_promotion_request = spec.require_promotion_request;
  record.state = PopulationState::Created;
  record.created_epoch = epoch_;
  record.state_epoch = epoch_;

  populations_.emplace(id, record);
  ledgers_.emplace(id, BudgetLedger(policy->budgets));
  statistics_.populations_created += 1;
  touch();
  return id;
}

Status FoundryCore::Impl::close_population_locked(PopulationId id) {
  PopulationRecord* population = population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  if (population->state == PopulationState::Closed) {
    return Status();
  }
  if (population_state_is_terminal(population->state)) {
    return Status(ErrorCode::LifecycleClosed,
                  "population " + id.to_string() + " is already terminal in state " +
                      std::string(population_state_name(population->state)));
  }

  std::vector<std::string> blockers;
  for (const auto& entry : attempts_) {
    const AttemptRecord& attempt = entry.second;
    if (attempt.population == id && !attempt_state_is_terminal(attempt.state)) {
      blockers.push_back("attempt " + attempt.id.to_string() + " is " +
                         std::string(attempt_state_name(attempt.state)));
    }
  }
  if (!population->committed_selection.valid()) {
    blockers.push_back("no committed selection decision");
  }
  if (!population->retention_committed) {
    blockers.push_back("no committed retention decision");
  }
  if (population->require_promotion_request && !population->committed_promotion_request.valid()) {
    blockers.push_back("promotion eligibility is unresolved");
  }
  for (const CandidateId candidate_id : population->candidates) {
    const CandidateRecord* candidate = candidate_locked(candidate_id);
    if (candidate == nullptr) {
      continue;
    }
    switch (candidate->state) {
      case CandidateState::Registered:
      case CandidateState::Producing:
      case CandidateState::Published:
      case CandidateState::Evaluating:
      case CandidateState::RevalidationRequired:
        blockers.push_back("candidate " + candidate_id.to_string() + " is " +
                           std::string(candidate_state_name(candidate->state)));
        break;
      default:
        break;
    }
  }
  const auto ledger = ledgers_.find(id);
  if (ledger != ledgers_.end() && ledger->second.open_reservations() != 0) {
    blockers.push_back(std::to_string(ledger->second.open_reservations()) +
                       " budget reservation(s) are still open");
  }

  if (!blockers.empty()) {
    population->closure_blockers = blockers;
    std::string message = "population " + id.to_string() + " cannot close:";
    for (const std::string& blocker : blockers) {
      message.append(" [").append(blocker).append("]");
    }
    return Status(ErrorCode::IllegalStateTransition, message);
  }
  population->closure_blockers.clear();
  AF_TRY(set_population_state_locked(*population, PopulationState::Closing,
                                     "closure contract satisfied"));
  AF_TRY(set_population_state_locked(*population, PopulationState::Closed,
                                     "population closed"));
  population->generation = PopulationGeneration::from_value(population->generation.value() + 1);
  statistics_.populations_closed += 1;
  return Status();
}

Status FoundryCore::Impl::add_elite_candidate_locked(PopulationRecord* successor,
                                                     const CandidateRecord& winner) {
  if (successor == nullptr) {
    return Status(ErrorCode::InvalidArgument, "elite carry-forward requires a successor population");
  }
  const LineageNode* parent = lineage_.find(winner.id);
  if (parent == nullptr) {
    return Status(ErrorCode::LineageOrphan, "elite carry-forward requires a lineage node");
  }
  BudgetLedger* ledger = ledger_locked(successor->id);
  if (ledger == nullptr) {
    return Status(ErrorCode::Internal, "successor population has no accounting ledger");
  }
  AF_TRY(ledger->reserve(BudgetKind::Candidates));
  AF_TRY(ledger->consume(BudgetKind::Candidates));

  CandidateId id;
  AF_TRY_ASSIGN(id, ids_.next<CandidateIdTag>());

  CandidateRecord elite;
  elite.id = id;
  elite.generation = CandidateGeneration::first();
  elite.population = successor->id;
  elite.population_generation = successor->generation;
  elite.task = successor->task;
  elite.task_generation = successor->task_generation;
  elite.lineage = winner.lineage;
  elite.parents.push_back(winner.id);
  elite.depth = winner.depth + 1;
  if (elite.depth > kMaxLineageDepth) {
    return Status(ErrorCode::GenerationRegression, "elite carry-forward exceeds the lineage depth bound");
  }
  elite.producer_worker = winner.producer_worker;
  elite.producer_boot = winner.producer_boot;
  elite.created_epoch = epoch_;
  elite.state_changed_epoch = epoch_;
  elite.state = CandidateState::Published;
  elite.artifacts = winner.artifacts;
  elite.artifact_set_digest = winner.artifact_set_digest;
  elite.evidence_generation = EvidenceGeneration::first();
  elite.diversity_key = winner.diversity_key;
  elite.retained = false;
  elite.selected = false;

  LineageNode node;
  node.candidate = id;
  node.candidate_generation = elite.generation;
  node.lineage = elite.lineage;
  node.parents = elite.parents;
  node.depth = elite.depth;
  node.created_epoch = epoch_;
  AF_TRY(lineage_.insert(node));

  successor->candidates.push_back(id);
  candidates_.emplace(id, elite);
  statistics_.candidate_slots_created += 1;
  touch();
  return Status();
}

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

Result<TaskId> FoundryCore::define_task(TaskSpec spec) {
  std::unique_lock lock(impl_->mutex);
  if (!spec.id.valid()) {
    TaskId allocated;
    AF_TRY_ASSIGN(allocated, impl_->ids_.next<TaskIdTag>());
    spec.id = allocated;
  }
  if (impl_->tasks_.find(spec.id) != impl_->tasks_.end()) {
    impl_->statistics_.duplicate_rejections += 1;
    return Status(ErrorCode::DuplicateIdentity, "task " + spec.id.to_string() + " already exists");
  }
  if (!spec.generation.valid()) {
    spec.generation = TaskGeneration::first();
  } else if (spec.generation != TaskGeneration::first()) {
    return Status(ErrorCode::InvalidArgument, "a newly defined task must start at generation 1");
  }
  AF_TRY(validate_task(spec));
  std::string digest;
  AF_TRY_ASSIGN(digest, compute_task_digest(spec));
  spec.content_digest = std::move(digest);
  impl_->tasks_.emplace(spec.id, spec);
  impl_->touch();
  return spec.id;
}

Result<TaskGeneration> FoundryCore::revise_task(TaskId id, TaskSpec replacement) {
  std::unique_lock lock(impl_->mutex);
  TaskSpec* existing = impl_->task_locked(id);
  if (existing == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "task " + id.to_string() + " does not exist");
  }
  replacement.id = id;
  const std::optional<TaskGeneration> next = existing->generation.next();
  if (!next.has_value()) {
    return Status(ErrorCode::GenerationRegression, "task generation cannot advance further");
  }
  replacement.generation = *next;
  AF_TRY(validate_task(replacement));
  std::string digest;
  AF_TRY_ASSIGN(digest, compute_task_digest(replacement));
  replacement.content_digest = std::move(digest);
  *existing = std::move(replacement);
  impl_->statistics_.task_revisions += 1;
  impl_->touch();
  return existing->generation;
}

Result<PolicyId> FoundryCore::define_policy(FoundryPolicy policy) {
  std::unique_lock lock(impl_->mutex);
  if (!policy.id.valid()) {
    PolicyId allocated;
    AF_TRY_ASSIGN(allocated, impl_->ids_.next<PolicyIdTag>());
    policy.id = allocated;
  }
  if (impl_->policies_.find(policy.id) != impl_->policies_.end()) {
    impl_->statistics_.duplicate_rejections += 1;
    return Status(ErrorCode::DuplicateIdentity,
                  "policy " + policy.id.to_string() + " already exists");
  }
  if (!policy.generation.valid()) {
    policy.generation = PolicyGeneration::first();
  } else if (policy.generation != PolicyGeneration::first()) {
    return Status(ErrorCode::InvalidArgument, "a newly defined policy must start at generation 1");
  }
  AF_TRY(validate_policy(policy));
  std::string digest;
  AF_TRY_ASSIGN(digest, compute_policy_digest(policy));
  policy.content_digest = std::move(digest);
  impl_->policies_.emplace(policy.id, policy);
  impl_->touch();
  return policy.id;
}

Result<PolicyGeneration> FoundryCore::revise_policy(PolicyId id, FoundryPolicy replacement) {
  std::unique_lock lock(impl_->mutex);
  FoundryPolicy* existing = impl_->policy_locked(id);
  if (existing == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "policy " + id.to_string() + " does not exist");
  }
  replacement.id = id;
  const std::optional<PolicyGeneration> next = existing->generation.next();
  if (!next.has_value()) {
    return Status(ErrorCode::GenerationRegression, "policy generation cannot advance further");
  }
  replacement.generation = *next;
  AF_TRY(validate_policy(replacement));
  std::string digest;
  AF_TRY_ASSIGN(digest, compute_policy_digest(replacement));
  replacement.content_digest = std::move(digest);
  *existing = std::move(replacement);
  impl_->statistics_.policy_revisions += 1;
  impl_->touch();
  return existing->generation;
}

Result<PopulationId> FoundryCore::create_population(const PopulationSpec& spec) {
  std::unique_lock lock(impl_->mutex);
  return impl_->create_population_locked(spec);
}

Status FoundryCore::start_population(PopulationId id, PopulationGeneration generation) {
  std::unique_lock lock(impl_->mutex);
  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  AF_TRY(check_population_generation(generation, population->generation));
  if (population->state != PopulationState::Created && population->state != PopulationState::Ready) {
    return Status(ErrorCode::IllegalStateTransition,
                  "population " + id.to_string() + " is " +
                      std::string(population_state_name(population->state)) +
                      " and cannot start");
  }
  AF_TRY(impl_->set_population_state_locked(*population, PopulationState::Ready,
                                            "start requested"));
  AF_TRY(impl_->set_population_state_locked(*population, PopulationState::Running,
                                            "population is accepting production work"));
  population->generation = PopulationGeneration::from_value(population->generation.value() + 1);
  return Status();
}

Status FoundryCore::cancel_population(PopulationId id, PopulationGeneration generation,
                                      std::string reason) {
  std::unique_lock lock(impl_->mutex);
  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  if (population_state_is_terminal(population->state)) {
    return Status(ErrorCode::LifecycleClosed,
                  "population is already terminal in state " +
                      std::string(population_state_name(population->state)));
  }
  AF_TRY(check_population_generation(generation, population->generation));

  for (auto& entry : impl_->attempts_) {
    AttemptRecord& attempt = entry.second;
    if (attempt.population != id || attempt_state_is_terminal(attempt.state)) {
      continue;
    }
    impl_->release_attempt_reservations_locked(attempt);
    AF_TRY(impl_->set_attempt_state_locked(attempt, AttemptState::Cancelled, reason));
    impl_->statistics_.attempts_cancelled += 1;
    CandidateRecord* candidate = impl_->candidate_locked(attempt.candidate);
    if (candidate != nullptr &&
        (candidate->state == CandidateState::Registered ||
         candidate->state == CandidateState::Producing)) {
      AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::ProductionCancelled,
                                               "population cancelled: " + reason));
    }
    WorkerRecord* worker = impl_->worker_locked(attempt.worker);
    if (worker != nullptr && worker->active_attempt == attempt.id) {
      worker->active_attempt = AttemptId();
      worker->active_assignment = AssignmentId();
      worker->state = WorkerState::Ready;
      worker->state_epoch = impl_->epoch_;
    }
  }

  AF_TRY(impl_->set_population_state_locked(*population, PopulationState::Cancelled, reason));
  population->generation = PopulationGeneration::from_value(population->generation.value() + 1);
  return Status();
}

Status FoundryCore::request_population_revalidation(PopulationId id, std::string reason) {
  std::unique_lock lock(impl_->mutex);
  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  if (population_state_is_terminal(population->state)) {
    return Status(ErrorCode::LifecycleClosed,
                  "population is already terminal in state " +
                      std::string(population_state_name(population->state)));
  }
  AF_TRY(impl_->set_population_state_locked(*population, PopulationState::RevalidationRequired,
                                            reason));
  for (const CandidateId candidate_id : population->candidates) {
    CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      continue;
    }
    if (candidate->state == CandidateState::Evaluated ||
        candidate->state == CandidateState::Published ||
        candidate->state == CandidateState::Evaluating) {
      AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::RevalidationRequired,
                                               reason));
    }
  }
  population->generation = PopulationGeneration::from_value(population->generation.value() + 1);
  return Status();
}

Status FoundryCore::revalidate_population(PopulationId id, PopulationGeneration generation,
                                          std::string detail) {
  std::unique_lock lock(impl_->mutex);
  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  if (population->state != PopulationState::RevalidationRequired) {
    return Status(ErrorCode::IllegalStateTransition,
                  "population " + id.to_string() + " is " +
                      std::string(population_state_name(population->state)) +
                      " and does not require revalidation");
  }
  AF_TRY(check_population_generation(generation, population->generation));

  // Evidence that was already complete and bound to the current candidate and
  // task generations stays valid history. Evidence that was never completed
  // simply does not exist, so re-entering Evaluating causes the coordinator to
  // run only the missing requirements again.
  for (const CandidateId candidate_id : population->candidates) {
    CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      continue;
    }
    if (candidate->state == CandidateState::RevalidationRequired) {
      AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::Evaluating,
                                               "revalidated: " + detail));
      impl_->settle_candidate_locked(*candidate);
    }
  }

  PopulationState target = PopulationState::Running;
  for (const CandidateId candidate_id : population->candidates) {
    const CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      continue;
    }
    if (candidate->state == CandidateState::Published ||
        candidate->state == CandidateState::Evaluating) {
      target = PopulationState::Evaluating;
      break;
    }
  }
  AF_TRY(impl_->set_population_state_locked(*population, target, "revalidated: " + detail));
  population->generation = PopulationGeneration::from_value(population->generation.value() + 1);
  return Status();
}

}  // namespace autonomous_foundry
