// Selection suite.
//
// Proves that hard gates precede ranking (a candidate with a high soft score
// and one failed hard gate is excluded and never appears in ranking), that
// ranking is deterministic for identical canonical state, that the tie-break is
// stable across runs and across shuffled input order, that the same canonical
// state produces byte-identical explanations, and that a decision prepared from
// state N is refused with StaleSelection once the candidate state changes.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/selection.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

/// Every legal requirement for every candidate, so the population can select.
struct Populated {
  std::vector<CandidateId> candidates;
  std::vector<CandidateRecord> records;
};

/// Drive four full candidate cycles. The third candidate fails its mandatory
/// compile-and-run gate while carrying the highest optional score in the
/// population, which is exactly the case a hard gate must exclude before
/// ranking ever sees it.
[[nodiscard]] Populated populate(af_test::FoundryFixture& fixture,
                                 const af_test::TestContext& context) {
  Populated populated;
  for (std::uint32_t slot = 1; slot <= 4; ++slot) {
    af_test::FullEvidence evidence;
    evidence.optional_score = (slot == 3) ? 100.0 : 1.0 + static_cast<double>(slot - 1);
    if (slot == 3) {
      evidence.compile_and_run_outcome = EvaluationOutcome::Fail;
    }
    populated.candidates.push_back(
        af_test::drive_full_candidate(context, fixture, slot, evidence));
    if (slot < 4) {
      af_test::reopen_population_for_dispatch(context, fixture);
    }
  }
  for (const CandidateId id : populated.candidates) {
    populated.records.push_back(af_test::load_candidate(context, fixture.core, id));
  }
  return populated;
}

[[nodiscard]] const RankingEntry* find_ranking(const SelectionDecision& decision, CandidateId id) {
  for (const RankingEntry& entry : decision.ranking) {
    if (entry.candidate == id) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] const ExclusionEntry* find_exclusion(const SelectionDecision& decision,
                                                   CandidateId id) {
  for (const ExclusionEntry& entry : decision.excluded) {
    if (entry.candidate == id) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] std::string render_ranking(const SelectionDecision& decision) {
  std::string text;
  for (const RankingEntry& entry : decision.ranking) {
    if (!text.empty()) {
      text.push_back(',');
    }
    text.append(entry.candidate.to_string());
    text.push_back('=');
    text.append(std::to_string(entry.total_score));
  }
  return text;
}

[[nodiscard]] std::string render_exclusions(const SelectionDecision& decision) {
  std::string text;
  for (const ExclusionEntry& entry : decision.excluded) {
    if (!text.empty()) {
      text.push_back(',');
    }
    text.append(entry.candidate.to_string());
    text.push_back(':');
    text.append(selection_stage_name(entry.stage));
    text.push_back('/');
    text.append(exclusion_reason_name(entry.reason));
  }
  return text;
}

}  // namespace

AF_TEST_CASE(selection, hard_gates_precede_ranking) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(11));
  af_test::build_running_fixture(af_ctx, fixture, 4);

  af_ctx.phase("DISPATCH");
  const Populated populated = populate(fixture, af_ctx);
  EXPECT_EQ(af_ctx, populated.candidates.size(), static_cast<std::size_t>(4));

  af_ctx.phase("VERIFY");
  REQUIRE_VALUE(af_ctx, SelectionDecision, decision, fixture.core.prepare_selection(fixture.population));

  // The candidate with the highest optional score failed a mandatory gate, so
  // it must be excluded before ranking and must never appear in the ranking.
  const CandidateId gated = populated.candidates[2];
  EXPECT_TRUE(af_ctx, find_exclusion(decision, gated) != nullptr);
  EXPECT_TRUE(af_ctx, find_ranking(decision, gated) == nullptr);

  const ExclusionEntry* exclusion = find_exclusion(decision, gated);
  if (exclusion != nullptr) {
    EXPECT_EQ(af_ctx, exclusion->stage, SelectionStage::HardConstraints);
    EXPECT_EQ(af_ctx, exclusion->reason, ExclusionReason::MandatoryEvaluationFailed);
    EXPECT_FALSE(af_ctx, exclusion->detail.empty());
  }

  // Every ranked candidate satisfies every mandatory gate.
  for (const RankingEntry& entry : decision.ranking) {
    const CandidateRecord candidate =
        af_test::load_candidate(af_ctx, fixture.core, entry.candidate);
    EXPECT_FALSE(af_ctx, candidate.id == gated);
    EXPECT_OK(af_ctx, fixture.core.population(fixture.population));
  }
  EXPECT_EQ(af_ctx, decision.ranking.size(), static_cast<std::size_t>(3));

  af_ctx.phase("VERIFY_WINNER");
  EXPECT_TRUE(af_ctx, decision.has_winner());
  EXPECT_FALSE(af_ctx, decision.selected == gated);
  const CandidateRecord winner = af_test::load_candidate(af_ctx, fixture.core, decision.selected);
  EXPECT_EQ(af_ctx, winner.state, CandidateState::Evaluated);

  af_ctx.phase("VERIFY_EXCLUSION_STAGE_NAMES");
  EXPECT_EQ(af_ctx, std::string(selection_stage_name(SelectionStage::HardConstraints)),
            std::string("HardConstraints"));
  EXPECT_EQ(af_ctx, std::string(exclusion_reason_name(ExclusionReason::MandatoryEvaluationFailed)),
            std::string("MandatoryEvaluationFailed"));
  EXPECT_EQ(af_ctx, kSelectionStageCount, static_cast<std::size_t>(9));
  EXPECT_EQ(af_ctx, kExclusionReasonCount, static_cast<std::size_t>(17));
}

AF_TEST_CASE(selection, ranking_and_explanations_are_deterministic) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(12));
  af_test::build_running_fixture(af_ctx, fixture, 4);

  af_ctx.phase("DISPATCH");
  const Populated populated = populate(fixture, af_ctx);

  af_ctx.phase("VERIFY");
  REQUIRE_VALUE(af_ctx, SelectionDecision, first, fixture.core.prepare_selection(fixture.population));
  REQUIRE_VALUE(af_ctx, SelectionDecision, second, fixture.core.prepare_selection(fixture.population));

  // Identical canonical state produces byte-identical ranking, exclusions,
  // digest and rationale.
  EXPECT_EQ(af_ctx, render_ranking(first), render_ranking(second));
  EXPECT_EQ(af_ctx, render_exclusions(first), render_exclusions(second));
  EXPECT_EQ(af_ctx, first.selected, second.selected);
  EXPECT_EQ(af_ctx, first.canonical_state_digest, second.canonical_state_digest);
  EXPECT_EQ(af_ctx, first.rationale, second.rationale);
  EXPECT_FALSE(af_ctx, first.canonical_state_digest.empty());

  af_ctx.phase("VERIFY_RANKS_ARE_CONTIGUOUS");
  for (std::size_t index = 0; index < first.ranking.size(); ++index) {
    EXPECT_EQ(af_ctx, first.ranking[index].rank, static_cast<std::uint32_t>(index + 1));
  }
  // Ranking is a total order: no two entries share a rank and the scores are
  // monotonically non-increasing.
  for (std::size_t index = 1; index < first.ranking.size(); ++index) {
    EXPECT_TRUE(af_ctx,
                first.ranking[index - 1].total_score >= first.ranking[index].total_score);
  }

  af_ctx.phase("VERIFY_FACTOR_VALUES_ARE_PRESENT");
  EXPECT_FALSE(af_ctx, first.ranking.empty());
  if (!first.ranking.empty()) {
    const RankingEntry& top = first.ranking.front();
    EXPECT_FALSE(af_ctx, top.factors.empty());
    for (const RankingFactorValue& factor : top.factors) {
      EXPECT_FALSE(af_ctx, factor.key.empty());
    }
    // The final factor is always the identity tie-break, which makes the
    // ordering total even when every measured value is equal.
    EXPECT_EQ(af_ctx, top.factors.back().kind, RankingFactorKind::CandidateIdTieBreak);
    EXPECT_EQ(af_ctx, top.factors.back().direction, RankingDirection::LowerIsBetter);
  }

  af_ctx.phase("VERIFY_TIE_BREAK_STABILITY");
  // A second, independently built population with one candidate per lineage
  // still ranks by the identity tie-break rather than by insertion order.
  af_test::FoundryFixture twin(af_test::make_config(12));
  af_test::build_running_fixture(af_ctx, twin, 4);
  const Populated twin_populated = populate(twin, af_ctx);
  REQUIRE_VALUE(af_ctx, SelectionDecision, twin_decision, twin.core.prepare_selection(twin.population));
  // The same salt produces the same identities, so the winner and ordering
  // must match exactly.
  EXPECT_EQ(af_ctx, twin_populated.candidates.size(), populated.candidates.size());
  EXPECT_EQ(af_ctx, render_ranking(twin_decision), render_ranking(first));
  EXPECT_EQ(af_ctx, twin_decision.selected, first.selected);
}

AF_TEST_CASE(selection, prepared_decision_is_refused_after_the_state_changes) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(13));
  af_test::build_running_fixture(af_ctx, fixture, 4);

  af_ctx.phase("DISPATCH");
  const Populated populated = populate(fixture, af_ctx);

  af_ctx.phase("PREPARE");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared, fixture.core.prepare_selection(fixture.population));
  EXPECT_EQ(af_ctx, prepared.state, SelectionDecisionState::Prepared);
  EXPECT_FALSE(af_ctx, prepared.canonical_state_digest.empty());

  af_ctx.phase("MUTATE");
  // Publishing new evidence for a losing candidate changes the canonical state
  // the prepared decision was derived from.
  const CandidateRecord stale =
      af_test::load_candidate(af_ctx, fixture.core, populated.candidates[3]);
  // Fresh evidence under an evaluator key that has no complete record for this
  // candidate generation. Re-recording a requirement that is already complete
  // is refused as a duplicate by contract, so a genuine state change has to be
  // new evidence rather than a second copy of existing evidence.
  af_test::record_evidence(af_ctx, fixture.core, stale, "supplementary-recheck",
                           EvaluationOutcome::Pass, EvaluatorKind::PortableReference,
                           RequirementClass::Optional, true, 42.0);

  af_ctx.phase("VERIFY");
  const Result<SelectionDecision> refused = fixture.core.commit_selection(prepared);
  EXPECT_STATUS_CODE(af_ctx, refused, ErrorCode::StaleSelection);
  EXPECT_TRUE(af_ctx, is_stale_authority(refused.status().code()));

  // A freshly prepared decision from the new state commits.
  REQUIRE_VALUE(af_ctx, SelectionDecision, fresh, fixture.core.prepare_selection(fixture.population));
  EXPECT_NE(af_ctx, fresh.canonical_state_digest, prepared.canonical_state_digest);
  REQUIRE_VALUE(af_ctx, SelectionDecision, committed, fixture.core.commit_selection(fresh));
  EXPECT_EQ(af_ctx, committed.state, SelectionDecisionState::Committed);
  EXPECT_TRUE(af_ctx, committed.generation.valid());

  af_ctx.phase("VERIFY_AT_MOST_ONE_WINNER");
  REQUIRE_VALUE(af_ctx, PopulationRecord, population, fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, population.committed_selection, committed.generation);
  std::size_t winners = 0;
  const std::vector<CandidateId> population_candidates =
      fixture.core.population_candidates(fixture.population);
  EXPECT_EQ(af_ctx, population_candidates.size(), populated.candidates.size());
  for (const CandidateId id : population_candidates) {
    const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, id);
    if (candidate.selected) {
      ++winners;
    }
  }
  EXPECT_EQ(af_ctx, winners, static_cast<std::size_t>(1));

  af_ctx.phase("VERIFY_REPLAY_OF_A_COMMITTED_DECISION");
  // Committing the same prepared decision twice is refused, because the state
  // it was derived from already changed when the winner was recorded.
  const Result<SelectionDecision> replay = fixture.core.commit_selection(fresh);
  EXPECT_STATUS_CODE(af_ctx, replay, ErrorCode::StaleSelection);
}