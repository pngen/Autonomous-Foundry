// Evaluation suite.
//
// Proves that UNKNOWN, UNSUPPORTED, ERROR and CANCELLED never satisfy a
// mandatory requirement, that a WorkerSelfReport never satisfies one whatever
// its outcome, that requirement completeness needs a COMPLETE record bound to
// the CURRENT candidate generation, and that a record bound to an older
// candidate generation is ignored rather than counted.

#include <cstdint>
#include <string>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "test_support.hpp"

namespace {

using namespace autonomous_foundry;

[[nodiscard]] CandidateId candidate(std::uint32_t counter) {
  return CandidateId::from_raw((static_cast<std::uint64_t>(IdKind::Candidate) << 56) |
                               0x0000B7ull << 32 | static_cast<std::uint64_t>(counter));
}

[[nodiscard]] EvaluationId evaluation(std::uint32_t counter) {
  return EvaluationId::from_raw((static_cast<std::uint64_t>(IdKind::Evaluation) << 56) |
                                0x0000B7ull << 32 | static_cast<std::uint64_t>(counter));
}

[[nodiscard]] EvaluatorId evaluator(std::uint32_t counter) {
  return EvaluatorId::from_raw((static_cast<std::uint64_t>(IdKind::Evaluator) << 56) |
                               0x0000B7ull << 32 | static_cast<std::uint64_t>(counter));
}

const TaskId kTask = TaskId::from_raw(0x040000B700000001ull);
const PopulationId kPopulation = PopulationId::from_raw(0x030000B700000001ull);
const CoordinatorEpoch kEpoch = CoordinatorEpoch::from_value(5);

/// A complete record for one candidate generation with the requested outcome.
[[nodiscard]] EvaluationRecord make_record(CandidateId target,
                                           CandidateGeneration candidate_generation,
                                           std::string_view key, EvaluationOutcome outcome,
                                           EvaluatorKind kind, bool complete) {
  EvaluationRecord record;
  record.id = evaluation(1);
  record.generation = EvaluationGeneration::first();
  record.candidate = target;
  record.candidate_generation = candidate_generation;
  record.task = kTask;
  record.task_generation = TaskGeneration::first();
  record.population = kPopulation;
  record.population_generation = PopulationGeneration::first();
  record.evaluator = evaluator(1);
  record.evaluator_key = std::string(key);
  record.kind = kind;
  record.requirement_class = RequirementClass::Mandatory;
  record.outcome = outcome;
  record.complete = complete;
  record.decided_epoch = complete ? kEpoch : CoordinatorEpoch();
  return record;
}

}  // namespace

AF_TEST_CASE(evaluation, only_a_complete_pass_satisfies_a_mandatory_requirement) {
  af_ctx.phase("VERIFY_OUTCOMES");
  EXPECT_TRUE(af_ctx, outcome_satisfies_mandatory(EvaluationOutcome::Pass));
  EXPECT_FALSE(af_ctx, outcome_satisfies_mandatory(EvaluationOutcome::Fail));
  EXPECT_FALSE(af_ctx, outcome_satisfies_mandatory(EvaluationOutcome::Unknown));
  EXPECT_FALSE(af_ctx, outcome_satisfies_mandatory(EvaluationOutcome::Unsupported));
  EXPECT_FALSE(af_ctx, outcome_satisfies_mandatory(EvaluationOutcome::Error));
  EXPECT_FALSE(af_ctx, outcome_satisfies_mandatory(EvaluationOutcome::Cancelled));

  af_ctx.phase("VERIFY_AUTHORITATIVE_KINDS");
  EXPECT_TRUE(af_ctx, evaluator_kind_is_authoritative(EvaluatorKind::ProcessCommand));
  EXPECT_TRUE(af_ctx, evaluator_kind_is_authoritative(EvaluatorKind::SourcePolicy));
  EXPECT_TRUE(af_ctx, evaluator_kind_is_authoritative(EvaluatorKind::PortableReference));
  // The producing worker is not a disinterested party.
  EXPECT_FALSE(af_ctx, evaluator_kind_is_authoritative(EvaluatorKind::WorkerSelfReport));

  af_ctx.phase("VERIFY_RECORD_PREDICATE");
  const CandidateId target = candidate(1);
  const CandidateGeneration generation = CandidateGeneration::first();

  EvaluationRecord passing = make_record(target, generation, "compile-and-run",
                                         EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand, true);
  EXPECT_TRUE(af_ctx, passing.authoritative_for_mandatory());

  for (const EvaluationOutcome outcome :
       {EvaluationOutcome::Fail, EvaluationOutcome::Unknown, EvaluationOutcome::Unsupported,
        EvaluationOutcome::Error, EvaluationOutcome::Cancelled}) {
    const EvaluationRecord record = make_record(target, generation, "compile-and-run", outcome,
                                                EvaluatorKind::ProcessCommand, true);
    EXPECT_FALSE(af_ctx, record.authoritative_for_mandatory());
  }

  af_ctx.phase("VERIFY_SELF_REPORT_NEVER_AUTHORITATIVE");
  for (const EvaluationOutcome outcome :
       {EvaluationOutcome::Pass, EvaluationOutcome::Fail, EvaluationOutcome::Unknown,
        EvaluationOutcome::Unsupported, EvaluationOutcome::Error, EvaluationOutcome::Cancelled}) {
    const EvaluationRecord record = make_record(target, generation, "compile-and-run", outcome,
                                                EvaluatorKind::WorkerSelfReport, true);
    EXPECT_FALSE(af_ctx, record.authoritative_for_mandatory());
  }

  af_ctx.phase("VERIFY_INCOMPLETE_NEVER_AUTHORITATIVE");
  // An incomplete record is a statement that the evaluator has not finished,
  // which is never a pass even when it carries a Pass outcome.
  const EvaluationRecord incomplete = make_record(target, generation, "compile-and-run",
                                                  EvaluationOutcome::Pass,
                                                  EvaluatorKind::ProcessCommand, false);
  EXPECT_FALSE(af_ctx, incomplete.complete);
  EXPECT_FALSE(af_ctx, incomplete.authoritative_for_mandatory());
}

AF_TEST_CASE(evaluation, evidence_lookup_is_generation_and_completeness_bound) {
  af_ctx.phase("SETUP");
  const CandidateId target = candidate(2);
  const CandidateGeneration current = CandidateGeneration::from_value(4);

  CandidateEvidence evidence;
  evidence.candidate = target;
  evidence.candidate_generation = current;
  evidence.task_generation = TaskGeneration::first();
  evidence.generation = EvidenceGeneration::first();

  af_ctx.phase("VERIFY_MISSING_EVIDENCE");
  EXPECT_TRUE(af_ctx, evidence.find("compile-and-run") == nullptr);
  EXPECT_FALSE(af_ctx, evidence.has_complete_authoritative_pass("compile-and-run"));

  af_ctx.phase("VERIFY_COMPLETE_CURRENT_PASS");
  evidence.records.push_back(make_record(target, current, "compile-and-run",
                                         EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                                         true));
  EXPECT_TRUE(af_ctx, evidence.find("compile-and-run") != nullptr);
  EXPECT_TRUE(af_ctx, evidence.has_complete_authoritative_pass("compile-and-run"));

  af_ctx.phase("VERIFY_OLDER_GENERATION_IS_IGNORED");
  {
    CandidateEvidence stale;
    stale.candidate = target;
    stale.candidate_generation = current;
    stale.task_generation = TaskGeneration::first();
    stale.generation = EvidenceGeneration::first();
    // The record is complete, authoritative and passing, but it describes
    // generation 3 while the candidate is at generation 4.
    stale.records.push_back(make_record(target, CandidateGeneration::from_value(3),
                                        "compile-and-run", EvaluationOutcome::Pass,
                                        EvaluatorKind::ProcessCommand, true));
    EXPECT_TRUE(af_ctx, stale.find("compile-and-run") == nullptr);
    EXPECT_FALSE(af_ctx, stale.has_complete_authoritative_pass("compile-and-run"));
  }

  af_ctx.phase("VERIFY_INCOMPLETE_RECORD_IS_NOT_FOUND");
  {
    CandidateEvidence partial;
    partial.candidate = target;
    partial.candidate_generation = current;
    partial.task_generation = TaskGeneration::first();
    partial.generation = EvidenceGeneration::first();
    partial.records.push_back(make_record(target, current, "compile-and-run",
                                          EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                                          false));
    EXPECT_TRUE(af_ctx, partial.find("compile-and-run") == nullptr);
    EXPECT_FALSE(af_ctx, partial.has_complete_authoritative_pass("compile-and-run"));
  }

  af_ctx.phase("VERIFY_SELF_REPORT_IS_NOT_FOUND");
  {
    CandidateEvidence self_reported;
    self_reported.candidate = target;
    self_reported.candidate_generation = current;
    self_reported.task_generation = TaskGeneration::first();
    self_reported.generation = EvidenceGeneration::first();
    self_reported.records.push_back(make_record(target, current, "compile-and-run",
                                                EvaluationOutcome::Pass,
                                                EvaluatorKind::WorkerSelfReport, true));
    EXPECT_TRUE(af_ctx, self_reported.find("compile-and-run") == nullptr);
    EXPECT_FALSE(af_ctx, self_reported.has_complete_authoritative_pass("compile-and-run"));
  }

  af_ctx.phase("VERIFY_KEY_LOOKUP");
  // A different key is a different requirement: evidence never leaks across
  // declared requirements.
  EXPECT_TRUE(af_ctx, evidence.find("source-policy") == nullptr);
  EXPECT_FALSE(af_ctx, evidence.has_complete_authoritative_pass("source-policy"));
  EXPECT_TRUE(af_ctx, evidence.has_complete_authoritative_pass("compile-and-run"));

  af_ctx.phase("VERIFY_NON_PASS_OUTCOMES_ARE_NOT_PASSES");
  for (const EvaluationOutcome outcome :
       {EvaluationOutcome::Fail, EvaluationOutcome::Unknown, EvaluationOutcome::Unsupported,
        EvaluationOutcome::Error, EvaluationOutcome::Cancelled}) {
    CandidateEvidence negative;
    negative.candidate = target;
    negative.candidate_generation = current;
    negative.task_generation = TaskGeneration::first();
    negative.generation = EvidenceGeneration::first();
    negative.records.push_back(make_record(target, current, "compile-and-run", outcome,
                                           EvaluatorKind::ProcessCommand, true));
    // The record is found, but it is not a pass.
    EXPECT_TRUE(af_ctx, negative.find("compile-and-run") != nullptr);
    EXPECT_FALSE(af_ctx, negative.has_complete_authoritative_pass("compile-and-run"));
  }
}

AF_TEST_CASE(evaluation, record_validation_accepts_well_formed_and_refuses_malformed) {
  af_ctx.phase("VERIFY_VALID");
  const CandidateId target = candidate(3);
  const EvaluationRecord valid =
      make_record(target, CandidateGeneration::first(), "compile-and-run",
                  EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand, true);
  EXPECT_OK(af_ctx, validate_evaluation_record(valid));

  af_ctx.phase("VERIFY_MISSING_IDENTITY");
  EvaluationRecord no_id = valid;
  no_id.id = EvaluationId();
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(no_id), ErrorCode::NullIdentity);

  EvaluationRecord no_candidate = valid;
  no_candidate.candidate = CandidateId();
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(no_candidate), ErrorCode::NullIdentity);

  EvaluationRecord zero_generation = valid;
  zero_generation.generation = EvaluationGeneration();
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(zero_generation),
                     ErrorCode::InvalidArgument);

  EvaluationRecord zero_candidate_generation = valid;
  zero_candidate_generation.candidate_generation = CandidateGeneration();
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(zero_candidate_generation),
                     ErrorCode::InvalidArgument);

  af_ctx.phase("VERIFY_EMPTY_KEY");
  EvaluationRecord empty_key = valid;
  empty_key.evaluator_key.clear();
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(empty_key), ErrorCode::InvalidArgument);

  af_ctx.phase("VERIFY_KEY_CHARACTER_SET");
  EvaluationRecord bad_key = valid;
  bad_key.evaluator_key = "compile and run";
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(bad_key), ErrorCode::InvalidArgument);
  EvaluationRecord path_key = valid;
  path_key.evaluator_key = "../escape";
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(path_key), ErrorCode::InvalidArgument);
  EvaluationRecord good_key = valid;
  good_key.evaluator_key = "compile-and-run.v2_final";
  EXPECT_OK(af_ctx, validate_evaluation_record(good_key));

  af_ctx.phase("VERIFY_COMPLETE_WITHOUT_EPOCH");
  EvaluationRecord no_epoch = valid;
  no_epoch.decided_epoch = CoordinatorEpoch();
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(no_epoch),
                     ErrorCode::MalformedEncoding);

  af_ctx.phase("VERIFY_NON_FINITE_SCORE");
  EvaluationRecord nan_score = valid;
  nan_score.has_score = true;
  nan_score.score = std::numeric_limits<double>::quiet_NaN();
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(nan_score), ErrorCode::InvalidArgument);

  af_ctx.phase("VERIFY_EVIDENCE_DIGEST_SHAPE");
  EvaluationRecord short_digest = valid;
  short_digest.evidence_digest = "abc";
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(short_digest),
                     ErrorCode::MalformedEncoding);
  EvaluationRecord upper_digest = valid;
  upper_digest.evidence_digest =
      "9F2C4C0C1F5A3D4E5B6C7D8E9F00112233445566778899AABBCCDDEEFF001122";
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(upper_digest),
                     ErrorCode::MalformedEncoding);
  EvaluationRecord good_digest = valid;
  good_digest.evidence_digest =
      "9f2c4c0c1f5a3d4e5b6c7d8e9f00112233445566778899aabbccddeeff001122";
  EXPECT_OK(af_ctx, validate_evaluation_record(good_digest));

  af_ctx.phase("VERIFY_ORDINAL_RANGES");
  EvaluationRecord bad_kind = valid;
  bad_kind.kind = static_cast<EvaluatorKind>(200);
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(bad_kind), ErrorCode::InvalidArgument);
  EvaluationRecord bad_outcome = valid;
  bad_outcome.outcome = static_cast<EvaluationOutcome>(200);
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(bad_outcome), ErrorCode::InvalidArgument);

  af_ctx.phase("VERIFY_DIAGNOSTICS_BOUND");
  EvaluationRecord long_diagnostics = valid;
  long_diagnostics.diagnostics.assign(kMaxDiagnosticsLength + 1, 'x');
  EXPECT_STATUS_CODE(af_ctx, validate_evaluation_record(long_diagnostics),
                     ErrorCode::LengthOutOfRange);

  af_ctx.phase("VERIFY_NAMES");
  EXPECT_EQ(af_ctx, std::string(evaluator_kind_name(EvaluatorKind::WorkerSelfReport)),
            std::string("WorkerSelfReport"));
  EXPECT_EQ(af_ctx, std::string(evaluation_outcome_name(EvaluationOutcome::Unsupported)),
            std::string("Unsupported"));
  EXPECT_EQ(af_ctx, std::string(evaluation_outcome_name(EvaluationOutcome::Cancelled)),
            std::string("Cancelled"));
  EXPECT_EQ(af_ctx, kEvaluatorKindCount, static_cast<std::size_t>(4));
  EXPECT_EQ(af_ctx, kEvaluationOutcomeCount, static_cast<std::size_t>(6));

  af_ctx.phase("VERIFY_ORDINALS");
  EXPECT_EQ(af_ctx, static_cast<int>(EvaluatorKind::ProcessCommand), 0);
  EXPECT_EQ(af_ctx, static_cast<int>(EvaluatorKind::WorkerSelfReport), 3);
  EXPECT_EQ(af_ctx, static_cast<int>(EvaluationOutcome::Pass), 0);
  EXPECT_EQ(af_ctx, static_cast<int>(EvaluationOutcome::Unknown), 2);
  EXPECT_EQ(af_ctx, static_cast<int>(EvaluationOutcome::Unsupported), 3);
  EXPECT_EQ(af_ctx, static_cast<int>(EvaluationOutcome::Error), 4);
  EXPECT_EQ(af_ctx, static_cast<int>(EvaluationOutcome::Cancelled), 5);
}
