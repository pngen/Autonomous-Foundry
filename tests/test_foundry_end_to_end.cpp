// End to end suite.
//
// Drives real in-process populations directly through the state machine and
// proves the whole decision path end to end:
//
//   * a committed selection names a winner that satisfies every mandatory gate,
//     and ranking never rescues a candidate that failed one;
//   * a candidate whose only evidence for a mandatory gate is the producing
//     worker's own PASS report never satisfies that gate and is never eligible;
//   * retention is a different question from selection: several candidates are
//     retained and the winner is retained because it was selected;
//   * a population only closes once every decision is committed and every
//     attempt has reached a terminal state, and its successor is seeded from the
//     committed winner;
//   * authority captured for the first population cannot mutate the second one,
//     and neither can an evaluation record bound to the old generation.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "autonomous_foundry/attempt.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

/// One produced candidate together with the worker authority that produced it.
struct Produced {
  CandidateId candidate;
  WorkerSessionAuthority session;
};

[[nodiscard]] const ExclusionEntry* find_exclusion(const SelectionDecision& decision,
                                                   CandidateId id) {
  for (const ExclusionEntry& entry : decision.excluded) {
    if (entry.candidate == id) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] const RankingEntry* find_ranking(const SelectionDecision& decision, CandidateId id) {
  for (const RankingEntry& entry : decision.ranking) {
    if (entry.candidate == id) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] bool contains(const std::vector<CandidateId>& values, CandidateId probe) {
  for (const CandidateId value : values) {
    if (value == probe) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] const EvaluationRecord* find_record(const std::vector<EvaluationRecord>& records,
                                                 std::string_view key) {
  for (const EvaluationRecord& record : records) {
    if (record.evaluator_key == key) {
      return &record;
    }
  }
  return nullptr;
}

/// Rebuild the operation authority of the candidate's latest attempt from
/// durable state: the session that produced it plus the attempt record.
[[nodiscard]] Result<WorkerOperationAuthority> operation_authority(
    FoundryCore& core, const WorkerSessionAuthority& session, CandidateId candidate_id) {
  const Result<CandidateRecord> candidate = core.candidate(candidate_id);
  if (!candidate.ok()) {
    return candidate.status();
  }
  const Result<AttemptRecord> attempt = core.attempt(candidate.value().attempt);
  if (!attempt.ok()) {
    return attempt.status();
  }
  WorkerOperationAuthority authority;
  authority.session = session;
  authority.population = candidate.value().population;
  authority.population_generation = attempt.value().population_generation;
  authority.task = candidate.value().task;
  authority.task_generation = candidate.value().task_generation;
  authority.candidate = candidate.value().id;
  authority.candidate_generation = candidate.value().generation;
  authority.attempt = attempt.value().id;
  authority.attempt_generation = attempt.value().generation;
  authority.assignment = attempt.value().assignment;
  return authority;
}

/// Produce one candidate on a dedicated worker and move it into Evaluating so
/// that completed evidence settles it into Evaluated.
[[nodiscard]] Produced produce(const af_test::TestContext& context,
                               af_test::FoundryFixture& fixture, std::uint32_t slot,
                               int strategy_ordinal, std::string_view label) {
  Produced produced;
  produced.session = af_test::connect_worker(context, fixture.core, slot);
  produced.candidate = af_test::produce_candidate(
      context, fixture.core, produced.session,
      af_test::reference_solution_source(strategy_ordinal), label);
  EXPECT_OK(context, fixture.core.begin_evaluation(produced.candidate));
  return produced;
}

/// Drive the documented revalidation cycle, which is the public path that
/// returns a settled population to Running so the next attempt can be
/// authorized.
void revalidate_cycle(const af_test::TestContext& context, FoundryCore& core,
                      PopulationId population) {
  EXPECT_OK(context, core.request_population_revalidation(population, "resume production"));
  REQUIRE_VALUE(context, PopulationRecord, record, core.population(population));
  EXPECT_OK(context, core.revalidate_population(population, record.generation, "resume production"));
}

/// Every mandatory requirement of the task must be covered by a complete,
/// authoritative, passing record for this candidate.
[[nodiscard]] bool satisfies_mandatory_gates(FoundryCore& core, const TaskSpec& task,
                                             CandidateId candidate) {
  const std::vector<EvaluationRecord> records = core.candidate_evaluations(candidate);
  for (const EvaluationRequirement& requirement : task.requirements) {
    if (requirement.requirement_class != RequirementClass::Mandatory) {
      continue;
    }
    bool satisfied = false;
    for (const EvaluationRecord& record : records) {
      if (record.evaluator_key == requirement.evaluator_key && record.complete &&
          record.authoritative_for_mandatory()) {
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

void record_all(const af_test::TestContext& context, FoundryCore& core, CandidateId candidate,
                EvaluationOutcome compile_and_run, double score) {
  const CandidateRecord record = af_test::load_candidate(context, core, candidate);
  af_test::record_evidence(context, core, record, kEvaluatorCompileAndRun, compile_and_run,
                           EvaluatorKind::ProcessCommand, RequirementClass::Mandatory, false, 0.0);
  af_test::record_evidence(context, core, record, kEvaluatorSourcePolicy, EvaluationOutcome::Pass,
                           EvaluatorKind::SourcePolicy, RequirementClass::Mandatory, false, 0.0);
  af_test::record_evidence(context, core, record, kEvaluatorPerformance, EvaluationOutcome::Pass,
                           EvaluatorKind::ProcessCommand, RequirementClass::Optional, true, score);
  af_test::record_evidence(context, core, record, kEvaluatorParsimony, EvaluationOutcome::Pass,
                           EvaluatorKind::PortableReference, RequirementClass::Optional, true,
                           score);
}

/// The strong self-reporting candidate: the producing worker claims a pass for
/// the mandatory compile-and-run gate while every other requirement is covered
/// authoritatively, and the optional scores are the highest in the population.
void record_self_reported(const af_test::TestContext& context, FoundryCore& core,
                          CandidateId candidate, double score) {
  const CandidateRecord record = af_test::load_candidate(context, core, candidate);
  af_test::record_evidence(context, core, record, kEvaluatorCompileAndRun, EvaluationOutcome::Pass,
                           EvaluatorKind::WorkerSelfReport, RequirementClass::Mandatory, false,
                           0.0);
  af_test::record_evidence(context, core, record, kEvaluatorSourcePolicy, EvaluationOutcome::Pass,
                           EvaluatorKind::SourcePolicy, RequirementClass::Mandatory, false, 0.0);
  af_test::record_evidence(context, core, record, kEvaluatorPerformance, EvaluationOutcome::Pass,
                           EvaluatorKind::ProcessCommand, RequirementClass::Optional, true, score);
  af_test::record_evidence(context, core, record, kEvaluatorParsimony, EvaluationOutcome::Pass,
                           EvaluatorKind::PortableReference, RequirementClass::Optional, true,
                           score);
}

void require_terminal_attempts(const af_test::TestContext& context, FoundryCore& core,
                               const std::vector<CandidateId>& candidates) {
  for (const CandidateId id : candidates) {
    const CandidateRecord record = af_test::load_candidate(context, core, id);
    REQUIRE_VALUE(context, AttemptRecord, attempt, core.attempt(record.attempt));
    EXPECT_TRUE(context, attempt_state_is_terminal(attempt.state));
  }
}

}  // namespace

AF_TEST_CASE(foundry_end_to_end, selection_retention_and_advance_close_the_population) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(41));
  af_test::build_running_fixture(af_ctx, fixture, 8);

  af_ctx.phase("DISPATCH");
  const Produced first = produce(af_ctx, fixture, 1, 0, "closed-form");
  record_all(af_ctx, fixture.core, first.candidate, EvaluationOutcome::Pass, 12.0);
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  const Produced second = produce(af_ctx, fixture, 2, 1, "iterative");
  record_all(af_ctx, fixture.core, second.candidate, EvaluationOutcome::Pass, 6.0);
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  const Produced third = produce(af_ctx, fixture, 3, 0, "closed-form-alt");
  record_all(af_ctx, fixture.core, third.candidate, EvaluationOutcome::Pass, 9.0);
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  const Produced fourth = produce(af_ctx, fixture, 4, 1, "iterative-alt");
  record_all(af_ctx, fixture.core, fourth.candidate, EvaluationOutcome::Pass, 4.0);

  const std::vector<CandidateId> candidates = {first.candidate, second.candidate, third.candidate,
                                               fourth.candidate};
  EXPECT_EQ(af_ctx, fixture.core.population_candidates(fixture.population).size(),
            candidates.size());
  for (const CandidateId id : candidates) {
    EXPECT_EQ(af_ctx, af_test::load_candidate(af_ctx, fixture.core, id).state,
              CandidateState::Evaluated);
  }

  af_ctx.phase("VERIFY_BLOCKERS_BEFORE_DECISIONS");
  const std::vector<std::string> blockers_before = fixture.core.closure_blockers(fixture.population);
  EXPECT_FALSE(af_ctx, blockers_before.empty());
  af_ctx.note("blockers before decisions: " + std::to_string(blockers_before.size()));

  af_ctx.phase("PREPARE");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared,
                fixture.core.prepare_selection(fixture.population));
  EXPECT_TRUE(af_ctx, prepared.excluded.empty());
  EXPECT_EQ(af_ctx, prepared.ranking.size(), candidates.size());
  EXPECT_TRUE(af_ctx, prepared.has_winner());
  EXPECT_EQ(af_ctx, prepared.selected, prepared.ranking.front().candidate);
  REQUIRE_VALUE(af_ctx, TaskSpec, task, fixture.core.task(fixture.task));
  for (const RankingEntry& entry : prepared.ranking) {
    EXPECT_TRUE(af_ctx, satisfies_mandatory_gates(fixture.core, task, entry.candidate));
  }

  af_ctx.phase("COMMIT");
  REQUIRE_VALUE(af_ctx, SelectionDecision, committed, fixture.core.commit_selection(prepared));
  EXPECT_EQ(af_ctx, committed.state, SelectionDecisionState::Committed);
  EXPECT_TRUE(af_ctx, committed.generation.valid());
  EXPECT_TRUE(af_ctx, satisfies_mandatory_gates(fixture.core, task, committed.selected));
  REQUIRE_VALUE(af_ctx, PopulationRecord, after_commit, fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, after_commit.committed_selection, committed.generation);
  std::size_t winners = 0;
  for (const CandidateId id : candidates) {
    if (af_test::load_candidate(af_ctx, fixture.core, id).selected) {
      ++winners;
    }
  }
  EXPECT_EQ(af_ctx, winners, static_cast<std::size_t>(1));

  af_ctx.phase("RESTART");
  // Selection is committed; retention must now be able to release the
  // candidates that lost. Revalidation is the documented recovery boundary that
  // re-establishes live authority for state that changed under the decision.
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  af_ctx.phase("COMMIT_RETENTION");
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention_prepared,
                fixture.core.prepare_retention(fixture.population));
  EXPECT_EQ(af_ctx, retention_prepared.selection_generation, committed.generation);
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention,
                fixture.core.commit_retention(retention_prepared));
  EXPECT_TRUE(af_ctx, retention.committed);

  af_ctx.phase("VERIFY_RETENTION_IS_NOT_SELECTION");
  EXPECT_TRUE(af_ctx, retention.retained.size() > 1);
  EXPECT_TRUE(af_ctx, contains(retention.retained, committed.selected));
  EXPECT_EQ(af_ctx, retention.retained.size() + retention.retired.size(), candidates.size());
  std::size_t retained_because_selected = 0;
  for (const RetentionDecisionEntry& entry : retention.entries) {
    if (entry.outcome == RetentionOutcome::RetainedBecauseSelected) {
      ++retained_because_selected;
      EXPECT_EQ(af_ctx, entry.candidate, committed.selected);
    }
  }
  EXPECT_EQ(af_ctx, retained_because_selected, static_cast<std::size_t>(1));
  for (const CandidateId id : retention.retained) {
    EXPECT_TRUE(af_ctx, af_test::load_candidate(af_ctx, fixture.core, id).retained);
  }

  af_ctx.phase("RESTART_AFTER_RETENTION");
  revalidate_cycle(af_ctx, fixture.core, fixture.population);
  for (const CandidateId id : candidates) {
    const CandidateRecord record = af_test::load_candidate(af_ctx, fixture.core, id);
    EXPECT_FALSE(af_ctx, record.state == CandidateState::Registered);
    EXPECT_FALSE(af_ctx, record.state == CandidateState::Producing);
    EXPECT_FALSE(af_ctx, record.state == CandidateState::Published);
    EXPECT_FALSE(af_ctx, record.state == CandidateState::Evaluating);
  }
  require_terminal_attempts(af_ctx, fixture.core, candidates);

  af_ctx.phase("CLOSE");
  const std::vector<std::string> blockers_after = fixture.core.closure_blockers(fixture.population);
  if (!blockers_after.empty()) {
    af_ctx.note("blockers after retention: " + std::to_string(blockers_after.size()) + " " +
                blockers_after.front());
  }
  EXPECT_TRUE(af_ctx, blockers_after.empty());
  EXPECT_OK(af_ctx, fixture.core.close_population(fixture.population));
  REQUIRE_VALUE(af_ctx, PopulationRecord, closed, fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, closed.state, PopulationState::Closed);

  af_ctx.phase("ADVANCE");
  REQUIRE_VALUE(af_ctx, PopulationId, successor,
                fixture.core.advance_population(fixture.population, "P2"));
  EXPECT_NE(af_ctx, successor, fixture.population);
  REQUIRE_VALUE(af_ctx, PopulationRecord, second_population, fixture.core.population(successor));
  EXPECT_EQ(af_ctx, second_population.population_index, static_cast<std::uint32_t>(2));
  EXPECT_EQ(af_ctx, second_population.predecessor, fixture.population);
  EXPECT_EQ(af_ctx, second_population.seed_candidate, committed.selected);
  EXPECT_FALSE(af_ctx, second_population.retention_committed);
  EXPECT_FALSE(af_ctx, second_population.committed_selection.valid());
  EXPECT_EQ(af_ctx, second_population.candidates.size(), static_cast<std::size_t>(0));

  af_ctx.phase("VERIFY_STALE_AUTHORITY_AGAINST_THE_SUCCESSOR");
  REQUIRE_VALUE(af_ctx, WorkerOperationAuthority, stale,
                operation_authority(fixture.core, first.session, first.candidate));
  // Authority captured while the candidate was produced in the first population
  // names that population generation; it must not be replayable after the
  // population advanced.
  EXPECT_STATUS_CODE(af_ctx, fixture.core.acknowledge_attempt(stale),
                     ErrorCode::StalePopulationGeneration);
  WorkerOperationAuthority retargeted = stale;
  retargeted.population = successor;
  EXPECT_STATUS_CODE(af_ctx, fixture.core.acknowledge_attempt(retargeted),
                     ErrorCode::StalePopulationGeneration);

  ControllerOperationAuthority controller;
  controller.session.coordinator_epoch = fixture.core.epoch();
  controller.session.run = fixture.core.run();
  controller.session.controller = ControllerId::from_raw(
      (static_cast<std::uint64_t>(IdKind::Controller) << 56) | 0x0000CAull << 32 | 1ull);
  controller.session.session = stale.session.session;
  controller.session.session_generation = stale.session.session_generation;
  controller.population = stale.population;
  controller.population_generation = stale.population_generation;
  controller.task = stale.task;
  controller.task_generation = stale.task_generation;
  controller.policy = fixture.policy;
  controller.policy_generation = fixture.policy_generation;
  EXPECT_STATUS_CODE(af_ctx, check_identity(successor, controller.population, "population"),
                     ErrorCode::StaleAuthority);
  EXPECT_STATUS_CODE(af_ctx, check_population_generation(controller.population_generation,
                                                         second_population.generation),
                     ErrorCode::StalePopulationGeneration);

  af_ctx.phase("VERIFY_STALE_EVIDENCE_IS_REFUSED");
  const CandidateRecord winner_record =
      af_test::load_candidate(af_ctx, fixture.core, committed.selected);
  EvaluationRecord late;
  late.candidate = winner_record.id;
  late.candidate_generation = winner_record.generation;
  late.task = winner_record.task;
  late.task_generation = winner_record.task_generation;
  late.population = stale.population;
  late.population_generation = stale.population_generation;
  late.evaluator = EvaluatorId::from_raw((static_cast<std::uint64_t>(IdKind::Evaluator) << 56) |
                                         0x0000CAull << 32 | 1ull);
  late.evaluator_key = std::string(kEvaluatorPerformance);
  late.kind = EvaluatorKind::ProcessCommand;
  late.requirement_class = RequirementClass::Optional;
  late.outcome = EvaluationOutcome::Pass;
  late.complete = true;
  late.has_score = true;
  late.score = 1.0;
  late.evidence_digest = "9f2c4c0c1f5a3d4e5b6c7d8e9f00112233445566778899aabbccddeeff001122";
  EXPECT_STATUS_CODE(af_ctx, fixture.core.record_evaluation(late),
                     ErrorCode::StalePopulationGeneration);

  EvaluationRecord misplaced = late;
  misplaced.population = successor;
  misplaced.population_generation = second_population.generation;
  EXPECT_STATUS_CODE(af_ctx, fixture.core.record_evaluation(misplaced), ErrorCode::StaleAuthority);

  af_ctx.phase("VERIFY_SUCCESSOR_IS_USABLE");
  EXPECT_OK(af_ctx, fixture.core.start_population(successor, second_population.generation));
  const Produced seeded = produce(af_ctx, fixture, 5, 0, "closed-form-child");
  const CandidateRecord child = af_test::load_candidate(af_ctx, fixture.core, seeded.candidate);
  EXPECT_EQ(af_ctx, child.population, successor);
  EXPECT_EQ(af_ctx, child.depth, static_cast<std::uint32_t>(1));
  EXPECT_EQ(af_ctx, child.parents.size(), static_cast<std::size_t>(1));
  if (!child.parents.empty()) {
    EXPECT_EQ(af_ctx, child.parents.front(), committed.selected);
  }

  af_ctx.phase("VERIFY_AUDIT");
  const std::vector<std::string> violations = fixture.core.audit();
  if (!violations.empty()) {
    af_ctx.note(violations.front());
  }
  EXPECT_TRUE(af_ctx, violations.empty());
}

AF_TEST_CASE(foundry_end_to_end, a_failed_gate_and_a_self_report_are_excluded_before_ranking) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(42));
  af_test::build_running_fixture(af_ctx, fixture, 8);

  af_ctx.phase("DISPATCH");
  const Produced closed_form = produce(af_ctx, fixture, 1, 0, "closed-form");
  record_all(af_ctx, fixture.core, closed_form.candidate, EvaluationOutcome::Pass, 12.0);
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  const Produced iterative = produce(af_ctx, fixture, 2, 1, "iterative");
  record_all(af_ctx, fixture.core, iterative.candidate, EvaluationOutcome::Pass, 6.0);
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  // Fails the mandatory compile-and-run gate while carrying by far the highest
  // optional score in the population.
  const Produced off_by_one = produce(af_ctx, fixture, 3, 2, "off-by-one");
  record_all(af_ctx, fixture.core, off_by_one.candidate, EvaluationOutcome::Fail, 1000.0);
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  const Produced second_closed_form = produce(af_ctx, fixture, 4, 0, "closed-form-alt");
  record_all(af_ctx, fixture.core, second_closed_form.candidate, EvaluationOutcome::Pass, 9.0);
  revalidate_cycle(af_ctx, fixture.core, fixture.population);

  // Produced last: its mandatory gate stays unresolved, so the population stays
  // in Evaluating while the candidate is present.
  const Produced self_reported = produce(af_ctx, fixture, 5, 3, "self-reported-pass");
  record_self_reported(af_ctx, fixture.core, self_reported.candidate, 900.0);

  const std::vector<CandidateId> candidates = {closed_form.candidate, iterative.candidate,
                                               off_by_one.candidate, second_closed_form.candidate,
                                               self_reported.candidate};
  EXPECT_EQ(af_ctx, fixture.core.population_candidates(fixture.population).size(),
            candidates.size());
  EXPECT_EQ(af_ctx, af_test::load_candidate(af_ctx, fixture.core, off_by_one.candidate).state,
            CandidateState::Evaluated);
  EXPECT_EQ(af_ctx, af_test::load_candidate(af_ctx, fixture.core, self_reported.candidate).state,
            CandidateState::Evaluating);

  af_ctx.phase("VERIFY_BLOCKERS_BEFORE_DECISIONS");
  const std::vector<std::string> blockers_before = fixture.core.closure_blockers(fixture.population);
  EXPECT_FALSE(af_ctx, blockers_before.empty());
  af_ctx.note("blockers before decisions: " + std::to_string(blockers_before.size()));

  af_ctx.phase("PREPARE");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared,
                fixture.core.prepare_selection(fixture.population));

  const ExclusionEntry* failed_gate = find_exclusion(prepared, off_by_one.candidate);
  EXPECT_TRUE(af_ctx, failed_gate != nullptr);
  if (failed_gate != nullptr) {
    EXPECT_EQ(af_ctx, failed_gate->reason, ExclusionReason::MandatoryEvaluationFailed);
    // The gate was evaluated and reported a failure, so the candidate violated a
    // hard constraint rather than leaving its evidence set unfinished.
    EXPECT_EQ(af_ctx, failed_gate->stage, SelectionStage::HardConstraints);
    EXPECT_FALSE(af_ctx, failed_gate->detail.empty());
  }
  EXPECT_TRUE(af_ctx, find_ranking(prepared, off_by_one.candidate) == nullptr);

  const ExclusionEntry* self_exclusion = find_exclusion(prepared, self_reported.candidate);
  EXPECT_TRUE(af_ctx, self_exclusion != nullptr);
  if (self_exclusion != nullptr) {
    af_ctx.note(std::string("self report exclusion ") +
                std::string(exclusion_reason_name(self_exclusion->reason)) + " at " +
                std::string(selection_stage_name(self_exclusion->stage)) + " :: " +
                self_exclusion->detail);
    // A worker's own pass can never satisfy a mandatory gate, so the candidate
    // is excluded for missing mandatory evidence. While the gate is unresolved
    // the candidate is still inside its evaluation lifecycle, which is the same
    // exclusion seen through the lifecycle stage.
    EXPECT_TRUE(af_ctx, self_exclusion->reason == ExclusionReason::MandatoryEvidenceMissing ||
                            self_exclusion->reason == ExclusionReason::LifecycleNotEligible);
    EXPECT_TRUE(af_ctx, self_exclusion->reason != ExclusionReason::MandatoryEvaluationFailed);
  }
  EXPECT_TRUE(af_ctx, find_ranking(prepared, self_reported.candidate) == nullptr);

  EXPECT_EQ(af_ctx, prepared.ranking.size(), static_cast<std::size_t>(3));
  EXPECT_TRUE(af_ctx, prepared.has_winner());
  EXPECT_EQ(af_ctx, prepared.selected, prepared.ranking.front().candidate);
  EXPECT_FALSE(af_ctx, prepared.selected == off_by_one.candidate);
  EXPECT_FALSE(af_ctx, prepared.selected == self_reported.candidate);

  af_ctx.phase("VERIFY_WINNER_GATES");
  REQUIRE_VALUE(af_ctx, TaskSpec, task, fixture.core.task(fixture.task));
  for (const RankingEntry& entry : prepared.ranking) {
    EXPECT_TRUE(af_ctx, satisfies_mandatory_gates(fixture.core, task, entry.candidate));
  }
  EXPECT_TRUE(af_ctx, satisfies_mandatory_gates(fixture.core, task, prepared.selected));
  EXPECT_EQ(af_ctx, af_test::load_candidate(af_ctx, fixture.core, prepared.selected).state,
            CandidateState::Evaluated);
  const std::vector<EvaluationRecord> excluded_evidence =
      fixture.core.candidate_evaluations(off_by_one.candidate);
  std::size_t performance_records = 0;
  for (const EvaluationRecord& record : excluded_evidence) {
    if (record.evaluator_key == kEvaluatorPerformance) {
      ++performance_records;
      af_ctx.note("performance record has_score=" + std::string(record.has_score ? "yes" : "no") +
                  " score=" + std::to_string(record.score) + " outcome=" +
                  std::string(evaluation_outcome_name(record.outcome)));
    }
  }
  af_ctx.note("excluded evidence records: " + std::to_string(excluded_evidence.size()) +
              " performance records: " + std::to_string(performance_records));
  const EvaluationRecord* best_optional = find_record(excluded_evidence, kEvaluatorPerformance);
  EXPECT_TRUE(af_ctx, best_optional != nullptr);
  if (best_optional != nullptr) {
    EXPECT_TRUE(af_ctx, best_optional->score > 100.0);
  }

  af_ctx.phase("COMMIT");
  REQUIRE_VALUE(af_ctx, SelectionDecision, committed, fixture.core.commit_selection(prepared));
  EXPECT_EQ(af_ctx, committed.state, SelectionDecisionState::Committed);
  EXPECT_TRUE(af_ctx, committed.generation.valid());
  EXPECT_EQ(af_ctx, committed.selected, prepared.selected);

  af_ctx.phase("VERIFY_AUDIT");
  const std::vector<std::string> violations = fixture.core.audit();
  if (!violations.empty()) {
    af_ctx.note(violations.front());
  }
  EXPECT_TRUE(af_ctx, violations.empty());
}

AF_TEST_CASE(foundry_end_to_end, a_worker_self_report_never_satisfies_a_mandatory_gate) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(43));
  af_test::build_running_fixture(af_ctx, fixture, 2);

  af_ctx.phase("DISPATCH");
  const Produced self_reported = produce(af_ctx, fixture, 1, 3, "self-reported-pass");
  record_self_reported(af_ctx, fixture.core, self_reported.candidate, 900.0);

  af_ctx.phase("VERIFY_STATE");
  const CandidateRecord candidate =
      af_test::load_candidate(af_ctx, fixture.core, self_reported.candidate);
  EXPECT_EQ(af_ctx, candidate.state, CandidateState::Evaluating);
  EXPECT_FALSE(af_ctx, candidate.selected);
  EXPECT_TRUE(af_ctx, candidate.evidence_generation.valid());

  af_ctx.phase("VERIFY_RECORD_IS_EVIDENCE_NOT_AUTHORITY");
  const std::vector<EvaluationRecord> records =
      fixture.core.candidate_evaluations(self_reported.candidate);
  const EvaluationRecord* self_report = find_record(records, kEvaluatorCompileAndRun);
  EXPECT_TRUE(af_ctx, self_report != nullptr);
  if (self_report != nullptr) {
    EXPECT_EQ(af_ctx, self_report->kind, EvaluatorKind::WorkerSelfReport);
    EXPECT_EQ(af_ctx, self_report->outcome, EvaluationOutcome::Pass);
    EXPECT_TRUE(af_ctx, self_report->complete);
    EXPECT_FALSE(af_ctx, self_report->authoritative_for_mandatory());
    EXPECT_FALSE(af_ctx, evaluator_kind_is_authoritative(self_report->kind));
  }
  const EvaluationRecord* policy_record = find_record(records, kEvaluatorSourcePolicy);
  EXPECT_TRUE(af_ctx, policy_record != nullptr);
  if (policy_record != nullptr) {
    EXPECT_TRUE(af_ctx, policy_record->authoritative_for_mandatory());
  }

  af_ctx.phase("VERIFY_GATE_IS_STILL_OPEN");
  REQUIRE_VALUE(af_ctx, std::vector<EvaluationRequirement>, pending,
                fixture.core.pending_evaluations(self_reported.candidate));
  bool compile_and_run_still_pending = false;
  for (const EvaluationRequirement& requirement : pending) {
    if (requirement.evaluator_key == kEvaluatorCompileAndRun) {
      compile_and_run_still_pending = true;
      EXPECT_EQ(af_ctx, requirement.requirement_class, RequirementClass::Mandatory);
    }
  }
  EXPECT_TRUE(af_ctx, compile_and_run_still_pending);
  EXPECT_TRUE(
      af_ctx, contains(fixture.core.candidates_awaiting_evaluation(), self_reported.candidate));

  af_ctx.phase("VERIFY_AUTHORITATIVE_RECORD_IS_REFUSED_FOR_THE_SAME_GATE");
  // The self report already occupies the evidence slot for this candidate
  // generation, so no second complete record for the same key is accepted.
  const CandidateRecord candidate_record =
      af_test::load_candidate(af_ctx, fixture.core, self_reported.candidate);
  REQUIRE_VALUE(af_ctx, PopulationRecord, population, fixture.core.population(fixture.population));
  EvaluationRecord authoritative;
  authoritative.candidate = candidate_record.id;
  authoritative.candidate_generation = candidate_record.generation;
  authoritative.task = candidate_record.task;
  authoritative.task_generation = candidate_record.task_generation;
  authoritative.population = candidate_record.population;
  authoritative.population_generation = population.generation;
  authoritative.evaluator = EvaluatorId::from_raw((static_cast<std::uint64_t>(IdKind::Evaluator) << 56) |
                                                  0x0000CBull << 32 | 1ull);
  authoritative.evaluator_key = std::string(kEvaluatorCompileAndRun);
  authoritative.kind = EvaluatorKind::ProcessCommand;
  authoritative.requirement_class = RequirementClass::Mandatory;
  authoritative.outcome = EvaluationOutcome::Pass;
  authoritative.complete = true;
  authoritative.evidence_digest = "9f2c4c0c1f5a3d4e5b6c7d8e9f00112233445566778899aabbccddeeff001122";
  EXPECT_STATUS_CODE(af_ctx, fixture.core.record_evaluation(authoritative),
                     ErrorCode::DuplicateResult);

  af_ctx.phase("VERIFY_NEVER_ELIGIBLE");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared,
                fixture.core.prepare_selection(fixture.population));
  EXPECT_TRUE(af_ctx, prepared.ranking.empty());
  EXPECT_FALSE(af_ctx, prepared.has_winner());
  EXPECT_EQ(af_ctx, prepared.state, SelectionDecisionState::Impossible);
  const ExclusionEntry* excluded = find_exclusion(prepared, self_reported.candidate);
  EXPECT_TRUE(af_ctx, excluded != nullptr);
  if (excluded != nullptr) {
    af_ctx.note(std::string("self report exclusion ") +
                std::string(exclusion_reason_name(excluded->reason)) + " at " +
                std::string(selection_stage_name(excluded->stage)));
    EXPECT_TRUE(af_ctx, excluded->reason != ExclusionReason::MandatoryEvaluationFailed);
    EXPECT_TRUE(af_ctx, excluded->reason == ExclusionReason::MandatoryEvidenceMissing ||
                            excluded->reason == ExclusionReason::LifecycleNotEligible);
  }

  af_ctx.phase("VERIFY_POPULATION_CANNOT_CLOSE_ON_UNRESOLVED_WORK");
  const std::vector<std::string> blockers = fixture.core.closure_blockers(fixture.population);
  EXPECT_FALSE(af_ctx, blockers.empty());
  EXPECT_STATUS_CODE(af_ctx, fixture.core.close_population(fixture.population),
                     ErrorCode::IllegalStateTransition);
  const Result<PopulationRecord> still_open = fixture.core.population(fixture.population);
  REQUIRE_OK(af_ctx, still_open);
  EXPECT_FALSE(af_ctx, population_state_is_terminal(still_open.value().state));

  af_ctx.phase("SHUTDOWN");
  EXPECT_TRUE(af_ctx, fixture.core.audit().empty());
}
