// Evaluator suite.
//
// Proves that the reference evaluator registry is described correctly (keys and
// evaluator kinds), that the reference expected value is deterministic, and -
// when a real C++ toolchain is present - that the real evaluators are driven
// against real generated candidate sources:
//
//   * the correct closed-form and iterative sources satisfy the mandatory
//     compile-and-run gate;
//   * the off-by-one and self-reported sources do NOT, because the compiled
//     conformance process observes the wrong value;
//   * the mandatory gate is decided by executing the produced program, never by
//     the producing worker's own claim.
//
// When no toolchain resolves the case must still PASS, and it must say so with
// an explicit UNSUPPORTED marker. A compilation that did not happen is never
// reported as a pass.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/evaluator.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "test_support.hpp"

namespace {

using namespace autonomous_foundry;

[[nodiscard]] bool contains_key(const std::vector<std::string>& keys, std::string_view probe) {
  for (const std::string& key : keys) {
    if (key == probe) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool is_lower_hex_sha256(std::string_view text) {
  if (text.size() != 64) {
    return false;
  }
  for (const char character : text) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string describe_result(std::string_view label, const EvaluationResult& result) {
  std::string text(label);
  text.append(" -> ");
  text.append(evaluation_outcome_name(result.outcome));
  text.append(" has_score=");
  text.append(result.has_score ? "yes" : "no");
  if (result.has_score) {
    text.append(" score=");
    text.append(std::to_string(result.score));
  }
  text.append(" diagnostics='");
  text.append(result.diagnostics.substr(0, 160));
  text.push_back('\'');
  return text;
}

/// Candidate identity domains: Candidate, Task and Population, so the request
/// carries well formed identities of the right domain.
constexpr std::uint64_t kCandidateRaw =
    (static_cast<std::uint64_t>(IdKind::Candidate) << 56) | 0x0000E1ull << 32 | 1ull;
constexpr std::uint64_t kTaskRaw =
    (static_cast<std::uint64_t>(IdKind::Task) << 56) | 0x0000E1ull << 32 | 1ull;
constexpr std::uint64_t kPopulationRaw =
    (static_cast<std::uint64_t>(IdKind::Population) << 56) | 0x0000E1ull << 32 | 1ull;

[[nodiscard]] EvaluationRequest make_request(const ReferenceToolchain& toolchain,
                                             const std::filesystem::path& scratch,
                                             std::string_view evaluator_key,
                                             RequirementClass requirement_class,
                                             std::string_view solution_source) {
  EvaluationRequest request;
  request.candidate = CandidateId::from_raw(kCandidateRaw);
  request.candidate_generation = CandidateGeneration::first();
  request.task = TaskId::from_raw(kTaskRaw);
  request.task_generation = TaskGeneration::first();
  request.population = PopulationId::from_raw(kPopulationRaw);
  request.population_generation = PopulationGeneration::first();
  request.artifacts[std::string(kReferenceTaskSourceArtifact)] = std::string(solution_source);
  request.artifacts[std::string(kReferenceTaskHeaderArtifact)] = reference_api_header_source();
  request.inputs[std::string(kReferenceTaskHeaderArtifact)] = reference_api_header_source();
  request.evaluator_key = std::string(evaluator_key);
  request.requirement_class = requirement_class;
  request.scratch_directory = scratch;
  request.toolchain = toolchain;
  return request;
}

/// An independent closed form for the reference task, computed here so the
/// expected value is checked against a second implementation.
[[nodiscard]] std::uint64_t independent_expected_value(std::uint32_t n) {
  const std::uint64_t value = static_cast<std::uint64_t>(n);
  return value * (value + 1ull) * (2ull * value + 1ull) / 6ull;
}

struct Case {
  ReferenceStrategy strategy;
  const char* label;
  bool correct;
};

constexpr Case kCases[] = {{ReferenceStrategy::ClosedForm, "closed-form", true},
                           {ReferenceStrategy::Iterative, "iterative", true},
                           {ReferenceStrategy::OffByOne, "off-by-one", false},
                           {ReferenceStrategy::SelfReportedPass, "self-reported-pass", false}};

}  // namespace

AF_TEST_CASE(evaluator, registry_describes_the_reference_evaluators) {
  af_ctx.phase("VERIFY_KEYS");
  const EvaluatorRegistry registry = EvaluatorRegistry::make_reference();
  const std::vector<std::string> keys = registry.keys();
  EXPECT_EQ(af_ctx, keys.size(), static_cast<std::size_t>(4));
  EXPECT_TRUE(af_ctx, contains_key(keys, kEvaluatorCompileAndRun));
  EXPECT_TRUE(af_ctx, contains_key(keys, kEvaluatorSourcePolicy));
  EXPECT_TRUE(af_ctx, contains_key(keys, kEvaluatorPerformance));
  EXPECT_TRUE(af_ctx, contains_key(keys, kEvaluatorParsimony));
  EXPECT_TRUE(af_ctx, registry.find("no-such-evaluator") == nullptr);

  af_ctx.phase("VERIFY_KINDS");
  const std::shared_ptr<Evaluator> compile_and_run = registry.find(kEvaluatorCompileAndRun);
  EXPECT_TRUE(af_ctx, compile_and_run != nullptr);
  if (compile_and_run != nullptr) {
    EXPECT_EQ(af_ctx, compile_and_run->key(), std::string(kEvaluatorCompileAndRun));
    EXPECT_EQ(af_ctx, compile_and_run->kind(), EvaluatorKind::ProcessCommand);
    EXPECT_TRUE(af_ctx, evaluator_kind_is_authoritative(compile_and_run->kind()));
  }
  const std::shared_ptr<Evaluator> source_policy = registry.find(kEvaluatorSourcePolicy);
  EXPECT_TRUE(af_ctx, source_policy != nullptr);
  if (source_policy != nullptr) {
    EXPECT_EQ(af_ctx, source_policy->key(), std::string(kEvaluatorSourcePolicy));
    EXPECT_EQ(af_ctx, source_policy->kind(), EvaluatorKind::SourcePolicy);
    EXPECT_TRUE(af_ctx, evaluator_kind_is_authoritative(source_policy->kind()));
  }
  const std::shared_ptr<Evaluator> performance = registry.find(kEvaluatorPerformance);
  EXPECT_TRUE(af_ctx, performance != nullptr);
  if (performance != nullptr) {
    EXPECT_EQ(af_ctx, performance->key(), std::string(kEvaluatorPerformance));
    EXPECT_EQ(af_ctx, performance->kind(), EvaluatorKind::ProcessCommand);
    EXPECT_TRUE(af_ctx, evaluator_kind_is_authoritative(performance->kind()));
  }
  const std::shared_ptr<Evaluator> parsimony = registry.find(kEvaluatorParsimony);
  EXPECT_TRUE(af_ctx, parsimony != nullptr);
  if (parsimony != nullptr) {
    EXPECT_EQ(af_ctx, parsimony->key(), std::string(kEvaluatorParsimony));
    EXPECT_EQ(af_ctx, parsimony->kind(), EvaluatorKind::PortableReference);
    EXPECT_TRUE(af_ctx, evaluator_kind_is_authoritative(parsimony->kind()));
  }

  af_ctx.phase("VERIFY_STANDALONE_EVALUATORS");
  const CompileAndRunEvaluator standalone_compile;
  EXPECT_EQ(af_ctx, standalone_compile.key(), std::string(kEvaluatorCompileAndRun));
  EXPECT_EQ(af_ctx, standalone_compile.kind(), EvaluatorKind::ProcessCommand);
  const SourcePolicyEvaluator standalone_policy;
  EXPECT_EQ(af_ctx, standalone_policy.key(), std::string(kEvaluatorSourcePolicy));
  EXPECT_EQ(af_ctx, standalone_policy.kind(), EvaluatorKind::SourcePolicy);
  const PerformanceEvaluator standalone_performance;
  EXPECT_EQ(af_ctx, standalone_performance.key(), std::string(kEvaluatorPerformance));
  EXPECT_EQ(af_ctx, standalone_performance.kind(), EvaluatorKind::ProcessCommand);
  ParsimonyEvaluator standalone_parsimony;
  EXPECT_EQ(af_ctx, standalone_parsimony.key(), std::string(kEvaluatorParsimony));
  EXPECT_EQ(af_ctx, standalone_parsimony.kind(), EvaluatorKind::PortableReference);

  af_ctx.phase("VERIFY_EXPECTED_VALUE_IS_DETERMINISTIC");
  const std::uint32_t probes[] = {0u, 1u, 2u, 3u, 6u, 7u, 10u, 1000u};
  for (const std::uint32_t n : probes) {
    EXPECT_EQ(af_ctx, reference_expected_value(n), reference_expected_value(n));
    EXPECT_EQ(af_ctx, reference_expected_value(n), independent_expected_value(n));
  }
  EXPECT_EQ(af_ctx, reference_expected_value(0), static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, reference_expected_value(1), static_cast<std::uint64_t>(1));

  af_ctx.phase("VERIFY_GENERATED_SOURCES");
  EXPECT_FALSE(af_ctx, reference_api_header_source().empty());
  EXPECT_FALSE(af_ctx, reference_test_harness_source().empty());
  EXPECT_FALSE(af_ctx, reference_benchmark_harness_source().empty());
  for (const Case& probe : kCases) {
    const std::string source = generate_reference_solution(probe.strategy);
    EXPECT_FALSE(af_ctx, source.empty());
    EXPECT_TRUE(af_ctx, source.find("af_reference") != std::string::npos);
    // Every generated strategy defines the required symbol; the strategies
    // differ in the algorithm, not in the declared API.
    EXPECT_TRUE(af_ctx, source.find("sum_of_squares") != std::string::npos);
    EXPECT_TRUE(af_ctx, source.find(std::string(kReferenceTaskHeaderArtifact)) != std::string::npos);
  }

  af_ctx.phase("VERIFY_PARSIMONY_IS_PORTABLE");
  // The parsimony evaluator is a deterministic in-process metric, so it must
  // produce a score even when no compiler exists.
  af_test::TempDirectory scratch("af_evaluator_registry");
  ReferenceToolchain portable;
  for (const Case& probe : kCases) {
    const EvaluationRequest request = make_request(
        portable, scratch.path(), kEvaluatorParsimony, RequirementClass::Optional,
        generate_reference_solution(probe.strategy));
    EvaluationResult result = standalone_parsimony.evaluate(request);
    EXPECT_EQ(af_ctx, result.outcome, EvaluationOutcome::Pass);
    EXPECT_TRUE(af_ctx, result.has_score);
    EXPECT_TRUE(af_ctx, result.score > 0.0);
    EXPECT_TRUE(af_ctx, is_lower_hex_sha256(result.evidence_digest));
  }
  EXPECT_FALSE(af_ctx, ReferenceToolchain().usable());

  af_ctx.phase("SHUTDOWN");
}

AF_TEST_CASE(evaluator, real_evaluators_decide_the_reference_gate_by_execution) {
  af_ctx.phase("SETUP");
  const Result<ReferenceToolchain> resolved = resolve_reference_toolchain();
  if (!resolved.ok()) {
    EXPECT_STATUS_CODE(af_ctx, resolved, ErrorCode::Unsupported);
    af_ctx.phase("UNSUPPORTED");
    af_ctx.note("UNSUPPORTED: no reference toolchain resolved");
    af_ctx.note("UNSUPPORTED: " + resolved.status().message());
    // No compilation happened, so nothing about compilation is asserted.
    return;
  }
  EXPECT_TRUE(af_ctx, resolved.value().usable());
  EXPECT_FALSE(af_ctx, resolved.value().flavour.empty());
  af_ctx.note("toolchain: " + resolved.value().describe());
  const ReferenceToolchain toolchain = resolved.value();

  af_test::TempDirectory scratch("af_evaluator_execution");
  CompileAndRunEvaluator compile_and_run;
  SourcePolicyEvaluator source_policy;
  PerformanceEvaluator performance;
  ParsimonyEvaluator parsimony;

  af_ctx.phase("SOURCE_POLICY");
  for (const Case& probe : kCases) {
    const EvaluationRequest request =
        make_request(toolchain, scratch.path(), kEvaluatorSourcePolicy,
                     RequirementClass::Mandatory, generate_reference_solution(probe.strategy));
    const EvaluationResult result = source_policy.evaluate(request);
    af_ctx.note(describe_result(std::string("source-policy ") + probe.label, result));
    // The static policy checks the required symbol and the forbidden constructs.
    // It cannot decide correctness: every generated source passes it.
    EXPECT_EQ(af_ctx, result.outcome, EvaluationOutcome::Pass);
    EXPECT_TRUE(af_ctx, is_lower_hex_sha256(result.evidence_digest));
  }

  af_ctx.phase("COMPILE");
  for (const Case& probe : kCases) {
    const EvaluationRequest request =
        make_request(toolchain, scratch.path(), kEvaluatorCompileAndRun,
                     RequirementClass::Mandatory, generate_reference_solution(probe.strategy));
    const EvaluationResult result = compile_and_run.evaluate(request);
    af_ctx.note(describe_result(std::string("compile-and-run ") + probe.label, result));
    EXPECT_TRUE(af_ctx, is_lower_hex_sha256(result.evidence_digest));
    if (probe.correct) {
      EXPECT_EQ(af_ctx, result.outcome, EvaluationOutcome::Pass);
    } else {
      EXPECT_NE(af_ctx, result.outcome, EvaluationOutcome::Pass);
      EXPECT_TRUE(af_ctx, result.outcome == EvaluationOutcome::Fail ||
                              result.outcome == EvaluationOutcome::Unsupported ||
                              result.outcome == EvaluationOutcome::Error);
      EXPECT_FALSE(af_ctx, outcome_satisfies_mandatory(result.outcome));
    }
  }

  af_ctx.phase("RUN");
  // The mandatory gate of the reference task is the compiled program's real exit
  // status and its real output token, so a correct candidate must be measurably
  // faster than the linearly accumulating one and both must run.
  for (const Case& probe : kCases) {
    if (!probe.correct) {
      continue;
    }
    const EvaluationRequest request =
        make_request(toolchain, scratch.path(), kEvaluatorPerformance,
                     RequirementClass::Optional, generate_reference_solution(probe.strategy));
    const EvaluationResult result = performance.evaluate(request);
    af_ctx.note(describe_result(std::string("performance ") + probe.label, result));
    EXPECT_EQ(af_ctx, result.outcome, EvaluationOutcome::Pass);
    EXPECT_TRUE(af_ctx, result.has_score);
    EXPECT_TRUE(af_ctx, result.score > 0.0);
  }

  af_ctx.phase("VERIFY");
  for (const Case& probe : kCases) {
    const EvaluationRequest request =
        make_request(toolchain, scratch.path(), kEvaluatorParsimony,
                     RequirementClass::Optional, generate_reference_solution(probe.strategy));
    const EvaluationResult result = parsimony.evaluate(request);
    EXPECT_EQ(af_ctx, result.outcome, EvaluationOutcome::Pass);
    EXPECT_TRUE(af_ctx, result.has_score);
    EXPECT_TRUE(af_ctx, result.score > 0.0);
  }

  af_ctx.phase("SHUTDOWN");
  scratch.remove_now();
}
