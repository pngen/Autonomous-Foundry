#include "foundry_core_impl.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "autonomous_foundry/authority.hpp"

namespace autonomous_foundry {

const FoundryConfig& FoundryCore::config() const noexcept { return impl_->config(); }

Status FoundryCore::recover(FoundrySnapshot snapshot) {
  std::unique_lock lock(impl_->mutex);

  if (snapshot.foundry.valid() && impl_->foundry_.valid() &&
      snapshot.foundry != impl_->foundry_) {
    return Status(ErrorCode::StaleAuthority,
                  "snapshot belongs to foundry " + snapshot.foundry.to_string() +
                      " but this process owns " + impl_->foundry_.to_string());
  }
  if (snapshot.run.valid() && impl_->run_.valid() && snapshot.run != impl_->run_) {
    return Status(ErrorCode::StaleAuthority,
                  "snapshot belongs to foundry run " + snapshot.run.to_string() +
                      " but this process owns " + impl_->run_.to_string());
  }

  // The coordinator incarnation advances. Every authority issued under the
  // previous epoch is dead from this point on, whether or not the process that
  // holds it is still alive.
  const std::optional<CoordinatorEpoch> next_epoch =
      snapshot.epoch.valid() ? snapshot.epoch.next()
                             : std::optional<CoordinatorEpoch>(CoordinatorEpoch::from_value(1));
  if (!next_epoch.has_value()) {
    return Status(ErrorCode::GenerationRegression,
                  "the coordinator epoch cannot advance beyond its numeric limit");
  }

  impl_->foundry_ = snapshot.foundry.valid() ? snapshot.foundry : impl_->foundry_;
  impl_->run_ = snapshot.run.valid() ? snapshot.run : impl_->run_;
  impl_->epoch_ = *next_epoch;
  impl_->revision_ = snapshot.revision;

  impl_->tasks_ = std::move(snapshot.tasks);
  impl_->policies_ = std::move(snapshot.policies);
  impl_->populations_ = std::move(snapshot.populations);
  impl_->candidates_ = std::move(snapshot.candidates);
  impl_->workers_ = std::move(snapshot.workers);
  impl_->attempts_ = std::move(snapshot.attempts);
  impl_->assignments_ = std::move(snapshot.assignments);
  impl_->evaluations_ = std::move(snapshot.evaluations);
  impl_->selections_ = std::move(snapshot.selections);
  impl_->retentions_ = std::move(snapshot.retentions);
  impl_->promotion_requests_ = std::move(snapshot.promotion_requests);
  impl_->ledgers_ = std::move(snapshot.ledgers);
  impl_->lineage_.restore(std::move(snapshot.lineage.nodes()));
  impl_->statistics_ = snapshot.statistics;

  // Identity allocation resumes from the durable counters so that no identity
  // that already exists can ever be issued again.
  //
  // The declared counter is a lower bound, never the whole truth. Two cases
  // make it too low to be used as it stands:
  //
  //   * worker and worker-boot identities are minted by the peer, not by the
  //     coordinator allocator, so those declared counters are legitimately zero
  //     while the restored tables hold live peer identities;
  //   * an embedder may hand recover() a snapshot it built itself, in which
  //     case the declared counters are whatever that caller wrote.
  //
  // Seeding every domain with the highest counter the restored state actually
  // contains is the one rule that holds in both cases. It is also what keeps a
  // recovered core's own snapshot readable: a core whose allocator sat below an
  // identity it already held would emit an image its own validator must refuse,
  // which is exactly the durability hole this closes.
  const std::array<std::uint32_t, kIdKindCount> restored_counters =
      observed_identity_counters(snapshot);

  impl_->ids_ = IdAllocator(snapshot.id_salt == 0 ? impl_->ids_.salt() : snapshot.id_salt);
  for (std::size_t index = 1; index < kIdKindCount; ++index) {
    std::uint32_t seed = snapshot.id_counters[index];
    if (restored_counters[index] > seed) {
      seed = restored_counters[index];
    }
    AF_TRY(impl_->ids_.restore_counter(static_cast<IdKind>(index), seed));
  }

  // 1. Live worker authority does not survive. A recovered worker is offline
  // and must present a fresh session before it may act.
  for (auto& entry : impl_->workers_) {
    WorkerRecord& worker = entry.second;
    worker.state = WorkerState::Offline;
    worker.state_epoch = impl_->epoch_;
    worker.active_attempt = AttemptId();
    worker.active_attempt_generation = AttemptGeneration();
    worker.active_assignment = AssignmentId();
    worker.last_diagnostic = "coordinator restarted at epoch " + impl_->epoch_.to_string();
  }

  // 2. Attempts that were never dispatched are cancelled. Attempts that were in
  // flight become ambiguous: the foundry cannot tell whether the work happened,
  // so it refuses to claim either outcome.
  for (auto& entry : impl_->attempts_) {
    AttemptRecord& attempt = entry.second;
    if (attempt.state == AttemptState::Created || attempt.state == AttemptState::Authorized) {
      impl_->release_attempt_reservations_locked(attempt);
      attempt.state = AttemptState::Cancelled;
      attempt.terminal_epoch = impl_->epoch_;
      attempt.failure_detail = "coordinator restarted before the assignment was dispatched";
      ++impl_->statistics_.attempts_cancelled;
    } else if (attempt.state == AttemptState::Dispatched ||
               attempt.state == AttemptState::Running) {
      impl_->release_attempt_reservations_locked(attempt);
      attempt.state = AttemptState::OutcomeUnknown;
      attempt.terminal_epoch = impl_->epoch_;
      attempt.failure_detail =
          "coordinator restarted with the assignment in flight; completion is undetermined";
      ++impl_->statistics_.attempts_outcome_unknown;
    }
  }

  // 3. Candidate slots whose attempt is now terminal become retryable.
  // Candidates whose output was published but whose evaluation did not finish
  // require explicit revalidation rather than silently inheriting readiness.
  for (auto& entry : impl_->candidates_) {
    CandidateRecord& candidate = entry.second;
    if (candidate.state == CandidateState::Registered ||
        candidate.state == CandidateState::Producing) {
      bool open = false;
      for (const auto& attempt_entry : impl_->attempts_) {
        const AttemptRecord& attempt = attempt_entry.second;
        if (attempt.candidate == candidate.id && !attempt_state_is_terminal(attempt.state)) {
          open = true;
          break;
        }
      }
      if (!open) {
        candidate.state = CandidateState::Registered;
        candidate.state_changed_epoch = impl_->epoch_;
        candidate.failure_reason = "coordinator restarted; the candidate slot is retryable";
      }
    } else if (candidate.state == CandidateState::Published ||
               candidate.state == CandidateState::Evaluating) {
      candidate.state = CandidateState::RevalidationRequired;
      candidate.state_changed_epoch = impl_->epoch_;
      candidate.failure_reason =
          "coordinator restarted during evaluation; evidence must be re-established";
    }
  }

  // 4. Decisions that were prepared but never committed were never
  // authoritative and are discarded rather than resurrected.
  for (auto iterator = impl_->selections_.begin(); iterator != impl_->selections_.end();) {
    if (iterator->second.state != SelectionDecisionState::Committed) {
      iterator = impl_->selections_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (auto iterator = impl_->retentions_.begin(); iterator != impl_->retentions_.end();) {
    if (!iterator->second.committed) {
      iterator = impl_->retentions_.erase(iterator);
    } else {
      ++iterator;
    }
  }

  // 5. Every live population must be explicitly revalidated before it may make
  // another authoritative decision.
  for (auto& entry : impl_->populations_) {
    PopulationRecord& population = entry.second;
    if (population_state_is_terminal(population.state) ||
        population.state == PopulationState::Created) {
      continue;
    }
    population.state = PopulationState::RevalidationRequired;
    population.state_epoch = impl_->epoch_;
    population.status_detail =
        "coordinator restart at epoch " + impl_->epoch_.to_string() +
        "; explicit revalidation is required before further authoritative progress";
    if (population.committed_selection.valid()) {
      const auto selection = impl_->selections_.find(population.id);
      if (selection == impl_->selections_.end()) {
        population.committed_selection = SelectionGeneration();
        population.retention_committed = false;
        population.status_detail.append("; the committed selection record is missing");
      }
    }
  }

  impl_->touch();
  return Status();
}

std::vector<std::string> FoundryCore::audit() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<std::string> violations;

  for (const auto& entry : impl_->populations_) {
    const PopulationRecord& population = entry.second;
    if (!population.generation.valid()) {
      violations.push_back("population " + population.id.to_string() +
                           " has an invalid generation");
    }
    if (population.candidates.size() > kMaxCandidatesPerPopulation) {
      violations.push_back("population " + population.id.to_string() +
                           " lists more candidates than the bound allows");
    }
    std::vector<CandidateId> sorted = population.candidates;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      violations.push_back("population " + population.id.to_string() +
                           " lists a candidate more than once");
    }
    for (const CandidateId candidate_id : population.candidates) {
      const CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
      if (candidate == nullptr) {
        violations.push_back("population " + population.id.to_string() + " references missing " +
                             candidate_id.to_string());
        continue;
      }
      if (candidate->population != population.id) {
        violations.push_back(candidate_id.to_string() + " names a different population");
      }
    }
    if (population.state == PopulationState::Closed && !population.committed_selection.valid()) {
      violations.push_back("population " + population.id.to_string() +
                           " is closed without a committed selection");
    }
    if (population.state == PopulationState::Closed && !population.retention_committed) {
      violations.push_back("population " + population.id.to_string() +
                           " is closed without a committed retention decision");
    }
  }

  for (const auto& entry : impl_->candidates_) {
    const CandidateRecord& candidate = entry.second;
    if (!candidate.generation.valid()) {
      violations.push_back("candidate " + candidate.id.to_string() +
                           " has an invalid generation");
    }
    if (candidate.parents.size() > kMaxCandidateParents) {
      violations.push_back("candidate " + candidate.id.to_string() + " declares too many parents");
    }
    for (const CandidateId parent : candidate.parents) {
      if (parent == candidate.id) {
        violations.push_back("candidate " + candidate.id.to_string() + " is its own parent");
      }
      if (!impl_->lineage_.contains(parent)) {
        violations.push_back("candidate " + candidate.id.to_string() + " names missing parent " +
                             parent.to_string());
      }
    }
    if (candidate.selected && candidate.state != CandidateState::Selected &&
        candidate.state != CandidateState::Retained && candidate.state != CandidateState::Retired) {
      violations.push_back("candidate " + candidate.id.to_string() +
                           " is marked selected while in state " +
                           std::string(candidate_state_name(candidate.state)));
    }
    if (!candidate.artifacts.empty() &&
        candidate.artifact_set_digest != artifact_set_digest(candidate.artifacts)) {
      violations.push_back("candidate " + candidate.id.to_string() +
                           " has an artifact set digest that does not match its artifacts");
    }
  }

  for (const auto& entry : impl_->attempts_) {
    const AttemptRecord& attempt = entry.second;
    if (impl_->candidate_locked(attempt.candidate) == nullptr) {
      violations.push_back("attempt " + attempt.id.to_string() + " references missing " +
                           attempt.candidate.to_string());
    }
    if (impl_->population_locked(attempt.population) == nullptr) {
      violations.push_back("attempt " + attempt.id.to_string() + " references missing " +
                           attempt.population.to_string());
    }
    if (impl_->assignments_.find(attempt.assignment) == impl_->assignments_.end()) {
      violations.push_back("attempt " + attempt.id.to_string() + " references missing " +
                           attempt.assignment.to_string());
    }
  }

  for (const auto& entry : impl_->assignments_) {
    if (impl_->attempt_locked(entry.second.attempt) == nullptr) {
      violations.push_back("assignment " + entry.first.to_string() + " references missing " +
                           entry.second.attempt.to_string());
    }
  }

  for (const auto& entry : impl_->evaluations_) {
    const EvaluationRecord& record = entry.second;
    if (impl_->candidate_locked(record.candidate) == nullptr) {
      violations.push_back("evaluation " + record.id.to_string() + " references missing " +
                           record.candidate.to_string());
    }
    if (impl_->task_locked(record.task) == nullptr) {
      violations.push_back("evaluation " + record.id.to_string() + " references missing " +
                           record.task.to_string());
    }
    if (record.complete && evaluator_kind_is_authoritative(record.kind) &&
        record.outcome == EvaluationOutcome::Pass) {
      const CandidateRecord* candidate = impl_->candidate_locked(record.candidate);
      if (candidate != nullptr && record.candidate_generation != candidate->generation) {
        violations.push_back("evaluation " + record.id.to_string() +
                             " is bound to a superseded candidate generation");
      }
    }
  }

  for (const auto& entry : impl_->selections_) {
    const SelectionDecision& decision = entry.second;
    if (impl_->population_locked(entry.first) == nullptr) {
      violations.push_back("selection for " + entry.first.to_string() +
                           " references a missing population");
    }
    if (decision.state == SelectionDecisionState::Committed && decision.selected.valid()) {
      const CandidateRecord* winner = impl_->candidate_locked(decision.selected);
      if (winner == nullptr) {
        violations.push_back("committed selection names a missing winner");
      } else if (!winner->selected) {
        violations.push_back("committed selection winner " + winner->id.to_string() +
                             " is not marked selected");
      }
    }
  }

  // At most one authoritative selection winner per population generation.
  for (const auto& entry : impl_->populations_) {
    const PopulationRecord& population = entry.second;
    std::size_t winners = 0;
    for (const CandidateId candidate_id : population.candidates) {
      const CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
      if (candidate != nullptr && candidate->selected) {
        ++winners;
      }
    }
    if (winners > 1) {
      violations.push_back("population " + population.id.to_string() + " has " +
                           std::to_string(winners) + " selected candidates");
    }
  }

  for (const auto& entry : impl_->ledgers_) {
    if (impl_->population_locked(entry.first) == nullptr) {
      violations.push_back("ledger for " + entry.first.to_string() +
                           " references a missing population");
    }
  }

  const Status lineage_status = impl_->lineage_.validate();
  if (!lineage_status.ok()) {
    violations.push_back("lineage: " + lineage_status.message());
  }

  if (!impl_->epoch_.valid()) {
    violations.push_back("the coordinator epoch is invalid");
  }
  return violations;
}

}  // namespace autonomous_foundry
