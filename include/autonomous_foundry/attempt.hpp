#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"

// Production attempts and the assignments that authorize them.
//
// An attempt is the foundry's record of one bounded attempt to produce one
// candidate on one worker incarnation. Dispatch is not completion. When a
// worker dies after accepting an assignment, the foundry records
// OutcomeUnknown: it does not invent success, and it does not invent failure
// either, because both would be fabrications about work it did not observe.

namespace autonomous_foundry {

enum class AttemptState : std::uint8_t {
  Created = 0,
  /// An attempt identity exists and budgets are reserved, but nothing was
  /// sent to a worker.
  Authorized = 1,
  /// An assignment frame was durably recorded as sent. Acknowledged or not,
  /// the worker may have started working.
  Dispatched = 2,
  /// The worker acknowledged the assignment.
  Running = 3,
  /// The worker published a candidate result for this attempt.
  Published = 4,
  /// The candidate produced by this attempt is under evaluation.
  Evaluating = 5,
  /// Terminal: the attempt completed and its candidate is authoritative.
  Completed = 6,
  /// Terminal: the attempt failed observably.
  Failed = 7,
  /// Terminal: the attempt was cancelled before an authoritative result.
  Cancelled = 8,
  /// Terminal: the foundry cannot determine whether the work completed.
  OutcomeUnknown = 9,
};

inline constexpr std::size_t kAttemptStateCount = 10;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view attempt_state_name(
    AttemptState state) noexcept;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool attempt_state_is_terminal(
    AttemptState state) noexcept;

struct AttemptRecord {
  AttemptId id;
  AttemptGeneration generation;

  PopulationId population;
  PopulationGeneration population_generation;

  TaskId task;
  TaskGeneration task_generation;

  CandidateId candidate;
  CandidateGeneration candidate_generation;

  WorkerId worker;
  WorkerBootId worker_boot;
  SessionId session;
  AssignmentId assignment;

  CoordinatorEpoch authorized_epoch;
  CoordinatorEpoch dispatched_epoch;
  CoordinatorEpoch terminal_epoch;

  AttemptState state{AttemptState::Created};

  /// Zero for a first attempt; incremented for every retry of the same
  /// candidate slot. A retry never reuses the previous attempt identity.
  std::uint32_t retry_index{0};
  AttemptId previous_attempt;

  std::string failure_detail;

  /// True while this attempt holds reserved budget that has not yet been
  /// converted into consumption or released.
  bool holds_reservation{false};

  friend bool operator==(const AttemptRecord&, const AttemptRecord&) noexcept = default;
};

struct AssignmentRecord {
  AssignmentId id;
  AssignmentGeneration generation;

  AttemptId attempt;
  AttemptGeneration attempt_generation;

  WorkerId worker;
  WorkerBootId worker_boot;
  SessionId session;
  WorkerSessionGeneration session_generation;

  CoordinatorEpoch issued_epoch;
  bool acknowledged{false};
  CoordinatorEpoch acknowledged_epoch;
  bool revoked{false};
  CoordinatorEpoch revoked_epoch;

  friend bool operator==(const AssignmentRecord&, const AssignmentRecord&) noexcept = default;
};

/// Work package handed to a worker. Contains everything the worker needs to
/// produce a candidate without contacting any other system.
struct AttemptPackage {
  AttemptId attempt;
  AttemptGeneration attempt_generation;
  AssignmentId assignment;

  PopulationId population;
  PopulationGeneration population_generation;

  TaskId task;
  TaskGeneration task_generation;

  CandidateId candidate;
  CandidateGeneration candidate_generation;

  std::string task_name;
  std::string objective;
  std::vector<std::string> required_outputs;
  std::vector<std::pair<std::string, std::string>> input_files;

  /// Absolute workspace root the worker must confine its output to.
  std::string workspace_root;

  std::string reference_strategy;

  friend bool operator==(const AttemptPackage&, const AttemptPackage&) noexcept = default;
};

}  // namespace autonomous_foundry
