#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/worker.hpp"

// The reference autonomous production task.
//
// The task asks a worker to produce a single C++ translation unit implementing
// a fixed API. The evaluator compiles that translation unit against a fixed
// harness that the worker never sees and cannot influence, runs the resulting
// program, and checks its real exit status and output token.
//
// The foundry discovers which candidate is correct from execution and
// evaluation. No test hard-codes a winner identity.

namespace autonomous_foundry {

/// The API the reference task asks for.
///
///   std::uint64_t af_reference_sum_of_squares(std::uint32_t n);
///
/// returns sum(i*i for i in 1..n) computed with wrapping 64-bit arithmetic.
inline constexpr std::string_view kReferenceTaskApiName = "af_reference_sum_of_squares";
inline constexpr std::string_view kReferenceTaskSourceArtifact = "solution.cpp";
inline constexpr std::string_view kReferenceTaskHeaderArtifact = "af_reference.hpp";

/// Deterministic expected value, computed by an independent slow method.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::uint64_t reference_expected_value(
    std::uint32_t n) noexcept;

/// The fixed public header handed to workers as task input.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string reference_api_header_source();

/// The fixed conformance harness. Never derived from worker input.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string reference_test_harness_source();

/// The fixed performance harness used by the optional performance evaluator.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string reference_benchmark_harness_source();

/// Generate a candidate implementation for a strategy.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string generate_reference_solution(
    ReferenceStrategy strategy);

/// Build the reference task contract. The evaluator keys referenced here are
/// exactly the keys the reference evaluator registry provides.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<TaskSpec> make_reference_task(
    TaskId id, PolicyId policy, PolicyGeneration policy_generation, std::uint32_t candidate_budget,
    BudgetLimits budgets);

/// Evaluator keys of the reference task.
inline constexpr std::string_view kEvaluatorCompileAndRun = "compile-and-run";
inline constexpr std::string_view kEvaluatorSourcePolicy = "source-policy";
inline constexpr std::string_view kEvaluatorPerformance = "performance";
inline constexpr std::string_view kEvaluatorParsimony = "parsimony";

}  // namespace autonomous_foundry
