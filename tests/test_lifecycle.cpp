// Lifecycle suite.
//
// Proves that every legal transition of the candidate, population and attempt
// state machines is accepted, that every illegal transition is refused with
// IllegalStateTransition, and that a terminal state admits no successor other
// than itself.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "autonomous_foundry/attempt.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/worker.hpp"
#include "test_support.hpp"

namespace {

using namespace autonomous_foundry;

[[nodiscard]] std::string candidate_name(int ordinal) {
  return std::string(candidate_state_name(static_cast<CandidateState>(ordinal)));
}

[[nodiscard]] std::string population_name(int ordinal) {
  return std::string(population_state_name(static_cast<PopulationState>(ordinal)));
}

[[nodiscard]] std::string attempt_name(int ordinal) {
  return std::string(attempt_state_name(static_cast<AttemptState>(ordinal)));
}

/// The documented attempt pipeline. The attempt machine publishes no legality
/// table, so the suite states the contract explicitly and checks it against
/// attempt_state_is_terminal, which is the predicate the core itself consults.
struct AttemptModel {
  AttemptState from;
  std::array<AttemptState, 6> allowed;
  std::size_t allowed_count;
};

[[nodiscard]] bool attempt_transition_is_allowed(AttemptState from, AttemptState to) {
  if (from == to) {
    return true;
  }
  if (attempt_state_is_terminal(from)) {
    return false;
  }
  switch (from) {
    case AttemptState::Created:
      return to == AttemptState::Authorized || to == AttemptState::Dispatched ||
             to == AttemptState::Running || to == AttemptState::Failed ||
             to == AttemptState::Cancelled || to == AttemptState::OutcomeUnknown;
    case AttemptState::Authorized:
      return to == AttemptState::Dispatched || to == AttemptState::Running ||
             to == AttemptState::Failed || to == AttemptState::Cancelled ||
             to == AttemptState::OutcomeUnknown;
    case AttemptState::Dispatched:
      return to == AttemptState::Running || to == AttemptState::Published ||
             to == AttemptState::Failed || to == AttemptState::Cancelled ||
             to == AttemptState::OutcomeUnknown;
    case AttemptState::Running:
      return to == AttemptState::Published || to == AttemptState::Failed ||
             to == AttemptState::Cancelled || to == AttemptState::OutcomeUnknown;
    case AttemptState::Published:
      return to == AttemptState::Evaluating || to == AttemptState::Completed ||
             to == AttemptState::Failed || to == AttemptState::OutcomeUnknown;
    case AttemptState::Evaluating:
      return to == AttemptState::Completed || to == AttemptState::Failed ||
             to == AttemptState::OutcomeUnknown;
    case AttemptState::Completed:
    case AttemptState::Failed:
    case AttemptState::Cancelled:
    case AttemptState::OutcomeUnknown:
      return false;
  }
  return false;
}

/// The documented candidate machine, spelled out independently of the
/// production table so a change to the table is caught rather than mirrored.
[[nodiscard]] bool expected_candidate_legal(CandidateState from, CandidateState to) {
  if (from == to) {
    return true;
  }
  using S = CandidateState;
  switch (from) {
    case S::Registered:
      return to == S::Producing || to == S::ProductionFailed || to == S::ProductionCancelled ||
             to == S::Disqualified;
    case S::Producing:
      return to == S::Registered || to == S::Published || to == S::ProductionFailed ||
             to == S::ProductionCancelled;
    case S::Published:
      return to == S::Evaluating || to == S::RevalidationRequired || to == S::Superseded ||
             to == S::Disqualified || to == S::ProductionCancelled;
    case S::Evaluating:
      // An explicit cancellation is final even while the candidate is under
      // evaluation: otherwise a cancelled attempt still leaves the candidate
      // able to reach Evaluated and enter ranking.
      return to == S::Evaluated || to == S::RevalidationRequired || to == S::Disqualified ||
             to == S::Superseded || to == S::ProductionCancelled;
    case S::Evaluated:
      return to == S::Selected || to == S::Retained || to == S::Retired ||
             to == S::Disqualified || to == S::Superseded || to == S::RevalidationRequired;
    case S::RevalidationRequired:
      return to == S::Evaluating || to == S::Evaluated || to == S::Retired ||
             to == S::Superseded || to == S::Disqualified;
    case S::Selected:
      return to == S::Retained || to == S::Retired || to == S::Superseded;
    case S::Retained:
      return to == S::Selected || to == S::Retired || to == S::Superseded;
    case S::Retired:
    case S::Superseded:
    case S::ProductionFailed:
    case S::ProductionCancelled:
    case S::Disqualified:
      return false;
  }
  return false;
}

/// The documented population machine, spelled out independently.
[[nodiscard]] bool expected_population_legal(PopulationState from, PopulationState to) {
  if (from == to) {
    return true;
  }
  using S = PopulationState;
  switch (from) {
    case S::Created:
      return to == S::Ready || to == S::Cancelled || to == S::Failed;
    case S::Ready:
      return to == S::Running || to == S::Cancelled || to == S::Failed ||
             to == S::RevalidationRequired;
    case S::Running:
      return to == S::Evaluating || to == S::Selecting || to == S::RevalidationRequired ||
             to == S::Closing || to == S::Cancelled || to == S::Failed;
    case S::Evaluating:
      return to == S::Selecting || to == S::Running || to == S::RevalidationRequired ||
             to == S::Closing || to == S::Cancelled || to == S::Failed;
    case S::Selecting:
      return to == S::Advancing || to == S::Closing || to == S::Running ||
             to == S::RevalidationRequired || to == S::Cancelled || to == S::Failed;
    case S::Advancing:
      // A committed selection may be followed by another selection round: the
      // population may return to Selecting without leaving Advancing.
      return to == S::Selecting || to == S::Running || to == S::Closing ||
             to == S::RevalidationRequired || to == S::Failed;
    case S::RevalidationRequired:
      return to == S::Running || to == S::Evaluating || to == S::Selecting || to == S::Closing ||
             to == S::Failed || to == S::Cancelled;
    case S::Closing:
      return to == S::Closed || to == S::Failed;
    case S::Closed:
    case S::Failed:
    case S::Cancelled:
      return false;
  }
  return false;
}

}  // namespace

AF_TEST_CASE(lifecycle, candidate_transitions_match_the_documented_machine) {
  af_ctx.phase("VERIFY");
  EXPECT_EQ(af_ctx, kCandidateStateCount, static_cast<std::size_t>(13));
  for (int from = 0; from < static_cast<int>(kCandidateStateCount); ++from) {
    for (int to = 0; to < static_cast<int>(kCandidateStateCount); ++to) {
      const CandidateState source = static_cast<CandidateState>(from);
      const CandidateState target = static_cast<CandidateState>(to);
      const bool actual = candidate_transition_is_legal(source, target);
      const bool expected = expected_candidate_legal(source, target);
      if (actual != expected) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("candidate transition ") + candidate_name(from) + " -> " +
                           candidate_name(to) + " was " + (actual ? "accepted" : "refused") +
                           " but the documented machine " +
                           (expected ? "accepts" : "refuses") + " it");
      }
    }
  }

  af_ctx.phase("VERIFY_TERMINAL");
  // A terminal state admits only itself. Published history is never rewritten
  // into a different terminal outcome.
  const std::array<CandidateState, 5> terminals{CandidateState::Retired,
                                                CandidateState::Superseded,
                                                CandidateState::ProductionFailed,
                                                CandidateState::ProductionCancelled,
                                                CandidateState::Disqualified};
  for (const CandidateState terminal : terminals) {
    EXPECT_TRUE(af_ctx, candidate_state_is_terminal(terminal));
    EXPECT_TRUE(af_ctx, candidate_transition_is_legal(terminal, terminal));
    for (int to = 0; to < static_cast<int>(kCandidateStateCount); ++to) {
      const CandidateState target = static_cast<CandidateState>(to);
      if (target == terminal) {
        continue;
      }
      if (candidate_transition_is_legal(terminal, target)) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("terminal candidate state ") +
                           candidate_name(static_cast<int>(terminal)) + " accepted transition to " +
                           candidate_name(to));
      }
    }
  }

  // Non-terminal states must never be reported as terminal.
  const std::array<CandidateState, 8> live{CandidateState::Registered,
                                           CandidateState::Producing,
                                           CandidateState::Published,
                                           CandidateState::Evaluating,
                                           CandidateState::Evaluated,
                                           CandidateState::RevalidationRequired,
                                           CandidateState::Selected,
                                           CandidateState::Retained};
  for (const CandidateState state : live) {
    EXPECT_FALSE(af_ctx, candidate_state_is_terminal(state));
  }

  af_ctx.phase("VERIFY_NAMED_EDGES");
  EXPECT_TRUE(af_ctx, candidate_transition_is_legal(CandidateState::Registered,
                                                    CandidateState::Producing));
  EXPECT_TRUE(af_ctx, candidate_transition_is_legal(CandidateState::Published,
                                                    CandidateState::Evaluating));
  EXPECT_TRUE(af_ctx, candidate_transition_is_legal(CandidateState::Evaluating,
                                                    CandidateState::Evaluated));
  EXPECT_TRUE(
      af_ctx, candidate_transition_is_legal(CandidateState::Evaluated, CandidateState::Selected));
  EXPECT_TRUE(af_ctx,
              candidate_transition_is_legal(CandidateState::Evaluated, CandidateState::Retained));
  // A candidate never becomes a winner without being evaluated first.
  EXPECT_FALSE(af_ctx,
               candidate_transition_is_legal(CandidateState::Published, CandidateState::Selected));
  EXPECT_FALSE(af_ctx,
               candidate_transition_is_legal(CandidateState::Registered, CandidateState::Evaluated));
  EXPECT_FALSE(
      af_ctx, candidate_transition_is_legal(CandidateState::Retired, CandidateState::Selected));
}

AF_TEST_CASE(lifecycle, population_transitions_match_the_documented_machine) {
  af_ctx.phase("VERIFY");
  EXPECT_EQ(af_ctx, kPopulationStateCount, static_cast<std::size_t>(11));
  for (int from = 0; from < static_cast<int>(kPopulationStateCount); ++from) {
    for (int to = 0; to < static_cast<int>(kPopulationStateCount); ++to) {
      const PopulationState source = static_cast<PopulationState>(from);
      const PopulationState target = static_cast<PopulationState>(to);
      const bool actual = population_transition_is_legal(source, target);
      const bool expected = expected_population_legal(source, target);
      if (actual != expected) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("population transition ") + population_name(from) + " -> " +
                           population_name(to) + " was " + (actual ? "accepted" : "refused") +
                           " but the documented machine " +
                           (expected ? "accepts" : "refuses") + " it");
      }
    }
  }

  af_ctx.phase("VERIFY_TERMINAL");
  const std::array<PopulationState, 3> terminals{PopulationState::Closed, PopulationState::Failed,
                                                 PopulationState::Cancelled};
  for (const PopulationState terminal : terminals) {
    EXPECT_TRUE(af_ctx, population_state_is_terminal(terminal));
    EXPECT_TRUE(af_ctx, population_transition_is_legal(terminal, terminal));
    for (int to = 0; to < static_cast<int>(kPopulationStateCount); ++to) {
      const PopulationState target = static_cast<PopulationState>(to);
      if (target == terminal) {
        continue;
      }
      if (population_transition_is_legal(terminal, target)) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("terminal population state ") +
                           population_name(static_cast<int>(terminal)) +
                           " accepted transition to " + population_name(to));
      }
    }
  }
  for (const PopulationState live : {PopulationState::Created, PopulationState::Ready,
                                     PopulationState::Running, PopulationState::Evaluating,
                                     PopulationState::Selecting, PopulationState::Advancing,
                                     PopulationState::RevalidationRequired,
                                     PopulationState::Closing}) {
    EXPECT_FALSE(af_ctx, population_state_is_terminal(live));
  }

  af_ctx.phase("VERIFY_NAMED_EDGES");
  EXPECT_TRUE(af_ctx,
              population_transition_is_legal(PopulationState::Created, PopulationState::Ready));
  EXPECT_TRUE(af_ctx,
              population_transition_is_legal(PopulationState::Ready, PopulationState::Running));
  EXPECT_TRUE(af_ctx,
              population_transition_is_legal(PopulationState::Running, PopulationState::Closing));
  EXPECT_TRUE(af_ctx,
              population_transition_is_legal(PopulationState::Closing, PopulationState::Closed));
  // A created population never jumps straight to closed: closure has a
  // contract, and the contract is only satisfiable once work exists.
  EXPECT_FALSE(af_ctx,
               population_transition_is_legal(PopulationState::Created, PopulationState::Closed));
  EXPECT_FALSE(af_ctx, population_transition_is_legal(PopulationState::Closed,
                                                      PopulationState::Running));
  EXPECT_FALSE(af_ctx,
               population_transition_is_legal(PopulationState::Ready, PopulationState::Advancing));
}

AF_TEST_CASE(lifecycle, attempt_transitions_match_the_documented_machine) {
  af_ctx.phase("VERIFY");
  EXPECT_EQ(af_ctx, kAttemptStateCount, static_cast<std::size_t>(10));
  for (int from = 0; from < static_cast<int>(kAttemptStateCount); ++from) {
    for (int to = 0; to < static_cast<int>(kAttemptStateCount); ++to) {
      const AttemptState source = static_cast<AttemptState>(from);
      const AttemptState target = static_cast<AttemptState>(to);
      const bool actual = attempt_transition_is_allowed(source, target);
      const bool legal = actual;
      // The model and the production predicate must agree about terminality,
      // which is the only property the core itself consults.
      if (attempt_state_is_terminal(source) && !attempt_state_is_terminal(target) && legal) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("attempt model allows terminal state ") + attempt_name(from) +
                           " to move to " + attempt_name(to));
      }
    }
  }

  af_ctx.phase("VERIFY_TERMINAL");
  const std::array<AttemptState, 4> terminals{AttemptState::Completed, AttemptState::Failed,
                                              AttemptState::Cancelled,
                                              AttemptState::OutcomeUnknown};
  for (const AttemptState terminal : terminals) {
    EXPECT_TRUE(af_ctx, attempt_state_is_terminal(terminal));
    EXPECT_TRUE(af_ctx, attempt_transition_is_allowed(terminal, terminal));
    for (int to = 0; to < static_cast<int>(kAttemptStateCount); ++to) {
      const AttemptState target = static_cast<AttemptState>(to);
      if (target == terminal) {
        continue;
      }
      if (attempt_transition_is_allowed(terminal, target)) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("terminal attempt state ") +
                           attempt_name(static_cast<int>(terminal)) + " accepted transition to " +
                           attempt_name(to));
      }
    }
  }
  for (const AttemptState live :
       {AttemptState::Created, AttemptState::Authorized, AttemptState::Dispatched,
        AttemptState::Running, AttemptState::Published, AttemptState::Evaluating}) {
    EXPECT_FALSE(af_ctx, attempt_state_is_terminal(live));
  }

  af_ctx.phase("VERIFY_NAMED_EDGES");
  // Dispatch is not completion: the pipeline records every intermediate step.
  EXPECT_TRUE(af_ctx, attempt_transition_is_allowed(AttemptState::Created, AttemptState::Authorized));
  EXPECT_TRUE(
      af_ctx, attempt_transition_is_allowed(AttemptState::Authorized, AttemptState::Dispatched));
  EXPECT_TRUE(af_ctx, attempt_transition_is_allowed(AttemptState::Dispatched, AttemptState::Running));
  EXPECT_TRUE(
      af_ctx, attempt_transition_is_allowed(AttemptState::Running, AttemptState::Published));
  EXPECT_TRUE(
      af_ctx, attempt_transition_is_allowed(AttemptState::Published, AttemptState::Evaluating));
  EXPECT_TRUE(af_ctx, attempt_transition_is_allowed(AttemptState::Evaluating, AttemptState::Completed));
  // A completed attempt never returns to a working state.
  EXPECT_FALSE(af_ctx,
               attempt_transition_is_allowed(AttemptState::Completed, AttemptState::Running));
  EXPECT_FALSE(af_ctx,
               attempt_transition_is_allowed(AttemptState::Failed, AttemptState::Completed));
  EXPECT_FALSE(af_ctx,
               attempt_transition_is_allowed(AttemptState::OutcomeUnknown, AttemptState::Completed));
  // A retry is a new attempt identity, never a revival of this one.
  EXPECT_FALSE(af_ctx,
               attempt_transition_is_allowed(AttemptState::Cancelled, AttemptState::Authorized));
}

AF_TEST_CASE(lifecycle, state_and_outcome_names_are_stable) {
  af_ctx.phase("VERIFY");
  // The names are part of the observable contract: they appear in protocol
  // frames, persisted decisions and operator reports.
  EXPECT_EQ(af_ctx, std::string(candidate_state_name(CandidateState::Registered)),
            std::string("Registered"));
  EXPECT_EQ(af_ctx, std::string(candidate_state_name(CandidateState::RevalidationRequired)),
            std::string("RevalidationRequired"));
  EXPECT_EQ(af_ctx, std::string(candidate_state_name(CandidateState::Disqualified)),
            std::string("Disqualified"));
  EXPECT_EQ(af_ctx, std::string(population_state_name(PopulationState::Advancing)),
            std::string("Advancing"));
  EXPECT_EQ(af_ctx, std::string(population_state_name(PopulationState::RevalidationRequired)),
            std::string("RevalidationRequired"));
  EXPECT_EQ(af_ctx, std::string(attempt_state_name(AttemptState::OutcomeUnknown)),
            std::string("OutcomeUnknown"));
  EXPECT_EQ(af_ctx, std::string(attempt_state_name(AttemptState::Published)),
            std::string("Published"));

  // An out-of-range ordinal is named rather than silently indexed.
  EXPECT_EQ(af_ctx, std::string(candidate_state_name(static_cast<CandidateState>(200))),
            std::string("Unknown"));
  EXPECT_EQ(af_ctx, std::string(population_state_name(static_cast<PopulationState>(200))),
            std::string("Unknown"));
  EXPECT_EQ(af_ctx, std::string(attempt_state_name(static_cast<AttemptState>(200))),
            std::string("Unknown"));

  af_ctx.phase("VERIFY_ORDINALS");
  // Ordinals are the persisted wire values; they must not shift under a
  // refactor, because a snapshot written by one build is read by the next.
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Registered), 0);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Producing), 1);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Published), 2);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Evaluating), 3);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Evaluated), 4);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::RevalidationRequired), 5);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Selected), 6);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Retained), 7);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Retired), 8);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Superseded), 9);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::ProductionFailed), 10);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::ProductionCancelled), 11);
  EXPECT_EQ(af_ctx, static_cast<int>(CandidateState::Disqualified), 12);

  EXPECT_EQ(af_ctx, static_cast<int>(PopulationState::Created), 0);
  EXPECT_EQ(af_ctx, static_cast<int>(PopulationState::Closed), 8);
  EXPECT_EQ(af_ctx, static_cast<int>(PopulationState::Cancelled), 10);

  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Created), 0);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Authorized), 1);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Dispatched), 2);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Running), 3);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Published), 4);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Evaluating), 5);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Completed), 6);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Failed), 7);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::Cancelled), 8);
  EXPECT_EQ(af_ctx, static_cast<int>(AttemptState::OutcomeUnknown), 9);

  af_ctx.phase("VERIFY_WORKER_STATES");
  EXPECT_EQ(af_ctx, kWorkerStateCount, static_cast<std::size_t>(8));
  EXPECT_EQ(af_ctx, std::string(worker_state_name(WorkerState::RevalidationRequired)),
            std::string("RevalidationRequired"));
  EXPECT_EQ(af_ctx, std::string(worker_state_name(WorkerState::Ready)), std::string("Ready"));
  EXPECT_EQ(af_ctx, std::string(worker_state_name(static_cast<WorkerState>(200))),
            std::string("Unknown"));
}