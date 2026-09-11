#include "foundry_core_impl.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "autonomous_foundry/hash.hpp"

namespace autonomous_foundry {
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

/// Numeric value a ranking factor contributes for one candidate.
struct FactorSample {
  bool available{false};
  double raw{0.0};
};

FactorSample sample_factor(RankingFactorKind kind, const CandidateRecord& candidate,
                           const std::vector<EvaluationRecord>& evaluations,
                           const TaskSpec& task, std::size_t complete_requirements) {
  FactorSample sample;
  switch (kind) {
    case RankingFactorKind::QualityScore: {
      double total = 0.0;
      std::size_t scored = 0;
      for (const EvaluationRecord& record : evaluations) {
        if (record.requirement_class != RequirementClass::Optional) {
          continue;
        }
        if (!record.complete || !record.has_score) {
          continue;
        }
        total += record.score;
        ++scored;
      }
      std::size_t optional_count = 0;
      for (const EvaluationRequirement& requirement : task.requirements) {
        if (requirement.requirement_class == RequirementClass::Optional) {
          ++optional_count;
        }
      }
      if (optional_count == 0) {
        sample.available = false;
        sample.raw = 0.0;
        return sample;
      }
      sample.available = scored != 0;
      sample.raw = total / static_cast<double>(optional_count);
      return sample;
    }
    case RankingFactorKind::EfficiencyScore: {
      for (const EvaluationRecord& record : evaluations) {
        if (record.evaluator_key != "performance") {
          continue;
        }
        if (!record.complete || !record.has_score) {
          continue;
        }
        sample.available = true;
        sample.raw = record.score;
        return sample;
      }
      return sample;
    }
    case RankingFactorKind::EvaluationCompleteness: {
      if (task.requirements.empty()) {
        sample.available = false;
        return sample;
      }
      sample.available = true;
      sample.raw = static_cast<double>(complete_requirements) /
                   static_cast<double>(task.requirements.size());
      return sample;
    }
    case RankingFactorKind::LineageDepth:
      sample.available = true;
      sample.raw = static_cast<double>(candidate.depth);
      return sample;
    case RankingFactorKind::AttemptCount:
      sample.available = true;
      sample.raw = static_cast<double>(candidate.attempt_count);
      return sample;
    case RankingFactorKind::ArtifactBytes: {
      std::uint64_t bytes = 0;
      for (const ArtifactRef& artifact : candidate.artifacts) {
        bytes += artifact.size_bytes;
      }
      sample.available = true;
      sample.raw = static_cast<double>(bytes);
      return sample;
    }
    case RankingFactorKind::CandidateIdTieBreak:
      sample.available = true;
      sample.raw = static_cast<double>(candidate.id.raw());
      return sample;
  }
  return sample;
}

void add_exclusion(SelectionDecision& decision, const CandidateRecord& candidate,
                   SelectionStage stage, ExclusionReason reason, std::string detail) {
  ExclusionEntry entry;
  entry.candidate = candidate.id;
  entry.candidate_generation = candidate.generation;
  entry.stage = stage;
  entry.reason = reason;
  entry.detail = std::move(detail);
  decision.excluded.push_back(std::move(entry));
}

}  // namespace

// ---------------------------------------------------------------------------
// Canonical state digests
// ---------------------------------------------------------------------------

std::string compute_selection_state_digest(const PopulationRecord& population,
                                           const TaskSpec& task, const FoundryPolicy& policy,
                                           const std::vector<CandidateRecord>& candidates,
                                           const std::vector<EvaluationRecord>& evaluations) {
  std::string encoded;
  append_u64(encoded, "population", population.id.raw());
  append_u64(encoded, "population_generation", population.generation.value());
  append_u64(encoded, "task", task.id.raw());
  append_u64(encoded, "task_generation", task.generation.value());
  append_field(encoded, "task_digest", task.content_digest);
  append_u64(encoded, "policy", policy.id.raw());
  append_u64(encoded, "policy_generation", policy.generation.value());
  append_field(encoded, "policy_digest", policy.content_digest);
  append_u64(encoded, "candidate_count", candidates.size());

  // Deliberately excluded: population lifecycle state, worker state, era
  // timestamps and statistics. Those change without changing what a selection
  // decision was derived from. Everything that can make the decision wrong is
  // included.
  std::vector<CandidateRecord> ordered = candidates;
  std::sort(ordered.begin(), ordered.end(),
            [](const CandidateRecord& a, const CandidateRecord& b) { return a.id < b.id; });
  for (const CandidateRecord& candidate : ordered) {
    std::vector<EvaluationRecord> owned;
    for (const EvaluationRecord& record : evaluations) {
      if (record.candidate == candidate.id) {
        owned.push_back(record);
      }
    }
    encoded.append(encode_candidate_for_digest(candidate, owned));
    encoded.push_back('#');
  }
  return sha256_hex(encoded);
}

std::string compute_retention_state_digest(const PopulationRecord& population,
                                           const FoundryPolicy& policy,
                                           const SelectionDecision& selection) {
  std::string encoded;
  append_u64(encoded, "population", population.id.raw());
  append_u64(encoded, "population_generation", population.generation.value());
  append_u64(encoded, "policy", policy.id.raw());
  append_u64(encoded, "policy_generation", policy.generation.value());
  append_field(encoded, "policy_digest", policy.content_digest);
  append_u64(encoded, "selection_generation", selection.generation.value());
  append_field(encoded, "selection_digest", selection.canonical_state_digest);
  append_u64(encoded, "ranked_count", selection.ranking.size());
  for (const RankingEntry& entry : selection.ranking) {
    append_u64(encoded, "ranked", entry.candidate.raw());
    append_field(encoded, "score", canonical_double(entry.total_score));
  }
  append_u64(encoded, "retain_top_k", policy.retention.retain_top_k);
  append_u64(encoded, "max_retained", policy.retention.max_retained);
  append_u64(encoded, "max_per_lineage", policy.retention.max_per_lineage);
  return sha256_hex(encoded);
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

Result<SelectionDecision> FoundryCore::prepare_selection(PopulationId id) {
  std::unique_lock lock(impl_->mutex);

  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  if (population->state == PopulationState::RevalidationRequired) {
    return Status(ErrorCode::RevalidationRequired,
                  "population " + id.to_string() +
                      " requires revalidation before it may select");
  }
  if (population->state != PopulationState::Running &&
      population->state != PopulationState::Evaluating &&
      population->state != PopulationState::Selecting &&
      population->state != PopulationState::Advancing) {
    return Status(ErrorCode::IllegalStateTransition,
                  "population " + id.to_string() + " is " +
                      std::string(population_state_name(population->state)) +
                      " and cannot select");
  }

  const TaskSpec* task = impl_->task_locked(population->task);
  if (task == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population task is missing");
  }
  const FoundryPolicy* policy = impl_->policy_locked(population->policy);
  if (policy == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population policy is missing");
  }

  SelectionDecision decision;
  decision.population = population->id;
  decision.population_generation = population->generation;
  decision.task = task->id;
  decision.task_generation = task->generation;
  decision.policy = policy->id;
  decision.policy_generation = policy->generation;
  decision.coordinator_epoch = impl_->epoch_;
  decision.state = SelectionDecisionState::Prepared;

  std::vector<CandidateRecord> participants;
  std::vector<EvaluationRecord> evidence;

  for (const CandidateId candidate_id : impl_->ordered_candidates_locked(id)) {
    CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      ExclusionEntry entry;
      entry.candidate = candidate_id;
      entry.stage = SelectionStage::CandidateLifecycle;
      entry.reason = ExclusionReason::UnknownCandidate;
      entry.detail = "candidate is referenced by the population but missing from state";
      decision.excluded.push_back(std::move(entry));
      continue;
    }

    // Stage: candidate lifecycle.
    switch (candidate->state) {
      case CandidateState::Superseded:
        add_exclusion(decision, *candidate, SelectionStage::CandidateLifecycle,
                      ExclusionReason::Superseded, "candidate was superseded");
        continue;
      case CandidateState::Retired:
        add_exclusion(decision, *candidate, SelectionStage::CandidateLifecycle,
                      ExclusionReason::Retired, "candidate was retired");
        continue;
      case CandidateState::Disqualified:
        add_exclusion(decision, *candidate, SelectionStage::CandidateLifecycle,
                      ExclusionReason::LifecycleNotEligible,
                      "candidate was disqualified by a hard gate");
        continue;
      case CandidateState::ProductionFailed:
      case CandidateState::ProductionCancelled:
        add_exclusion(decision, *candidate, SelectionStage::CandidateLifecycle,
                      ExclusionReason::LifecycleNotEligible,
                      "candidate never produced a published output");
        continue;
      case CandidateState::RevalidationRequired:
        add_exclusion(decision, *candidate, SelectionStage::CandidateLifecycle,
                      ExclusionReason::EvidenceGenerationMismatch,
                      "candidate evidence must be rebuilt after revalidation");
        continue;
      case CandidateState::Registered:
      case CandidateState::Producing:
        // No output has been committed, so there is nothing to rank and no
        // evidence that could be about it.
        add_exclusion(decision, *candidate, SelectionStage::CandidateLifecycle,
                      ExclusionReason::LifecycleNotEligible,
                      "candidate has not finished evaluation");
        continue;
      case CandidateState::Published:
      case CandidateState::Evaluating:
        // The candidate has committed output; whether it has proven enough to be
        // ranked is the next stage's question, not this one. Both states must be
        // admitted: record_evaluation() only advances the lifecycle record to
        // Evaluating once the whole declared evidence set is complete, so a
        // candidate whose mandatory gates are already recorded can still read as
        // Published, and a candidate with all mandatory gates complete but
        // optional evidence still in flight reads as Evaluating. Excluding
        // either of them made SelectionPolicy::require_complete_mandatory
        // unreachable: no candidate could ever be ranked before every optional
        // requirement had a complete record, which is exactly what that policy
        // flag exists to decide.
        break;
      case CandidateState::Evaluated:
      case CandidateState::Selected:
      case CandidateState::Retained:
        break;
    }

    // Stage: task compatibility.
    if (candidate->task_generation != task->generation) {
      add_exclusion(decision, *candidate, SelectionStage::TaskCompatibility,
                    ExclusionReason::TaskIncompatible,
                    "candidate was produced under task generation " +
                        candidate->task_generation.to_string() + " but the task is now at " +
                        task->generation.to_string());
      continue;
    }

    // Stage: required evidence and hard constraints. A hard gate is evaluated
    // before ranking, so a high soft score cannot rescue a failed gate.
    bool eligible = true;
    const std::vector<EvaluationRecord> candidate_records =
        impl_->evaluations_for_locked(candidate->id);
    for (const EvaluationRequirement& requirement : task->requirements) {
      if (requirement.requirement_class != RequirementClass::Mandatory) {
        continue;
      }
      const EvaluationRecord* record =
          impl_->requirement_record_locked(*candidate, requirement.evaluator_key);
      if (record == nullptr) {
        // SelectionPolicy::require_complete_mandatory answers exactly one
        // question: whether a mandatory gate that has no complete authoritative
        // record yet removes the candidate before ranking, or is merely priced
        // as a missing factor. Optional requirements never remove a candidate.
        if (!policy->selection.require_complete_mandatory) {
          continue;
        }
        add_exclusion(decision, *candidate, SelectionStage::RequiredEvidence,
                      ExclusionReason::MandatoryEvidenceMissing,
                      "mandatory requirement '" + requirement.evaluator_key +
                          "' has no complete authoritative record for candidate generation " +
                          candidate->generation.to_string());
        eligible = false;
        break;
      }
      if (record->outcome != EvaluationOutcome::Pass) {
        SelectionStage stage = SelectionStage::RequiredEvidence;
        ExclusionReason reason = ExclusionReason::MandatoryEvaluationFailed;
        switch (record->outcome) {
          case EvaluationOutcome::Fail:
            // A mandatory gate that was evaluated and failed is a hard
            // constraint the candidate violated: it is excluded before ranking
            // and no soft score can rescue it. Missing evidence is a different
            // stage, because there the question is whether the evidence set is
            // finished rather than whether the candidate satisfied the gate.
            reason = ExclusionReason::MandatoryEvaluationFailed;
            stage = SelectionStage::HardConstraints;
            break;
          case EvaluationOutcome::Unknown:
            reason = ExclusionReason::MandatoryEvaluationUnknown;
            break;
          case EvaluationOutcome::Unsupported:
            reason = ExclusionReason::MandatoryEvaluationUnsupported;
            stage = SelectionStage::HardConstraints;
            break;
          case EvaluationOutcome::Error:
            reason = ExclusionReason::MandatoryEvaluationError;
            stage = SelectionStage::HardConstraints;
            break;
          case EvaluationOutcome::Cancelled:
            reason = ExclusionReason::MandatoryEvaluationCancelled;
            break;
          case EvaluationOutcome::Pass:
            break;
        }
        add_exclusion(decision, *candidate, stage, reason,
                      "mandatory requirement '" + requirement.evaluator_key + "' reported " +
                          std::string(evaluation_outcome_name(record->outcome)));
        eligible = false;
        break;
      }
    }
    if (!eligible) {
      continue;
    }

    // Optional evidence that is still in flight is not an eligibility question:
    // it is exactly what the EvaluationCompleteness ranking factor exists to
    // price, and an unfinished optional requirement must not be
    // indistinguishable from a failed hard gate. Reproducibility does not
    // depend on completeness either - the canonical state digest covers the
    // evidence set a decision was derived from, whatever its size.
    participants.push_back(*candidate);
    for (const EvaluationRecord& record : candidate_records) {
      evidence.push_back(record);
    }
  }

  // Stage: policy feasibility, then ranking.
  //
  // The digest covers the one canonical candidate/evidence set for this
  // population in both the impossible path and the ranked path. commit_selection
  // revalidates against exactly that set, so a decision derived from a smaller
  // participant list could never be committed - and, conversely, an excluded
  // candidate can never be mistaken for a change of state.
  const Impl::SelectionCanonicalInputs canonical =
      impl_->selection_canonical_inputs_locked(population->id);

  if (participants.empty()) {
    decision.state = SelectionDecisionState::Impossible;
    decision.rationale =
        "no candidate satisfied every mandatory gate; ranking was not entered";
    decision.canonical_state_digest = compute_selection_state_digest(
        *population, *task, *policy, canonical.candidates, canonical.evaluations);
    impl_->selections_[population->id] = decision;
    impl_->statistics_.selections_prepared += 1;
    AF_TRY(impl_->set_population_state_locked(*population, PopulationState::Selecting,
                                              "selection prepared with no eligible candidate"));
    return decision;
  }

  std::vector<RankingEntry> ranking;
  ranking.reserve(participants.size());
  for (const CandidateRecord& candidate : participants) {
    RankingEntry entry;
    entry.candidate = candidate.id;
    entry.candidate_generation = candidate.generation;
    entry.lineage_depth = candidate.depth;
    entry.diversity_key = candidate.diversity_key;
    for (const ArtifactRef& artifact : candidate.artifacts) {
      entry.artifact_bytes += artifact.size_bytes;
    }

    std::vector<EvaluationRecord> owned;
    for (const EvaluationRecord& record : evidence) {
      if (record.candidate == candidate.id) {
        owned.push_back(record);
      }
    }
    const std::size_t complete = impl_->complete_requirement_count_locked(candidate);

    double total = 0.0;
    for (const RankingFactor& factor : policy->selection.factors) {
      RankingFactorValue value;
      value.key = factor.key;
      value.kind = factor.kind;
      value.direction = factor.direction;
      value.weight = factor.weight;
      const FactorSample sample = sample_factor(factor.kind, candidate, owned, *task, complete);
      value.available = sample.available;
      value.raw_value = sample.raw;
      if (factor.kind == RankingFactorKind::CandidateIdTieBreak) {
        value.contribution = 0.0;
      } else if (sample.available) {
        const double signed_value = factor.direction == RankingDirection::HigherIsBetter
                                        ? sample.raw
                                        : -sample.raw;
        value.contribution = factor.weight * signed_value;
      } else {
        value.contribution = 0.0;
      }
      total += value.contribution;
      entry.factors.push_back(std::move(value));
    }
    entry.total_score = total;
    ranking.push_back(std::move(entry));
  }

  // Deterministic ordering: total score descending, then identity ascending.
  // The identity comparison is exact, so the tie-break is a total order and
  // never depends on floating point.
  std::sort(ranking.begin(), ranking.end(),
            [](const RankingEntry& a, const RankingEntry& b) {
              if (a.total_score != b.total_score) {
                return a.total_score > b.total_score;
              }
              return a.candidate < b.candidate;
            });
  for (std::size_t index = 0; index < ranking.size(); ++index) {
    ranking[index].rank = static_cast<std::uint32_t>(index + 1);
  }

  decision.ranking = ranking;
  decision.selected = ranking.front().candidate;
  decision.selected_generation = ranking.front().candidate_generation;
  decision.state = SelectionDecisionState::Prepared;
  decision.canonical_state_digest = compute_selection_state_digest(
      *population, *task, *policy, canonical.candidates, canonical.evaluations);
  decision.rationale = "ranked " + std::to_string(ranking.size()) +
                       " eligible candidate(s) over " +
                       std::to_string(decision.excluded.size()) +
                       " excluded candidate(s); winner " + decision.selected.to_string() +
                       " with total score " + canonical_double(ranking.front().total_score);

  impl_->selections_[population->id] = decision;
  impl_->statistics_.selections_prepared += 1;
  AF_TRY(impl_->set_population_state_locked(*population, PopulationState::Selecting,
                                            "selection prepared"));
  return decision;
}

Result<SelectionDecision> FoundryCore::commit_selection(const SelectionDecision& prepared) {
  std::unique_lock lock(impl_->mutex);

  PopulationRecord* population = impl_->population_locked(prepared.population);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population does not exist");
  }
  AF_TRY(check_population_generation(prepared.population_generation, population->generation));
  AF_TRY(check_policy_generation(prepared.policy_generation, population->policy_generation));
  AF_TRY(check_task_generation(prepared.task_generation, population->task_generation));
  AF_TRY(check_identity(prepared.task, population->task, "task"));
  AF_TRY(check_identity(prepared.policy, population->policy, "policy"));

  const TaskSpec* task = impl_->task_locked(population->task);
  const FoundryPolicy* policy = impl_->policy_locked(population->policy);
  if (task == nullptr || policy == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population task or policy is missing");
  }

  // Revalidate the prepared decision against the exact canonical state it was
  // derived from. A candidate mutation between prepare and commit changes this
  // digest and the decision is refused instead of being applied to state it
  // never saw.
  const Impl::SelectionCanonicalInputs canonical =
      impl_->selection_canonical_inputs_locked(population->id);
  const std::string current_digest = compute_selection_state_digest(
      *population, *task, *policy, canonical.candidates, canonical.evaluations);
  AF_TRY(check_canonical_digest(prepared.canonical_state_digest, current_digest));

  SelectionDecision committed = prepared;
  committed.state = SelectionDecisionState::Committed;
  const SelectionGeneration previous = population->committed_selection;
  committed.generation = previous.valid()
                             ? SelectionGeneration::from_value(previous.value() + 1)
                             : SelectionGeneration::first();

  // Clear any previous winner so that at most one authoritative selection
  // exists per population generation.
  for (const CandidateId candidate_id : population->candidates) {
    CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      continue;
    }
    if (candidate->selected && candidate->id != committed.selected) {
      candidate->selected = false;
      candidate->selection_generation = SelectionGeneration();
    }
  }

  if (committed.selected.valid()) {
    CandidateRecord* winner = impl_->candidate_locked(committed.selected);
    if (winner == nullptr) {
      return Status(ErrorCode::UnknownIdentity, "selected candidate is missing from state");
    }
    // Selected is only reachable from Evaluated; Published and Evaluating have
    // no edge to it. Selection may legitimately admit a candidate whose
    // mandatory gates are complete while its optional evidence is still in
    // flight, so the winner can still be Published or Evaluating here. Walking
    // the declared edges records the fact that admission to ranking already
    // established - every declared mandatory requirement has a complete
    // authoritative record - rather than leaving a committed winner in a state
    // that denies it won. A winner whose mandatory gates are not complete (only
    // reachable from a policy that does not require complete mandatory
    // evidence) is refused the transition instead of being recorded as
    // evaluated.
    bool mandatory_gates_complete = true;
    for (const EvaluationRequirement& requirement : task->requirements) {
      if (requirement.requirement_class != RequirementClass::Mandatory) {
        continue;
      }
      if (!impl_->requirement_complete_locked(*winner, requirement.evaluator_key)) {
        mandatory_gates_complete = false;
        break;
      }
    }
    if (mandatory_gates_complete) {
      if (winner->state == CandidateState::Published) {
        AF_TRY(impl_->set_candidate_state_locked(*winner, CandidateState::Evaluating,
                                                 "won selection; evaluation state caught up"));
      }
      if (winner->state == CandidateState::Evaluating) {
        AF_TRY(impl_->set_candidate_state_locked(*winner, CandidateState::Evaluated,
                                                 "won selection; every mandatory gate is satisfied"));
      }
    }
    AF_TRY(impl_->set_candidate_state_locked(*winner, CandidateState::Selected,
                                             "won selection for this population generation"));
    winner->selected = true;
    winner->selection_generation = committed.generation;
  }

  population->committed_selection = committed.generation;
  impl_->selections_[population->id] = committed;
  impl_->statistics_.selections_committed += 1;
  AF_TRY(impl_->set_population_state_locked(*population, PopulationState::Advancing,
                                            "selection committed"));
  return committed;
}

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

Result<RetentionDecision> FoundryCore::prepare_retention(PopulationId id) {
  std::unique_lock lock(impl_->mutex);

  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  const auto selection_entry = impl_->selections_.find(id);
  if (!population->committed_selection.valid() || selection_entry == impl_->selections_.end() ||
      selection_entry->second.state != SelectionDecisionState::Committed) {
    return Status(ErrorCode::SelectionImpossible,
                  "retention requires a committed selection decision for population " +
                      id.to_string());
  }
  // A retention decision that is already committed for the selection generation
  // this population currently holds is authoritative. Re-preparing it must
  // return the committed decision rather than overwrite it with a fresh,
  // uncommitted one that would read as "retention is still undecided".
  {
    const auto stored = impl_->retentions_.find(id);
    if (stored != impl_->retentions_.end() && stored->second.committed &&
        population->retention_committed &&
        stored->second.selection_generation == population->committed_selection) {
      return stored->second;
    }
  }

  const FoundryPolicy* policy = impl_->policy_locked(population->policy);
  if (policy == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population policy is missing");
  }
  const SelectionDecision& selection = selection_entry->second;

  RetentionDecision decision;
  decision.selection_generation = selection.generation;
  decision.population = population->id;
  decision.population_generation = population->generation;
  decision.policy = policy->id;
  decision.policy_generation = policy->generation;
  decision.coordinator_epoch = impl_->epoch_;

  std::vector<CandidateId> retained;
  std::vector<std::pair<LineageId, std::uint32_t>> lineage_counts;

  auto lineage_count = [&lineage_counts](LineageId lineage) -> std::uint32_t& {
    for (auto& entry : lineage_counts) {
      if (entry.first == lineage) {
        return entry.second;
      }
    }
    lineage_counts.emplace_back(lineage, 0u);
    return lineage_counts.back().second;
  };

  auto try_retain = [&](const RankingEntry& entry, RetentionOutcome reason,
                        std::string detail) -> bool {
    const CandidateRecord* candidate = impl_->candidate_locked(entry.candidate);
    if (candidate == nullptr) {
      return false;
    }
    if (retained.size() >= policy->retention.max_retained) {
      RetentionDecisionEntry rejected;
      rejected.candidate = entry.candidate;
      rejected.candidate_generation = entry.candidate_generation;
      rejected.outcome = RetentionOutcome::RetiredCapacityLimit;
      rejected.rank = entry.rank;
      rejected.detail = "population retention capacity reached";
      decision.entries.push_back(std::move(rejected));
      return false;
    }
    std::uint32_t& count = lineage_count(candidate->lineage);
    if (count >= policy->retention.max_per_lineage) {
      RetentionDecisionEntry rejected;
      rejected.candidate = entry.candidate;
      rejected.candidate_generation = entry.candidate_generation;
      rejected.outcome = RetentionOutcome::RetiredLineageCap;
      rejected.rank = entry.rank;
      rejected.detail = "lineage retention cap reached";
      decision.entries.push_back(std::move(rejected));
      return false;
    }
    ++count;
    retained.push_back(entry.candidate);
    RetentionDecisionEntry kept;
    kept.candidate = entry.candidate;
    kept.candidate_generation = entry.candidate_generation;
    kept.outcome = reason;
    kept.rank = entry.rank;
    kept.detail = std::move(detail);
    decision.entries.push_back(std::move(kept));
    return true;
  };

  const bool has_winner = selection.selected.valid();
  const bool retain_winner = has_winner && policy->retention.retain_selected;

  // RetentionPolicy::retain_top_k is documented as the number of best-ranked
  // eligible candidates retained INCLUDING the winner. Retaining the winner
  // first and then letting the rank cut below count it - it is already in the
  // retained list when that loop runs - is what makes retained.size() <=
  // retain_top_k hold by construction. The old order skipped the winner in the
  // loop and appended it afterwards, which let the retained set exceed
  // retain_top_k by exactly one. Offering the winner the first slot also keeps
  // the policy's guarantee to it from being lost to a lineage or capacity
  // rejection incurred by a lower-ranked candidate; validate_policy()
  // guarantees retain_top_k >= 1 and max_per_lineage >= 1, so a valid policy
  // always leaves it a slot.
  const auto retain_ranked = [&](const RankingEntry& entry) {
    if (retained.size() >= policy->retention.retain_top_k) {
      RetentionDecisionEntry below;
      below.candidate = entry.candidate;
      below.candidate_generation = entry.candidate_generation;
      below.outcome = RetentionOutcome::RetiredRankedBelowCut;
      below.rank = entry.rank;
      below.detail = "ranked below the retention cut";
      decision.entries.push_back(std::move(below));
      return;
    }
    (void)try_retain(entry, RetentionOutcome::Retained, "retained by rank");
  };

  // The winner is the front of the ranking it won, so this pass always reaches
  // it; the loop over the remaining ranked candidates then fills whatever slots
  // the cut still has.
  for (const RankingEntry& entry : selection.ranking) {
    if (retain_winner && entry.candidate == selection.selected) {
      (void)try_retain(entry, RetentionOutcome::RetainedBecauseSelected,
                       "selected winner is retained by policy");
      break;
    }
  }
  for (const RankingEntry& entry : selection.ranking) {
    if (retain_winner && entry.candidate == selection.selected) {
      continue;
    }
    retain_ranked(entry);
  }

  for (const CandidateId candidate_id : population->candidates) {
    if (std::find(retained.begin(), retained.end(), candidate_id) != retained.end()) {
      continue;
    }
    bool described = false;
    for (const RetentionDecisionEntry& entry : decision.entries) {
      if (entry.candidate == candidate_id) {
        described = true;
        break;
      }
    }
    if (described) {
      continue;
    }
    const CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    RetentionDecisionEntry entry;
    entry.candidate = candidate_id;
    entry.candidate_generation =
        candidate == nullptr ? CandidateGeneration() : candidate->generation;
    entry.outcome = RetentionOutcome::RetiredIneligible;
    entry.detail = "candidate was not eligible for retention";
    decision.entries.push_back(std::move(entry));
  }

  std::sort(retained.begin(), retained.end());
  decision.retained = retained;
  for (const RetentionDecisionEntry& entry : decision.entries) {
    if (entry.outcome == RetentionOutcome::RetiredRankedBelowCut ||
        entry.outcome == RetentionOutcome::RetiredLineageCap ||
        entry.outcome == RetentionOutcome::RetiredCapacityLimit ||
        entry.outcome == RetentionOutcome::RetiredIneligible) {
      decision.retired.push_back(entry.candidate);
    }
  }
  std::sort(decision.retired.begin(), decision.retired.end());
  decision.canonical_state_digest = compute_retention_state_digest(*population, *policy, selection);
  decision.committed = false;

  impl_->retentions_[population->id] = decision;
  return decision;
}

Result<RetentionDecision> FoundryCore::commit_retention(const RetentionDecision& prepared) {
  std::unique_lock lock(impl_->mutex);

  PopulationRecord* population = impl_->population_locked(prepared.population);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population does not exist");
  }
  AF_TRY(check_population_generation(prepared.population_generation, population->generation));
  AF_TRY(check_policy_generation(prepared.policy_generation, population->policy_generation));
  if (population->committed_selection != prepared.selection_generation) {
    return Status(ErrorCode::StaleSelection,
                  "retention names selection generation " +
                      prepared.selection_generation.to_string() + " but the population committed " +
                      population->committed_selection.to_string());
  }
  const auto selection_entry = impl_->selections_.find(population->id);
  if (selection_entry == impl_->selections_.end()) {
    return Status(ErrorCode::StaleSelection, "committed selection decision is missing");
  }
  const FoundryPolicy* policy = impl_->policy_locked(population->policy);
  if (policy == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population policy is missing");
  }
  const std::string current_digest =
      compute_retention_state_digest(*population, *policy, selection_entry->second);
  AF_TRY(check_canonical_digest(prepared.canonical_state_digest, current_digest));

  RetentionDecision committed = prepared;
  committed.committed = true;

  for (CandidateId candidate_id : committed.retained) {
    CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      return Status(ErrorCode::UnknownIdentity, "retained candidate is missing from state");
    }
    candidate->retained = true;
  }
  for (CandidateId candidate_id : committed.retired) {
    CandidateRecord* candidate = impl_->candidate_locked(candidate_id);
    if (candidate == nullptr) {
      continue;
    }
    if (candidate->id == selection_entry->second.selected) {
      continue;
    }
    // Retention only decides what to keep. It may not force a state change a
    // candidate cannot legally make: a candidate that never published, or that
    // already reached a terminal state, has no live authority to retire and is
    // already recorded in history. Requiring Retired here made a population
    // holding any such candidate permanently unclosable.
    if (!candidate_transition_is_legal(candidate->state, CandidateState::Retired)) {
      continue;
    }
    AF_TRY(impl_->set_candidate_state_locked(*candidate, CandidateState::Retired,
                                             "retention policy released this candidate"));
  }

  population->retention_committed = true;
  population->retention_selection_generation = committed.selection_generation;
  impl_->retentions_[population->id] = committed;
  impl_->statistics_.retentions_committed += 1;
  impl_->touch();
  return committed;
}

// ---------------------------------------------------------------------------
// Promotion eligibility
// ---------------------------------------------------------------------------

Result<PromotionRequest> FoundryCore::prepare_promotion_request(PopulationId id) {
  std::unique_lock lock(impl_->mutex);

  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  if (!population->committed_selection.valid() || !population->retention_committed) {
    return Status(ErrorCode::PromotionNotEligible,
                  "promotion eligibility requires committed selection and retention");
  }
  const auto selection_entry = impl_->selections_.find(id);
  if (selection_entry == impl_->selections_.end() ||
      selection_entry->second.state != SelectionDecisionState::Committed) {
    return Status(ErrorCode::PromotionNotEligible, "no committed selection decision");
  }
  const SelectionDecision& selection = selection_entry->second;
  if (!selection.selected.valid()) {
    return Status(ErrorCode::PromotionNotEligible, "the selection decision has no winner");
  }
  const CandidateRecord* winner = impl_->candidate_locked(selection.selected);
  if (winner == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "selected candidate is missing from state");
  }
  const TaskSpec* task = impl_->task_locked(population->task);
  const FoundryPolicy* policy = impl_->policy_locked(population->policy);
  if (task == nullptr || policy == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population task or policy is missing");
  }

  PromotionRequest request;
  PromotionRequestId request_id;
  AF_TRY_ASSIGN(request_id, impl_->ids_.next<PromotionRequestIdTag>());
  request.id = request_id;
  request.foundry = impl_->foundry_;
  request.run = impl_->run_;
  request.coordinator_epoch = impl_->epoch_;
  request.population = population->id;
  request.population_generation = population->generation;
  request.candidate = winner->id;
  request.candidate_generation = winner->generation;
  request.task = task->id;
  request.task_generation = task->generation;
  request.lineage = winner->lineage;
  request.selection_generation = selection.generation;
  request.policy = policy->id;
  request.policy_generation = policy->generation;
  request.evidence_generation = winner->evidence_generation;
  request.artifacts = winner->artifacts;
  request.artifact_set_digest = winner->artifact_set_digest;
  for (const ArtifactRef& artifact : winner->artifacts) {
    request.total_artifact_bytes += artifact.size_bytes;
  }
  request.lineage_depth = winner->depth;
  // LineageGraph::path_to_root() is the root-to-node path and ends at the node
  // itself. PromotionRequest::ancestry is the ANCESTRY of the candidate - the
  // candidate's own identity is already request.candidate - so the node is
  // dropped here: a candidate is never its own ancestor, a root candidate has
  // an empty ancestry, and ancestry.size() equals lineage_depth by construction.
  std::vector<CandidateId> ancestry = impl_->lineage_.path_to_root(winner->id);
  if (!ancestry.empty() && ancestry.back() == winner->id) {
    ancestry.pop_back();
  }
  request.ancestry = std::move(ancestry);
  request.canonical_state_digest =
      compute_candidate_state_digest(*winner, impl_->evaluations_for_locked(winner->id));

  bool eligible = true;
  for (const EvaluationRequirement& requirement : task->requirements) {
    const EvaluationRecord* record =
        impl_->requirement_record_locked(*winner, requirement.evaluator_key);
    PromotionEvidenceSummary summary;
    summary.evaluator_key = requirement.evaluator_key;
    summary.requirement_class = requirement.requirement_class;
    if (record != nullptr) {
      summary.kind = record->kind;
      summary.outcome = record->outcome;
      summary.complete = record->complete;
      summary.has_score = record->has_score;
      summary.score = record->score;
      summary.generation = record->generation;
      summary.evidence_digest = record->evidence_digest;
    } else {
      summary.kind = EvaluatorKind::PortableReference;
      summary.outcome = EvaluationOutcome::Unknown;
      summary.complete = false;
    }
    if (requirement.requirement_class == RequirementClass::Mandatory) {
      if (record == nullptr || !record->authoritative_for_mandatory()) {
        eligible = false;
        request.outstanding_requirements.push_back(
            "mandatory requirement '" + requirement.evaluator_key + "' is not satisfied");
      }
    }
    request.mandatory_evidence.push_back(std::move(summary));
  }
  if (winner->task_generation != task->generation) {
    eligible = false;
    request.outstanding_requirements.push_back("candidate task generation is not current");
  }
  if (!winner->selected) {
    eligible = false;
    request.outstanding_requirements.push_back("candidate is not the selected winner");
  }
  request.eligibility =
      eligible ? PromotionEligibilityState::Eligible : PromotionEligibilityState::NotEligible;

  impl_->promotion_requests_[request.id] = request;
  population->committed_promotion_request = request.id;
  population->promotion_requests += 1;
  impl_->statistics_.promotion_requests += 1;
  impl_->touch();
  return request;
}

Status FoundryCore::record_promotion_receipt(PromotionRequestId id, PromotionReceipt receipt) {
  std::unique_lock lock(impl_->mutex);
  const auto found = impl_->promotion_requests_.find(id);
  if (found == impl_->promotion_requests_.end()) {
    return Status(ErrorCode::UnknownIdentity,
                  "promotion request " + id.to_string() + " does not exist");
  }
  // The receipt is the receiving system's statement about its own processing.
  // It is validated and returned to the caller; it is never converted into a
  // claim that the artifact was promoted, and it is not durable authority.
  if (receipt.handoff == PromotionHandoffState::NotRequested) {
    return Status(ErrorCode::InvalidArgument,
                  "a promotion receipt must describe an attempted handoff");
  }
  if (!receipt.request_digest.empty() &&
      receipt.request_digest != found->second.canonical_state_digest) {
    return Status(ErrorCode::StaleAuthority,
                  "promotion receipt digest does not match the request it answers");
  }
  return Status();
}

// ---------------------------------------------------------------------------
// Closure
// ---------------------------------------------------------------------------

Status FoundryCore::close_population(PopulationId id) {
  std::unique_lock lock(impl_->mutex);

  // One closure rule, one implementation, one counter increment: the public
  // entry point and advance_population() share close_population_locked() rather
  // than maintaining two copies that can drift apart.
  return impl_->close_population_locked(id);
}

Result<PopulationId> FoundryCore::advance_population(PopulationId id, std::string next_name) {
  std::unique_lock lock(impl_->mutex);

  PopulationRecord* population = impl_->population_locked(id);
  if (population == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population " + id.to_string() + " does not exist");
  }
  if (!population->committed_selection.valid() || !population->retention_committed) {
    return Status(ErrorCode::IllegalStateTransition,
                  "advancing requires a committed selection and a committed retention decision");
  }
  const auto selection_entry = impl_->selections_.find(id);
  if (selection_entry == impl_->selections_.end() ||
      !selection_entry->second.selected.valid()) {
    return Status(ErrorCode::SelectionImpossible,
                  "advancing requires a selected winner to seed the next population");
  }
  const FoundryPolicy* policy = impl_->policy_locked(population->policy);
  const TaskSpec* task = impl_->task_locked(population->task);
  if (policy == nullptr || task == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "population policy or task is missing");
  }
  if (population->population_index >= policy->max_generation_depth) {
    return Status(ErrorCode::PolicyConflict,
                  "population index " + std::to_string(population->population_index) +
                      " reached the policy maximum generation depth " +
                      std::to_string(policy->max_generation_depth));
  }
  const CandidateRecord* winner = impl_->candidate_locked(selection_entry->second.selected);
  if (winner == nullptr) {
    return Status(ErrorCode::UnknownIdentity, "selected candidate is missing from state");
  }

  PopulationSpec spec;
  spec.name = std::move(next_name);
  spec.task = population->task;
  spec.task_generation = population->task_generation;
  spec.policy = population->policy;
  spec.policy_generation = population->policy_generation;
  spec.candidate_budget = population->candidate_budget;
  spec.worker_budget = population->worker_budget;
  spec.population_index = population->population_index + 1;
  spec.predecessor = population->id;
  spec.seed_candidate = winner->id;
  spec.seed_candidate_generation = winner->generation;
  spec.seed_lineage = winner->lineage;
  spec.require_promotion_request = population->require_promotion_request;

  PopulationId successor_id;
  AF_TRY_ASSIGN(successor_id, impl_->create_population_locked(spec));
  if (policy->carry_forward_elite) {
    AF_TRY(impl_->add_elite_candidate_locked(impl_->population_locked(successor_id), *winner));
  }
  AF_TRY(impl_->close_population_locked(population->id));
  return successor_id;
}

}  // namespace autonomous_foundry
