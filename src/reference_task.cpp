#include "autonomous_foundry/reference_task.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/hash.hpp"

namespace autonomous_foundry {
namespace {

// The reference objective is deliberately small enough to compile and run
// inside a validation suite, and rich enough that a wrong implementation is
// caught by execution rather than by inspection.
//
// Contract:
//
//   std::uint64_t af_reference_sum_of_squares(std::uint32_t n)
//
// returns sum(i * i for i in 1..n) evaluated with wrapping 64-bit arithmetic.
//
// The declared API name, the name in kReferenceTaskApiName and the name the
// source-policy evaluator requires are the same string by construction: one
// contract, three views of it. A worker that reads the header and defines what
// it declares necessarily satisfies the static policy, and a candidate that
// satisfies the static policy has defined the symbol the header declares.

constexpr std::string_view kApiHeader = R"AFDELIM(#pragma once

#include <cstdint>

// Returns sum(i * i for i in 1..n), computed with wrapping 64-bit arithmetic:
// the accumulation is performed on std::uint64_t, so a result that would exceed
// 2^64 - 1 wraps modulo 2^64 exactly as the accumulation loop does.
//
// n == 0 must return 0.
//
// The definition must live in the global namespace with exactly this name,
// because the conformance harness links against this declaration.
std::uint64_t af_reference_sum_of_squares(std::uint32_t n);
)AFDELIM";

// The conformance harness is owned by the evaluator and by the task contract.
// A worker never sees it, never receives it as task input, and cannot influence
// it: the evaluator compiles its own copy in a scratch directory and copies
// only the candidate translation unit beside it.
constexpr std::string_view kTestHarness = R"AFDELIM(#include <cstdint>
#include <cstdio>

#include "af_reference.hpp"

namespace {

// Independent, deliberately naive reference: a plain accumulation loop. This
// is not the candidate implementation and it is not a stored constant, so a
// candidate cannot satisfy the harness by printing a token.
std::uint64_t reference_value(std::uint32_t n) {
  std::uint64_t total = 0;
  for (std::uint32_t i = 1; i <= n; ++i) {
    const std::uint64_t term = static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(i);
    total = total + term;
  }
  return total;
}

int check_case(std::uint32_t n) {
  const std::uint64_t expected = reference_value(n);
  const std::uint64_t observed = af_reference_sum_of_squares(n);
  if (observed != expected) {
    std::printf("AF_REFERENCE_MISMATCH n=%u expected=%llu observed=%llu\n", n,
                static_cast<unsigned long long>(expected),
                static_cast<unsigned long long>(observed));
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  const std::uint32_t cases[] = {0u, 1u, 2u, 3u, 6u, 7u, 10u, 1000u, 100000u, 1000000u};
  for (std::size_t index = 0; index < (sizeof(cases) / sizeof(cases[0])); ++index) {
    if (check_case(cases[index]) != 0) {
      return 1;
    }
  }
  std::printf("AF_REFERENCE_OK\n");
  return 0;
}
)AFDELIM";

constexpr std::string_view kBenchmarkHarness = R"AFDELIM(#include <chrono>
#include <cstdint>
#include <cstdio>

#include "af_reference.hpp"

int main() {
  const std::uint32_t rounds = 32;
  const std::uint32_t n = 1000000u;

  std::uint64_t sink = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t round = 0; round < rounds; ++round) {
    sink = sink + af_reference_sum_of_squares(n);
  }
  const auto finish = std::chrono::steady_clock::now();

  const double micros = std::chrono::duration<double, std::micro>(finish - start).count();
  if (micros <= 0.0) {
    std::printf("AF_BENCH_INVALID\n");
    return 1;
  }
  const double operations_per_second = static_cast<double>(rounds) * 1.0e6 / micros;
  std::printf("AF_BENCH_OPS_PER_SEC %.6f\n", operations_per_second);
  std::printf("AF_BENCH_SINK %llu\n", static_cast<unsigned long long>(sink));
  std::printf("AF_BENCH_OK\n");
  return 0;
}
)AFDELIM";

constexpr std::string_view kClosedFormSolution = R"AFDELIM(#include "af_reference.hpp"

std::uint64_t af_reference_sum_of_squares(std::uint32_t n) {
  // Closed form n * (n + 1) * (2n + 1) / 6. The division is performed on the
  // factors before multiplying so that the result is exact for every input and
  // the final multiply wraps exactly like the accumulation loop would.
  std::uint64_t a = static_cast<std::uint64_t>(n);
  std::uint64_t b = static_cast<std::uint64_t>(n) + 1ull;
  std::uint64_t c = 2ull * static_cast<std::uint64_t>(n) + 1ull;

  if ((a % 2ull) == 0ull) {
    a /= 2ull;
  } else {
    b /= 2ull;
  }
  if ((a % 3ull) == 0ull) {
    a /= 3ull;
  } else if ((b % 3ull) == 0ull) {
    b /= 3ull;
  } else {
    c /= 3ull;
  }
  return a * b * c;
}
)AFDELIM";

constexpr std::string_view kIterativeSolution = R"AFDELIM(#include "af_reference.hpp"

std::uint64_t af_reference_sum_of_squares(std::uint32_t n) {
  // Correct, but linear in n: the foundry should rank this below the closed
  // form on the measured performance factor while still accepting it.
  std::uint64_t total = 0;
  for (std::uint64_t value = 1; value <= static_cast<std::uint64_t>(n); ++value) {
    total += value * value;
  }
  return total;
}
)AFDELIM";

constexpr std::string_view kOffByOneSolution = R"AFDELIM(#include "af_reference.hpp"

std::uint64_t af_reference_sum_of_squares(std::uint32_t n) {
  // Subtly wrong: the final term is dropped, so every non-zero input is
  // rejected by the conformance harness.
  std::uint64_t total = 0;
  for (std::uint64_t value = 1; value < static_cast<std::uint64_t>(n); ++value) {
    total += value * value;
  }
  return total;
}
)AFDELIM";

constexpr std::string_view kSelfReportedSolution = R"AFDELIM(#include "af_reference.hpp"

std::uint64_t af_reference_sum_of_squares(std::uint32_t n) {
  // Subtly wrong in a different way from the off-by-one variant: the first
  // term is dropped. A worker using this strategy additionally reports
  // unconditional success, which is exactly the situation the mandatory
  // compile-and-run gate exists to catch.
  std::uint64_t total = 0;
  for (std::uint64_t value = 2; value <= static_cast<std::uint64_t>(n); ++value) {
    total += value * value;
  }
  return total;
}
)AFDELIM";

}  // namespace

std::uint64_t reference_expected_value(std::uint32_t n) noexcept {
  std::uint64_t total = 0;
  for (std::uint32_t value = 1; value <= n; ++value) {
    const std::uint64_t term = static_cast<std::uint64_t>(value) * static_cast<std::uint64_t>(value);
    total += term;
  }
  return total;
}

std::string reference_api_header_source() { return std::string(kApiHeader); }

std::string reference_test_harness_source() { return std::string(kTestHarness); }

std::string reference_benchmark_harness_source() { return std::string(kBenchmarkHarness); }

std::string generate_reference_solution(ReferenceStrategy strategy) {
  switch (strategy) {
    case ReferenceStrategy::ClosedForm:
      return std::string(kClosedFormSolution);
    case ReferenceStrategy::Iterative:
      return std::string(kIterativeSolution);
    case ReferenceStrategy::OffByOne:
      return std::string(kOffByOneSolution);
    case ReferenceStrategy::SelfReportedPass:
      return std::string(kSelfReportedSolution);
  }
  return std::string(kOffByOneSolution);
}

Result<TaskSpec> make_reference_task(TaskId id, PolicyId policy,
                                     PolicyGeneration policy_generation,
                                     std::uint32_t candidate_budget, BudgetLimits budgets) {
  if (!id.valid()) {
    return Status(ErrorCode::NullIdentity, "reference task requires a task identity");
  }
  if (!policy.valid()) {
    return Status(ErrorCode::NullIdentity, "reference task requires a policy identity");
  }
  if (!policy_generation.valid()) {
    return Status(ErrorCode::InvalidArgument, "reference task requires a policy generation");
  }
  if (candidate_budget == 0) {
    return Status(ErrorCode::InvalidArgument, "reference task requires a non-zero candidate budget");
  }

  TaskSpec task;
  task.id = id;
  task.generation = TaskGeneration::first();
  task.name = "reference-sum-of-squares";
  task.objective =
      "Produce a single C++20 translation unit named solution.cpp that implements "
      "af_reference_sum_of_squares exactly as declared in the task input header.";

  InputFile header;
  header.name = std::string(kReferenceTaskHeaderArtifact);
  header.content = reference_api_header_source();
  header.content_digest = sha256_hex(header.content);
  task.inputs.push_back(std::move(header));

  task.required_outputs.push_back(std::string(kReferenceTaskSourceArtifact));

  EvaluationRequirement compile_and_run;
  compile_and_run.requirement_class = RequirementClass::Mandatory;
  compile_and_run.evaluator_key = std::string(kEvaluatorCompileAndRun);
  compile_and_run.description =
      "Compile the candidate against the fixed conformance harness and run it. "
      "The process must exit with status 0 and report AF_REFERENCE_OK.";
  compile_and_run.ranking_weight = 1.0;
  task.requirements.push_back(std::move(compile_and_run));

  EvaluationRequirement source_policy;
  source_policy.requirement_class = RequirementClass::Mandatory;
  source_policy.evaluator_key = std::string(kEvaluatorSourcePolicy);
  source_policy.description =
      "Static policy over the candidate source: the required symbol must be defined and "
      "no forbidden construct may appear.";
  source_policy.ranking_weight = 1.0;
  task.requirements.push_back(std::move(source_policy));

  EvaluationRequirement performance;
  performance.requirement_class = RequirementClass::Optional;
  performance.evaluator_key = std::string(kEvaluatorPerformance);
  performance.description =
      "Measured calls per second of the reference workload. Real elapsed time, not an estimate.";
  performance.ranking_weight = 1.0;
  task.requirements.push_back(std::move(performance));

  EvaluationRequirement parsimony;
  parsimony.requirement_class = RequirementClass::Optional;
  parsimony.evaluator_key = std::string(kEvaluatorParsimony);
  parsimony.description =
      "Reference structural metric: number of identifier-like tokens in the candidate source.";
  parsimony.ranking_weight = 1.0;
  task.requirements.push_back(std::move(parsimony));

  task.hard_constraints.push_back("the candidate must compile with the configured C++20 compiler");
  task.hard_constraints.push_back("the candidate must not spawn processes or open sockets");

  task.closure_criteria.push_back("every mandatory requirement has a complete authoritative record");
  task.closure_criteria.push_back("a selection decision is committed for the population generation");

  task.policy = policy;
  task.policy_generation = policy_generation;
  task.budgets = budgets;

  std::string digest;
  AF_TRY_ASSIGN(digest, compute_task_digest(task));
  task.content_digest = std::move(digest);

  AF_TRY(validate_task(task));
  return task;
}

}  // namespace autonomous_foundry
