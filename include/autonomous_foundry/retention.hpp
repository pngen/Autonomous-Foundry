#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"

// Retention.
//
// Selection picks the candidate allowed to advance. Retention decides which
// alternatives remain worth keeping. They are different questions, and the
// runtime answers them with different policies. Retention is bounded, is
// deterministic under a deterministic policy, and never destroys provenance:
// a retired candidate stays historically inspectable forever.

namespace autonomous_foundry {

enum class RetentionOutcome : std::uint8_t {
  Retained = 0,
  RetainedBecauseSelected = 1,
  RetainedForLineageDiversity = 2,
  RetiredRankedBelowCut = 3,
  RetiredLineageCap = 4,
  RetiredCapacityLimit = 5,
  RetiredIneligible = 6,
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view retention_outcome_name(
    RetentionOutcome outcome) noexcept;

struct RetentionDecisionEntry {
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  RetentionOutcome outcome{RetentionOutcome::RetiredIneligible};
  std::uint32_t rank{0};
  std::string lineage_key;
  std::string detail;

  friend bool operator==(const RetentionDecisionEntry&,
                         const RetentionDecisionEntry&) noexcept = default;
};

struct RetentionDecision {
  SelectionGeneration selection_generation;

  PopulationId population;
  PopulationGeneration population_generation;

  PolicyId policy;
  PolicyGeneration policy_generation;

  CoordinatorEpoch coordinator_epoch;

  std::vector<RetentionDecisionEntry> entries;

  /// Sorted identity lists, derived from entries, for convenient consumption.
  std::vector<CandidateId> retained;
  std::vector<CandidateId> retired;

  std::string canonical_state_digest;
  bool committed{false};

  friend bool operator==(const RetentionDecision&, const RetentionDecision&) noexcept = default;
};

}  // namespace autonomous_foundry
