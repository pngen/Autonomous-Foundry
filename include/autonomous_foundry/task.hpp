#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/limits.hpp"

// The durable task contract.
//
// A task is what makes evaluation meaningful: without a durable, versioned
// objective, a candidate can be scored against the wrong thing and nobody can
// tell. Task mutation always advances TaskGeneration and always changes the
// task content digest, so a candidate evaluated against generation N can be
// detected as incompatible under generation N+1 instead of silently remaining
// selected.

namespace autonomous_foundry {

/// A file handed to every worker producing a candidate for the task.
struct InputFile {
  /// Logical name. Validated with the same rules as artifact names.
  std::string name;
  std::string content;
  /// Lowercase hexadecimal SHA-256 of content, verified on ingest.
  std::string content_digest;

  [[nodiscard]] std::uint64_t size_bytes() const noexcept {
    return static_cast<std::uint64_t>(content.size());
  }
  friend bool operator==(const InputFile&, const InputFile&) noexcept = default;
};

enum class RequirementClass : std::uint8_t {
  /// Must be satisfied by a complete authoritative PASS. An UNKNOWN, FAIL,
  /// UNSUPPORTED, ERROR or CANCELLED outcome does not satisfy it.
  Mandatory = 0,
  /// Contributes to ranking only.
  Optional = 1,
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view requirement_class_name(
    RequirementClass cls) noexcept;

struct EvaluationRequirement {
  RequirementClass requirement_class{RequirementClass::Mandatory};
  /// Stable evaluator key, for example "compile-and-run".
  std::string evaluator_key;
  std::string description;
  /// Optional score floor. Only consulted for optional requirements.
  bool has_score_floor{false};
  double score_floor{0.0};
  /// Weight applied when this requirement contributes to ranking.
  double ranking_weight{1.0};

  friend bool operator==(const EvaluationRequirement&, const EvaluationRequirement&) noexcept =
      default;
};

/// Immutable task contract.
struct TaskSpec {
  TaskId id;
  TaskGeneration generation;

  std::string name;
  std::string objective;

  std::vector<InputFile> inputs;
  /// Logical artifact names every accepted candidate must declare.
  std::vector<std::string> required_outputs;
  std::vector<EvaluationRequirement> requirements;
  /// Human readable hard constraints. Machine enforcement happens through
  /// mandatory evaluation requirements; these are carried so that an external
  /// promotion system can re-read the intent.
  std::vector<std::string> hard_constraints;
  std::vector<std::string> closure_criteria;

  PolicyId policy;
  PolicyGeneration policy_generation;

  BudgetLimits budgets;

  /// Lowercase hexadecimal SHA-256 over the canonical task encoding.
  std::string content_digest;

  [[nodiscard]] const EvaluationRequirement* find_requirement(std::string_view key) const noexcept;
  [[nodiscard]] std::size_t mandatory_requirement_count() const noexcept;
  friend bool operator==(const TaskSpec&, const TaskSpec&) noexcept = default;
};

/// Canonical, order independent byte encoding used for digests.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string encode_task_canonical(const TaskSpec& task);

/// Recompute and validate the task digest. Returns the digest on success.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::string> compute_task_digest(
    const TaskSpec& task);

/// Full structural validation for a task as it arrives from a snapshot or an
/// operator command.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_task(const TaskSpec& task);

/// True when the two tasks describe the same objective contract. A candidate
/// evaluated under a task whose digest differs is not compatible.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool tasks_are_compatible(const TaskSpec& a,
                                                               const TaskSpec& b) noexcept;

}  // namespace autonomous_foundry
