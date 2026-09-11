#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"

// Authority.
//
// Autonomous Foundry separates four things that informal runtimes conflate:
//
//   historical truth   what provably happened, recorded durably
//   durable identity   which object a record is about, stable across restarts
//   recovered state    what a new coordinator believes after reading storage
//   live authority     the right to mutate current state, right now
//
// Authority is explicitly bound to every mutable source that can make a
// decision stale. An authorization never survives an arbitrary change in the
// coordinator epoch, the worker boot incarnation, the run, or any population,
// task, candidate, attempt, assignment, evaluation or policy generation.

namespace autonomous_foundry {

/// Live authority of one connected worker incarnation.
struct WorkerSessionAuthority {
  CoordinatorEpoch coordinator_epoch;
  FoundryRunId run;
  WorkerId worker;
  WorkerBootId boot;
  SessionId session;
  WorkerSessionGeneration session_generation;

  friend bool operator==(const WorkerSessionAuthority&,
                         const WorkerSessionAuthority&) noexcept = default;
};

/// Live authority of one connected controller (operator or CI harness).
struct ControllerSessionAuthority {
  CoordinatorEpoch coordinator_epoch;
  FoundryRunId run;
  ControllerId controller;
  SessionId session;
  WorkerSessionGeneration session_generation;

  friend bool operator==(const ControllerSessionAuthority&,
                         const ControllerSessionAuthority&) noexcept = default;
};

/// Authority carried by a worker message that mutates candidate or attempt state.
struct WorkerOperationAuthority {
  WorkerSessionAuthority session;
  PopulationId population;
  PopulationGeneration population_generation;
  TaskId task;
  TaskGeneration task_generation;
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  AttemptId attempt;
  AttemptGeneration attempt_generation;
  AssignmentId assignment;

  friend bool operator==(const WorkerOperationAuthority&,
                         const WorkerOperationAuthority&) noexcept = default;
};

/// Authority carried by a controller message.
struct ControllerOperationAuthority {
  ControllerSessionAuthority session;
  PopulationId population;
  PopulationGeneration population_generation;
  TaskId task;
  TaskGeneration task_generation;
  PolicyId policy;
  PolicyGeneration policy_generation;

  friend bool operator==(const ControllerOperationAuthority&,
                         const ControllerOperationAuthority&) noexcept = default;
};

/// Authority that binds a prepared decision to the exact canonical state it
/// was derived from. Selection and retention use this so that
///   evaluate state N -> mutate candidate -> commit stale decision from N
/// cannot happen.
struct PreparedDecisionAuthority {
  CoordinatorEpoch coordinator_epoch;
  PopulationId population;
  PopulationGeneration population_generation;
  PolicyId policy;
  PolicyGeneration policy_generation;
  /// SHA-256 over the canonical encoding of every input the decision used.
  std::string canonical_state_digest;

  friend bool operator==(const PreparedDecisionAuthority&,
                         const PreparedDecisionAuthority&) noexcept = default;
};

// -- individual checks ------------------------------------------------------

[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_coordinator_epoch(CoordinatorEpoch expected,
                                                                   CoordinatorEpoch actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_run(FoundryRunId expected, FoundryRunId actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_worker_boot(WorkerBootId expected,
                                                             WorkerBootId actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_session(SessionId expected, SessionId actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_session_generation(
    WorkerSessionGeneration expected, WorkerSessionGeneration actual);

template <typename Tag>
[[nodiscard]] inline Status check_identity(StrongId<Tag> expected, StrongId<Tag> actual,
                                           std::string_view field) {
  if (expected != actual) {
    return Status(ErrorCode::StaleAuthority,
                  "authority field '" + std::string(field) + "' names " + expected.to_string() +
                      " but current state is " + actual.to_string());
  }
  return Status();
}

/// Generation checks report the domain specific stale code so callers can
/// implement per-domain revalidation without string matching.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_population_generation(
    PopulationGeneration expected, PopulationGeneration actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_task_generation(TaskGeneration expected,
                                                                 TaskGeneration actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_candidate_generation(
    CandidateGeneration expected, CandidateGeneration actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_evaluation_generation(
    EvaluationGeneration expected, EvaluationGeneration actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_policy_generation(PolicyGeneration expected,
                                                                   PolicyGeneration actual);
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_attempt_generation(AttemptGeneration expected,
                                                                     AttemptGeneration actual);

/// True when the derived state digest still matches current state.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status check_canonical_digest(
    std::string_view expected, std::string_view actual);

}  // namespace autonomous_foundry
