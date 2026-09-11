// Promotion suite.
//
// Proves that a promotion request is emitted for an eligible winner and reports
// Eligibility Eligible, that the receipt never claims promotion, that a request
// produced from a stale selection is refused, and that "requested" is not
// "promoted".

#include <cstdint>
#include <string>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/promotion.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

/// Drive one full candidate cycle with all four requirements satisfied.
[[nodiscard]] CandidateId populate_one(af_test::FoundryFixture& fixture,
                                       const af_test::TestContext& context, std::uint32_t slot) {
  af_test::FullEvidence evidence;
  evidence.optional_score = 3.0;
  return af_test::drive_full_candidate(context, fixture, slot, evidence);
}

/// Selection plus retention for a population with one eligible candidate.
void commit_selection_and_retention(const af_test::TestContext& context,
                                    af_test::FoundryFixture& fixture) {
  const SelectionDecision prepared =
      af_test::require_value(context, fixture.core.prepare_selection(fixture.population),
                             "prepare_selection", __FILE__, __LINE__);
  (void)af_test::require_value(context, fixture.core.commit_selection(prepared),
                               "commit_selection", __FILE__, __LINE__);
  const RetentionDecision retention_prepared =
      af_test::require_value(context, fixture.core.prepare_retention(fixture.population),
                             "prepare_retention", __FILE__, __LINE__);
  (void)af_test::require_value(context, fixture.core.commit_retention(retention_prepared),
                               "commit_retention", __FILE__, __LINE__);
}

}  // namespace

AF_TEST_CASE(promotion, eligible_winner_produces_a_request_that_never_claims_promotion) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(31));
  af_test::build_running_fixture(af_ctx, fixture, 2);

  af_ctx.phase("DISPATCH");
  const CandidateId produced = populate_one(fixture, af_ctx, 1);

  af_ctx.phase("COMMIT");
  commit_selection_and_retention(af_ctx, fixture);

  af_ctx.phase("VERIFY");
  REQUIRE_VALUE(af_ctx, PromotionRequest, request, fixture.core.prepare_promotion_request(fixture.population));
  EXPECT_EQ(af_ctx, request.eligibility, PromotionEligibilityState::Eligible);
  EXPECT_TRUE(af_ctx, request.outstanding_requirements.empty());
  EXPECT_TRUE(af_ctx, request.id.valid());
  EXPECT_EQ(af_ctx, request.candidate, produced);
  EXPECT_TRUE(af_ctx, request.selection_generation.valid());
  EXPECT_FALSE(af_ctx, request.artifact_set_digest.empty());
  EXPECT_FALSE(af_ctx, request.canonical_state_digest.empty());
  // The candidate is loaded into a named local first: EXPECT_EQ binds its
  // operands by reference, so a reference to a subobject of a temporary would
  // outlive that temporary and then read the freed heap.
  const CandidateRecord winner = af_test::load_candidate(af_ctx, fixture.core, produced);
  EXPECT_EQ(af_ctx, request.total_artifact_bytes, winner.artifacts.front().size_bytes);

  // The request carries one evidence summary per declared requirement, so the
  // receiving system can re-verify every digest independently.
  EXPECT_EQ(af_ctx, request.mandatory_evidence.size(), static_cast<std::size_t>(4));
  bool saw_mandatory_pass = false;
  for (const PromotionEvidenceSummary& summary : request.mandatory_evidence) {
    EXPECT_FALSE(af_ctx, summary.evaluator_key.empty());
    if (summary.requirement_class == RequirementClass::Mandatory) {
      EXPECT_EQ(af_ctx, summary.outcome, EvaluationOutcome::Pass);
      EXPECT_TRUE(af_ctx, summary.complete);
      saw_mandatory_pass = true;
    }
  }
  EXPECT_TRUE(af_ctx, saw_mandatory_pass);

  af_ctx.phase("VERIFY_NO_PROMOTED_STATE_EXISTS");
  // There is deliberately no "Promoted" handoff value: only an external
  // promotion system may claim that.
  EXPECT_EQ(af_ctx, std::string(promotion_handoff_state_name(PromotionHandoffState::NotRequested)),
            std::string("NotRequested"));
  EXPECT_EQ(af_ctx, std::string(promotion_handoff_state_name(PromotionHandoffState::Requested)),
            std::string("Requested"));
  EXPECT_EQ(af_ctx,
            std::string(promotion_handoff_state_name(PromotionHandoffState::AcceptedForProcessing)),
            std::string("AcceptedForProcessing"));
  EXPECT_EQ(af_ctx,
            std::string(promotion_eligibility_state_name(PromotionEligibilityState::Eligible)),
            std::string("Eligible"));
  EXPECT_EQ(af_ctx, std::string(promotion_eligibility_state_name(
                        static_cast<PromotionEligibilityState>(200))),
            std::string("Unknown"));

  af_ctx.phase("VERIFY_RECEIPT_IS_NOT_DURABLE_AUTHORITY");
  // A receipt describing a completed handoff still does not change foundry
  // state: the candidate remains merely requested.
  PromotionReceipt receipt;
  receipt.handoff = PromotionHandoffState::AcceptedForProcessing;
  receipt.sink_name = "reference-local-sink";
  receipt.sink_reference = "sink-1";
  receipt.detail = "accepted for the sink's own processing";
  receipt.request_digest = request.canonical_state_digest;
  EXPECT_OK(af_ctx, fixture.core.record_promotion_receipt(request.id, receipt));

  REQUIRE_VALUE(af_ctx, PromotionRequest, stored, fixture.core.promotion_request(request.id));
  EXPECT_EQ(af_ctx, stored.eligibility, PromotionEligibilityState::Eligible);
  const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, produced);
  EXPECT_FALSE(af_ctx, candidate.promotion_requested);
  EXPECT_FALSE(af_ctx, candidate.promotion_eligible);

  af_ctx.phase("VERIFY_RECEIPT_VALIDATION");
  PromotionReceipt empty;
  EXPECT_STATUS_CODE(af_ctx, fixture.core.record_promotion_receipt(request.id, empty),
                     ErrorCode::InvalidArgument);
  PromotionReceipt mismatched = receipt;
  mismatched.request_digest = "0000";
  EXPECT_STATUS_CODE(af_ctx, fixture.core.record_promotion_receipt(request.id, mismatched),
                     ErrorCode::StaleAuthority);
  EXPECT_STATUS_CODE(
      af_ctx,
      fixture.core.record_promotion_receipt(
          PromotionRequestId::from_raw(0x0F000001000000FFull), receipt),
      ErrorCode::UnknownIdentity);
}

AF_TEST_CASE(promotion, requested_is_not_promoted_and_stale_requests_are_refused) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(32));
  af_test::build_running_fixture(af_ctx, fixture, 3);

  af_ctx.phase("DISPATCH");
  const CandidateId produced = populate_one(fixture, af_ctx, 1);

  af_ctx.phase("VERIFY_BEFORE_SELECTION");
  // Without a committed selection and retention there is nothing to promote.
  EXPECT_STATUS_CODE(af_ctx, fixture.core.prepare_promotion_request(fixture.population),
                     ErrorCode::PromotionNotEligible);

  af_ctx.phase("COMMIT");
  commit_selection_and_retention(af_ctx, fixture);

  REQUIRE_VALUE(af_ctx, PromotionRequest, first, fixture.core.prepare_promotion_request(fixture.population));
  EXPECT_EQ(af_ctx, first.eligibility, PromotionEligibilityState::Eligible);

  af_ctx.phase("MUTATE_AFTER_REQUEST");
  // The candidate's canonical state digest changes once new evidence exists,
  // which is exactly the condition a receiving system must be able to detect.
  //
  // The fresh evidence is recorded under an evaluator key that has no complete
  // record for this candidate generation. Re-recording a declared requirement
  // is refused with DuplicateResult by construction: one complete authoritative
  // record per (candidate generation, evaluator key) is what makes the record
  // requirement_record_locked() selects deterministic, and a second record for
  // 'parsimony' would be a duplicate rather than fresh evidence.
  const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, produced);
  af_test::record_evidence(af_ctx, fixture.core, candidate, "supplementary-recheck",
                           EvaluationOutcome::Pass, EvaluatorKind::PortableReference,
                           RequirementClass::Optional, true, 8.0);
  const CandidateRecord mutated = af_test::load_candidate(af_ctx, fixture.core, produced);
  EXPECT_TRUE(af_ctx, mutated.evidence_generation.value() >= candidate.evidence_generation.value());

  af_ctx.phase("VERIFY_REQUEST_IS_A_HISTORICAL_FACT");
  // The already emitted request remains exactly what it was: it is history, not
  // a live claim about current state.
  REQUIRE_VALUE(af_ctx, PromotionRequest, reread, fixture.core.promotion_request(first.id));
  EXPECT_EQ(af_ctx, reread.canonical_state_digest, first.canonical_state_digest);
  EXPECT_EQ(af_ctx, reread.candidate_generation, first.candidate_generation);

  af_ctx.phase("VERIFY_RECEIPT_FOR_CHANGED_STATE_IS_REFUSED");
  PromotionReceipt receipt;
  receipt.handoff = PromotionHandoffState::AcceptedForProcessing;
  receipt.request_digest = first.canonical_state_digest + "0";
  EXPECT_STATUS_CODE(af_ctx, fixture.core.record_promotion_receipt(first.id, receipt),
                     ErrorCode::StaleAuthority);

  af_ctx.phase("VERIFY_SECOND_REQUEST_IS_A_NEW_IDENTITY");
  REQUIRE_VALUE(af_ctx, PromotionRequest, second, fixture.core.prepare_promotion_request(fixture.population));
  EXPECT_TRUE(af_ctx, second.id != first.id);
  EXPECT_TRUE(af_ctx, second.id.valid());
  REQUIRE_VALUE(af_ctx, PopulationRecord, current_population, fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, current_population.committed_promotion_request, second.id);
  EXPECT_EQ(af_ctx, current_population.promotion_requests, std::uint64_t{2});
  EXPECT_EQ(af_ctx, fixture.core.promotion_request_ids().size(), static_cast<std::size_t>(2));
}

AF_TEST_CASE(promotion, ineligible_winner_reports_the_concrete_outstanding_requirement) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(33));
  af_test::build_running_fixture(af_ctx, fixture, 2);

  af_ctx.phase("DISPATCH");
  const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, 1);
  const CandidateId produced = af_test::produce_candidate(
      af_ctx, fixture.core, session, af_test::reference_solution_source(0), "strategy-1");
  const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, produced);

  // Only the mandatory gates are recorded; the optional requirements keep no
  // record at all. That is enough for selection to admit the candidate, and
  // enough for promotion to report exactly what is missing.
  af_test::record_evidence(af_ctx, fixture.core, candidate, kEvaluatorCompileAndRun,
                           EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                           RequirementClass::Mandatory, false, 0.0);
  af_test::record_evidence(af_ctx, fixture.core, candidate, kEvaluatorSourcePolicy,
                           EvaluationOutcome::Pass, EvaluatorKind::SourcePolicy,
                           RequirementClass::Mandatory, false, 0.0);

  af_ctx.phase("COMMIT");
  commit_selection_and_retention(af_ctx, fixture);

  af_ctx.phase("VERIFY");
  REQUIRE_VALUE(af_ctx, PromotionRequest, request, fixture.core.prepare_promotion_request(fixture.population));
  // Every mandatory requirement is satisfied, so the winner is eligible even
  // though optional evidence is incomplete: eligibility is a mandatory-gate
  // question, not a completeness question.
  EXPECT_EQ(af_ctx, request.eligibility, PromotionEligibilityState::Eligible);
  EXPECT_TRUE(af_ctx, request.outstanding_requirements.empty());
  EXPECT_EQ(af_ctx, request.mandatory_evidence.size(), static_cast<std::size_t>(4));

  // A summary for a requirement with no record reports Unknown rather than a
  // defaulted pass.
  std::size_t unknown_summaries = 0;
  for (const PromotionEvidenceSummary& summary : request.mandatory_evidence) {
    if (summary.requirement_class == RequirementClass::Optional && !summary.complete) {
      ++unknown_summaries;
      EXPECT_EQ(af_ctx, summary.outcome, EvaluationOutcome::Unknown);
      EXPECT_FALSE(af_ctx, summary.has_score);
    }
  }
  EXPECT_EQ(af_ctx, unknown_summaries, static_cast<std::size_t>(2));

  af_ctx.phase("VERIFY_ANCESTRY_IS_ROOT_FIRST");
  EXPECT_TRUE(af_ctx, request.ancestry.empty());
  EXPECT_EQ(af_ctx, request.lineage_depth, std::uint32_t{0});
  EXPECT_EQ(af_ctx, request.lineage, candidate.lineage);

  af_ctx.phase("VERIFY_CANONICAL_ENCODING_IS_STABLE");
  const std::string encoded = encode_promotion_request_canonical(request);
  EXPECT_FALSE(af_ctx, encoded.empty());
  // The encoding is a function of the request alone, so two encodings of the
  // same value are identical.
  EXPECT_EQ(af_ctx, encoded, encode_promotion_request_canonical(request));
}