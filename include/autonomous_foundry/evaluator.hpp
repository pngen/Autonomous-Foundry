#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/process.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/workspace.hpp"

// Evaluation engine.
//
// The reference evaluator launches a real compiler and a real test binary and
// observes the actual exit status. Process creation alone is never treated as
// evaluation: the exit code, the produced output and the expected token are all
// checked. Nothing in this engine consults a worker's self report.

namespace autonomous_foundry {

/// Location of a usable C++ toolchain for the compile-backed evaluator.
struct ReferenceToolchain {
  std::filesystem::path compiler;
  std::filesystem::path include_directories;
  std::filesystem::path library_directories;
  /// "msvc" or "gnu" -- selects the argument spelling.
  std::string flavour;

  [[nodiscard]] bool usable() const noexcept { return !compiler.empty(); }
  [[nodiscard]] std::string describe() const;
};

/// Resolve a toolchain.
///
/// Order: the AF_REFERENCE_CXX environment variable (with AF_REFERENCE_INCLUDE
/// and AF_REFERENCE_LIB), then a compiler already on PATH with a working
/// environment. Returns Unsupported when nothing usable is present. The
/// evaluator reports UNSUPPORTED in that case; it never reports PASS.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ReferenceToolchain> resolve_reference_toolchain();

/// Inputs an evaluator needs. Candidate source arrives as bytes, never as a
/// path supplied by the worker.
struct EvaluationRequest {
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  TaskId task;
  TaskGeneration task_generation;
  PopulationId population;
  PopulationGeneration population_generation;

  /// Declared artifacts of the candidate: logical name to content.
  std::map<std::string, std::string> artifacts;

  /// Task input files, keyed by logical name.
  std::map<std::string, std::string> inputs;

  std::string evaluator_key;
  RequirementClass requirement_class{RequirementClass::Mandatory};

  /// Scratch directory the evaluator may use. Confined by the caller.
  std::filesystem::path scratch_directory;

  ReferenceToolchain toolchain;
};

struct EvaluationResult {
  EvaluationOutcome outcome{EvaluationOutcome::Unknown};
  bool has_score{false};
  double score{0.0};
  std::string diagnostics;
  std::string evidence_digest;
  std::uint64_t duration_micros{0};
};

/// One named evaluator.
class AUTONOMOUS_FOUNDRY_API Evaluator {
 public:
  Evaluator() = default;
  virtual ~Evaluator();
  Evaluator(const Evaluator&) = delete;
  Evaluator& operator=(const Evaluator&) = delete;

  [[nodiscard]] virtual std::string key() const = 0;
  [[nodiscard]] virtual EvaluatorKind kind() const = 0;
  [[nodiscard]] virtual EvaluationResult evaluate(const EvaluationRequest& request) = 0;
};

/// Compile the candidate source against the task's fixed test harness with a
/// real compiler, then run the produced binary and check its exit code and its
/// output token. This is the mandatory hard gate of the reference task.
class AUTONOMOUS_FOUNDRY_API CompileAndRunEvaluator final : public Evaluator {
 public:
  [[nodiscard]] std::string key() const override;
  [[nodiscard]] EvaluatorKind kind() const override { return EvaluatorKind::ProcessCommand; }
  [[nodiscard]] EvaluationResult evaluate(const EvaluationRequest& request) override;
};

/// Deterministic static rule over the candidate source: rejects constructs the
/// task forbids. Runs in process, never executes candidate code.
class AUTONOMOUS_FOUNDRY_API SourcePolicyEvaluator final : public Evaluator {
 public:
  [[nodiscard]] std::string key() const override;
  [[nodiscard]] EvaluatorKind kind() const override { return EvaluatorKind::SourcePolicy; }
  [[nodiscard]] EvaluationResult evaluate(const EvaluationRequest& request) override;
};

/// Compile the candidate and run a larger workload, measuring real elapsed
/// time. Produces a score; never satisfies a mandatory requirement.
class AUTONOMOUS_FOUNDRY_API PerformanceEvaluator final : public Evaluator {
 public:
  [[nodiscard]] std::string key() const override;
  [[nodiscard]] EvaluatorKind kind() const override { return EvaluatorKind::ProcessCommand; }
  [[nodiscard]] EvaluationResult evaluate(const EvaluationRequest& request) override;
};

/// Deterministic structural metric over the candidate source: token count.
/// Explicitly labelled as a reference parsimony metric, not a quality claim.
class AUTONOMOUS_FOUNDRY_API ParsimonyEvaluator final : public Evaluator {
 public:
  [[nodiscard]] std::string key() const override;
  [[nodiscard]] EvaluatorKind kind() const override { return EvaluatorKind::PortableReference; }
  [[nodiscard]] EvaluationResult evaluate(const EvaluationRequest& request) override;
};

/// Registry mapping evaluator keys to implementations.
class AUTONOMOUS_FOUNDRY_API EvaluatorRegistry {
 public:
  EvaluatorRegistry();
  void add(std::shared_ptr<Evaluator> evaluator);
  [[nodiscard]] std::shared_ptr<Evaluator> find(std::string_view key) const;
  [[nodiscard]] std::vector<std::string> keys() const;

  /// The reference registry: compile-and-run, source-policy, performance and
  /// parsimony.
  [[nodiscard]] static EvaluatorRegistry make_reference();

 private:
  std::vector<std::shared_ptr<Evaluator>> evaluators_;
};

}  // namespace autonomous_foundry
