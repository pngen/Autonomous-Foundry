// Retention suite.
//
// Proves that retention is a different question from selection (a population
// can retain more than one candidate), that the retained set is deterministic,
// that the per-lineage cap and the total cap are enforced, and that retired
// candidates keep their history and cannot regain authority.

#include <cstdint>
#include <string>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/retention.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

/// Drive a whole population of full candidate cycles with distinct optional
/// scores, so retention has a real ranking to cut.
[[nodiscard]] std::vector<CandidateId> populate(af_test::FoundryFixture& fixture,
                                                const af_test::TestContext& context,
                                                std::size_t count) {
  std::vector<CandidateId> candidates;
  for (std::uint32_t slot = 1; slot <= count; ++slot) {
    af_test::FullEvidence evidence;
    evidence.optional_score = 1.0 + static_cast<double>(slot);
    candidates.push_back(af_test::drive_full_candidate(context, fixture, slot, evidence));
    if (slot < count) {
      af_test::reopen_population_for_dispatch(context, fixture);
    }
  }
  return candidates;
}

[[nodiscard]] std::string render(const std::vector<CandidateId>& values) {
  std::string text;
  for (const CandidateId value : values) {
    if (!text.empty()) {
      text.push_back(',');
    }
    text.append(value.to_string());
  }
  return text;
}

[[nodiscard]] bool contains(const std::vector<CandidateId>& values, CandidateId probe) {
  for (const CandidateId value : values) {
    if (value == probe) {
      return true;
    }
  }
  return false;
}

}  // namespace

AF_TEST_CASE(retention, retention_is_distinct_from_selection) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(21));
  af_test::build_running_fixture(af_ctx, fixture, 4);

  af_ctx.phase("DISPATCH");
  const std::vector<CandidateId> candidates = populate(fixture, af_ctx, 4);
  EXPECT_EQ(af_ctx, candidates.size(), static_cast<std::size_t>(4));

  af_ctx.phase("COMMIT_SELECTION");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared, fixture.core.prepare_selection(fixture.population));
  REQUIRE_VALUE(af_ctx, SelectionDecision, committed, fixture.core.commit_selection(prepared));
  EXPECT_TRUE(af_ctx, committed.has_winner());
  EXPECT_EQ(af_ctx, committed.ranking.size(), static_cast<std::size_t>(4));

  af_ctx.phase("COMMIT_RETENTION");
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention_prepared, fixture.core.prepare_retention(fixture.population));
  EXPECT_EQ(af_ctx, retention_prepared.selection_generation, committed.generation);
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention, fixture.core.commit_retention(retention_prepared));
  EXPECT_TRUE(af_ctx, retention.committed);

  af_ctx.phase("VERIFY_DISTINCT_QUESTION");
  // Selection named exactly one winner; retention kept more than one candidate.
  EXPECT_TRUE(af_ctx, retention.retained.size() > 1);
  EXPECT_TRUE(af_ctx, contains(retention.retained, committed.selected));
  // The retained set is exactly the complement of the retired set within the
  // population.
  EXPECT_EQ(af_ctx, retention.retained.size() + retention.retired.size(),
            candidates.size());
  for (const CandidateId id : retention.retained) {
    EXPECT_FALSE(af_ctx, contains(retention.retired, id));
  }

  af_ctx.phase("VERIFY_OUTCOME_VOCABULARY");
  std::size_t because_selected = 0;
  for (const RetentionDecisionEntry& entry : retention.entries) {
    if (entry.outcome == RetentionOutcome::RetainedBecauseSelected) {
      ++because_selected;
      EXPECT_EQ(af_ctx, entry.candidate, committed.selected);
    }
    if (entry.outcome == RetentionOutcome::RetiredRankedBelowCut ||
        entry.outcome == RetentionOutcome::RetiredLineageCap ||
        entry.outcome == RetentionOutcome::RetiredCapacityLimit ||
        entry.outcome == RetentionOutcome::RetiredIneligible) {
      EXPECT_TRUE(af_ctx, contains(retention.retired, entry.candidate));
    }
  }
  EXPECT_EQ(af_ctx, because_selected, static_cast<std::size_t>(1));

  af_ctx.phase("VERIFY_CANDIDATE_FLAGS");
  for (const CandidateId id : retention.retained) {
    const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, id);
    EXPECT_TRUE(af_ctx, candidate.retained);
  }
  for (const CandidateId id : retention.retired) {
    const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, id);
    EXPECT_FALSE(af_ctx, candidate.retained);
    EXPECT_TRUE(af_ctx, candidate.state == CandidateState::Retired ||
                            candidate.state == CandidateState::Selected);
  }

  af_ctx.phase("VERIFY_NAMES");
  EXPECT_EQ(af_ctx, std::string(retention_outcome_name(RetentionOutcome::RetainedBecauseSelected)),
            std::string("RetainedBecauseSelected"));
  EXPECT_EQ(af_ctx, std::string(retention_outcome_name(RetentionOutcome::RetiredLineageCap)),
            std::string("RetiredLineageCap"));
}

AF_TEST_CASE(retention, retained_set_is_deterministic) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(22));
  af_test::build_running_fixture(af_ctx, fixture, 4);

  af_ctx.phase("DISPATCH");
  const std::vector<CandidateId> produced = populate(fixture, af_ctx, 4);
  EXPECT_EQ(af_ctx, produced.size(), static_cast<std::size_t>(4));
  af_ctx.phase("COMMIT_SELECTION");
  af_ctx.phase("COMMIT_SELECTION");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared, fixture.core.prepare_selection(fixture.population));
  (void)af_test::require_value(af_ctx, fixture.core.commit_selection(prepared),
                               "commit_selection", __FILE__, __LINE__);

  REQUIRE_VALUE(af_ctx, RetentionDecision, first, fixture.core.prepare_retention(fixture.population));
  REQUIRE_VALUE(af_ctx, RetentionDecision, second, fixture.core.prepare_retention(fixture.population));
  EXPECT_EQ(af_ctx, render(first.retained), render(second.retained));
  EXPECT_EQ(af_ctx, render(first.retired), render(second.retired));
  EXPECT_EQ(af_ctx, first.canonical_state_digest, second.canonical_state_digest);
  EXPECT_FALSE(af_ctx, first.canonical_state_digest.empty());

  // The retained list is sorted by identity, which makes it a canonical value
  // rather than a function of iteration order.
  for (std::size_t index = 1; index < first.retained.size(); ++index) {
    EXPECT_TRUE(af_ctx, first.retained[index - 1] < first.retained[index]);
  }
  for (std::size_t index = 1; index < first.retired.size(); ++index) {
    EXPECT_TRUE(af_ctx, first.retired[index - 1] < first.retired[index]);
  }

  af_ctx.phase("VERIFY_COMMIT_IS_REFUSED_OUT_OF_ORDER");
  // Retention requires a committed selection; preparing and committing without
  // one is refused rather than silently producing an empty decision.
  af_test::FoundryFixture bare(af_test::make_config(23));
  af_test::build_running_fixture(af_ctx, bare, 2);
  const Result<RetentionDecision> no_selection = bare.core.prepare_retention(bare.population);
  EXPECT_STATUS_CODE(af_ctx, no_selection, ErrorCode::SelectionImpossible);
}

AF_TEST_CASE(retention, caps_are_enforced_and_retired_candidates_keep_their_history) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(24));
  af_test::build_running_fixture(af_ctx, fixture, 6);

  af_ctx.phase("DISPATCH");
  const std::vector<CandidateId> candidates = populate(fixture, af_ctx, 6);

  af_ctx.phase("COMMIT_SELECTION");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared, fixture.core.prepare_selection(fixture.population));
  (void)af_test::require_value(af_ctx, fixture.core.commit_selection(prepared), "commit_selection",
                               __FILE__, __LINE__);

  af_ctx.phase("COMMIT_RETENTION");
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention_prepared, fixture.core.prepare_retention(fixture.population));
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention, fixture.core.commit_retention(retention_prepared));

  af_ctx.phase("VERIFY_CAPS");
  REQUIRE_VALUE(af_ctx, PopulationRecord, population, fixture.core.population(fixture.population));
  REQUIRE_VALUE(af_ctx, FoundryPolicy, policy, fixture.core.policy(fixture.policy));
  EXPECT_TRUE(af_ctx, retention.retained.size() <= policy.retention.max_retained);
  EXPECT_TRUE(af_ctx, retention.retained.size() <= policy.retention.retain_top_k);
  EXPECT_TRUE(af_ctx,
              retention.retired.size() >= candidates.size() - policy.retention.retain_top_k);

  // Every entry carries a rank, and every entry carries a documented reason.
  for (const RetentionDecisionEntry& entry : retention.entries) {
    EXPECT_TRUE(af_ctx, entry.rank >= 1);
    EXPECT_TRUE(af_ctx, !entry.detail.empty());
  }

  af_ctx.phase("VERIFY_HISTORY_IS_PRESERVED");
  // A retired candidate keeps its artifacts, its lineage and its evidence.
  for (const CandidateId id : retention.retired) {
    const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, id);
    EXPECT_TRUE(af_ctx, candidate.state == CandidateState::Retired);
    EXPECT_FALSE(af_ctx, candidate.artifacts.empty());
    EXPECT_FALSE(af_ctx, candidate.artifact_set_digest.empty());
    EXPECT_TRUE(af_ctx, candidate.lineage.valid());
    EXPECT_FALSE(af_ctx, fixture.core.candidate_evaluations(id).empty());
  }

  af_ctx.phase("VERIFY_RETIRED_CANDIDATE_CANNOT_REGAIN_AUTHORITY");
  // Recording new evidence for a retired candidate is a late completion, and a
  // fresh selection can never pick it again.
  if (!retention.retired.empty()) {
    const CandidateRecord retired =
        af_test::load_candidate(af_ctx, fixture.core, retention.retired[0]);
    EvaluationRecord late;
    late.candidate = retired.id;
    late.candidate_generation = retired.generation;
    late.task = retired.task;
    late.task_generation = retired.task_generation;
    late.population = retired.population;
    late.population_generation = population.generation;
    late.evaluator_key = std::string(kEvaluatorPerformance);
    late.kind = EvaluatorKind::ProcessCommand;
    late.outcome = EvaluationOutcome::Pass;
    late.complete = true;
    late.has_score = true;
    late.score = 999.0;
    late.decided_epoch = fixture.core.epoch();
    EXPECT_STATUS_CODE(af_ctx, fixture.core.record_evaluation(late), ErrorCode::LateCompletion);

    REQUIRE_VALUE(af_ctx, SelectionDecision, again, fixture.core.prepare_selection(fixture.population));
    EXPECT_FALSE(af_ctx, again.selected == retired.id);
    REQUIRE_VALUE(af_ctx, SelectionDecision, recommitted, fixture.core.commit_selection(again));
    EXPECT_FALSE(af_ctx, recommitted.selected == retired.id);
    const CandidateRecord still_retired =
        af_test::load_candidate(af_ctx, fixture.core, retired.id);
    EXPECT_TRUE(af_ctx, still_retired.state == CandidateState::Retired);
    EXPECT_FALSE(af_ctx, still_retired.selected);
  }
}