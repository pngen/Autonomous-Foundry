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

// Populations.
//
// A population is an explicit runtime object, not a loop counter. It carries
// its own task contract, policy binding, budgets, lifecycle state and closure
// contract. A population that has closed can never be mutated again, and a
// population that still has authoritative work capable of changing the winner
// can never report itself closed.

namespace autonomous_foundry {

enum class PopulationState : std::uint8_t {
  Created = 0,
  Ready = 1,
  Running = 2,
  Evaluating = 3,
  Selecting = 4,
  Advancing = 5,
  /// Something the population depended on changed, or the coordinator
  /// restarted while work was in flight. Fresh authority must be established
  /// before the population may progress.
  RevalidationRequired = 6,
  Closing = 7,
  Closed = 8,
  Failed = 9,
  Cancelled = 10,
};

inline constexpr std::size_t kPopulationStateCount = 11;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view population_state_name(
    PopulationState state) noexcept;
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool population_state_is_terminal(
    PopulationState state) noexcept;
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool population_transition_is_legal(
    PopulationState from, PopulationState to) noexcept;

struct PopulationSpec {
  std::string name;

  TaskId task;
  TaskGeneration task_generation;

  PolicyId policy;
  PolicyGeneration policy_generation;

  /// Maximum number of candidate slots in this population.
  std::uint32_t candidate_budget{8};
  /// Maximum number of workers this population may occupy at once.
  std::uint32_t worker_budget{4};

  /// Evolution index: P1 of a run is 1, the population seeded by the winner of
  /// P1 is 2, and so on. Purely descriptive; authority never depends on it.
  std::uint32_t population_index{1};

  /// Predecessor population for index > 1.
  PopulationId predecessor;
  CandidateId seed_candidate;
  CandidateGeneration seed_candidate_generation;
  LineageId seed_lineage;

  /// When true the population cannot close until the selected candidate has a
  /// committed promotion request.
  bool require_promotion_request{false};

  friend bool operator==(const PopulationSpec&, const PopulationSpec&) noexcept = default;
};

struct PopulationRecord {
  PopulationId id;
  PopulationGeneration generation;

  FoundryRunId run;
  std::string name;

  TaskId task;
  TaskGeneration task_generation;

  PolicyId policy;
  PolicyGeneration policy_generation;

  std::uint32_t candidate_budget{8};
  std::uint32_t worker_budget{4};
  std::uint32_t population_index{1};

  PopulationId predecessor;
  CandidateId seed_candidate;
  CandidateGeneration seed_candidate_generation;
  LineageId seed_lineage;

  PopulationState state{PopulationState::Created};
  CoordinatorEpoch created_epoch;
  CoordinatorEpoch state_epoch;

  /// Committed selection generation, or the invalid value when none is
  /// committed. Only a committed decision makes a winner authoritative.
  SelectionGeneration committed_selection;
  bool retention_committed{false};
  SelectionGeneration retention_selection_generation;

  bool require_promotion_request{false};

  std::vector<CandidateId> candidates;
  std::uint64_t promotion_requests{0};
  PromotionRequestId committed_promotion_request;

  /// Populated when the population is in RevalidationRequired or Failed so an
  /// operator can see the concrete reason.
  std::string status_detail;
  std::vector<std::string> closure_blockers;

  friend bool operator==(const PopulationRecord&, const PopulationRecord&) noexcept = default;
};

/// Accounting snapshot for one population, kept beside the record so that the
/// ledger survives serialization without becoming part of the record identity.
struct PopulationAccounting {
  PopulationId population;
  PopulationGeneration generation;
  BudgetLedger ledger;
};

}  // namespace autonomous_foundry
