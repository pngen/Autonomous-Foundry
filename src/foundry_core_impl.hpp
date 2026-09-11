#pragma once

// Internal implementation state for FoundryCore.
//
// This header is private to the library. It exists so that the foundry state
// machine can be implemented across more than one translation unit without
// exposing any of its internals through the public include tree.
//
// CONCURRENCY CONTRACT
//
//   * Exactly one lock guards all mutable state: Impl::mutex.
//   * Every public FoundryCore method takes either a shared lock (read paths,
//     returning immutable copies) or the exclusive lock (one validated
//     transition). No public method ever takes the lock twice, and no lock is
//     ever upgraded.
//   * No method of this class performs I/O, waits on a process, calls a
//     callback, joins a thread or touches a socket. The coordinator performs
//     all of those outside the lock.
//   * Methods suffixed _locked assume the caller already holds the lock in the
//     required mode and never take it again.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "autonomous_foundry/foundry.hpp"

namespace autonomous_foundry {

/// Maximum number of parents a candidate may declare.
inline constexpr std::size_t kMaxCandidateParents = 8;

/// Maximum number of production attempts spent on one candidate slot.
inline constexpr std::uint32_t kMaxAttemptsPerCandidate = 16;

class FoundryCore::Impl {
 public:
  explicit Impl(FoundryConfig config) : config_(std::move(config)) {
    foundry_ = config_.foundry;
    run_ = config_.run;
    epoch_ = config_.epoch.valid() ? config_.epoch : CoordinatorEpoch::from_value(1);
    ids_ = IdAllocator(config_.id_salt == 0 ? make_id_salt() : config_.id_salt);
  }

  [[nodiscard]] const FoundryConfig& config() const noexcept { return config_; }

  // -- guarded state --------------------------------------------------------

  mutable std::shared_mutex mutex;

  // -- state ----------------------------------------------------------------

  FoundryConfig config_;
  FoundryId foundry_;
  FoundryRunId run_;
  CoordinatorEpoch epoch_;
  std::uint64_t revision_{0};
  IdAllocator ids_;

  std::unordered_map<TaskId, TaskSpec> tasks_;
  std::unordered_map<PolicyId, FoundryPolicy> policies_;
  std::unordered_map<PopulationId, PopulationRecord> populations_;
  std::unordered_map<CandidateId, CandidateRecord> candidates_;
  std::unordered_map<WorkerId, WorkerRecord> workers_;
  std::unordered_map<AttemptId, AttemptRecord> attempts_;
  std::unordered_map<AssignmentId, AssignmentRecord> assignments_;
  std::unordered_map<EvaluationId, EvaluationRecord> evaluations_;
  std::unordered_map<PopulationId, SelectionDecision> selections_;
  std::unordered_map<PopulationId, RetentionDecision> retentions_;
  std::unordered_map<PromotionRequestId, PromotionRequest> promotion_requests_;
  std::unordered_map<PopulationId, BudgetLedger> ledgers_;
  LineageGraph lineage_;
  FoundryStatistics statistics_;

  // -- small helpers (lock held) -------------------------------------------

  void touch() noexcept { ++revision_; }

  [[nodiscard]] PopulationRecord* population_locked(PopulationId id) {
    const auto found = populations_.find(id);
    return found == populations_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const PopulationRecord* population_locked(PopulationId id) const {
    const auto found = populations_.find(id);
    return found == populations_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] CandidateRecord* candidate_locked(CandidateId id) {
    const auto found = candidates_.find(id);
    return found == candidates_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const CandidateRecord* candidate_locked(CandidateId id) const {
    const auto found = candidates_.find(id);
    return found == candidates_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] AttemptRecord* attempt_locked(AttemptId id) {
    const auto found = attempts_.find(id);
    return found == attempts_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] WorkerRecord* worker_locked(WorkerId id) {
    const auto found = workers_.find(id);
    return found == workers_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] TaskSpec* task_locked(TaskId id) {
    const auto found = tasks_.find(id);
    return found == tasks_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const TaskSpec* task_locked(TaskId id) const {
    const auto found = tasks_.find(id);
    return found == tasks_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] FoundryPolicy* policy_locked(PolicyId id) {
    const auto found = policies_.find(id);
    return found == policies_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const FoundryPolicy* policy_locked(PolicyId id) const {
    const auto found = policies_.find(id);
    return found == policies_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] BudgetLedger* ledger_locked(PopulationId id) {
    const auto found = ledgers_.find(id);
    return found == ledgers_.end() ? nullptr : &found->second;
  }

  /// Population identities in ascending order: the deterministic order in
  /// which the foundry considers populations for new work.
  [[nodiscard]] std::vector<PopulationId> ordered_populations_locked() const;
  [[nodiscard]] std::vector<CandidateId> ordered_candidates_locked(PopulationId id) const;
  [[nodiscard]] std::vector<EvaluationRecord> evaluations_for_locked(CandidateId id) const;
  [[nodiscard]] std::vector<AttemptRecord> attempts_for_locked(PopulationId id) const;

  /// The one canonical candidate/evidence set a selection decision is derived
  /// from. prepare_selection and commit_selection both build their digest from
  /// this single accessor, so the two phases can never disagree about which
  /// candidates and which evidence records a decision covers.
  struct SelectionCanonicalInputs {
    std::vector<CandidateRecord> candidates;
    std::vector<EvaluationRecord> evaluations;
  };
  [[nodiscard]] SelectionCanonicalInputs selection_canonical_inputs_locked(PopulationId id) const;

  /// The single authority gate every path that can create candidate evidence
  /// passes through. A population that has left the live lifecycle owns no
  /// authority over the work it authorised, so cancelling a population also
  /// ends the authority of candidates that were already published inside it.
  [[nodiscard]] Status require_live_population_authority_locked(const PopulationRecord& population,
                                                                std::string_view action) const;

  // -- transitions (lock held) ---------------------------------------------

  Status set_population_state_locked(PopulationRecord& record, PopulationState next,
                                     std::string detail);
  Status set_candidate_state_locked(CandidateRecord& record, CandidateState next,
                                    std::string reason);
  Status set_attempt_state_locked(AttemptRecord& record, AttemptState next, std::string detail);

  // -- evidence (lock held) -------------------------------------------------

  /// A requirement is satisfied only by a COMPLETE record produced by an
  /// evaluator kind that is allowed to be authoritative. A worker self report
  /// therefore never closes a requirement.
  [[nodiscard]] const EvaluationRecord* requirement_record_locked(const CandidateRecord& candidate,
                                                                  std::string_view evaluator_key)
      const;
  [[nodiscard]] bool requirement_complete_locked(const CandidateRecord& candidate,
                                                 std::string_view evaluator_key) const;
  [[nodiscard]] std::size_t complete_requirement_count_locked(
      const CandidateRecord& candidate) const;

  /// Move a candidate to Evaluated when every declared requirement has a
  /// complete authoritative-capable record, and settle its attempt.
  void settle_candidate_locked(CandidateRecord& candidate);

  // -- dispatch helpers (lock held) ----------------------------------------

  [[nodiscard]] std::size_t active_attempts_for_locked(PopulationId id) const;
  [[nodiscard]] std::uint32_t count_candidates_with_artifacts_locked(PopulationId id) const;

  /// Validate a live worker session and report the concrete reason otherwise.
  /// Counts the rejection in the statistics so that operational staleness is
  /// observable rather than merely rejected.
  Status validate_worker_session_locked(const WorkerSessionAuthority& session);

  /// Validate the full operation authority of a mutating worker message.
  Status validate_worker_operation_locked(const WorkerOperationAuthority& authority);

  /// Release every reservation an attempt still holds.
  void release_attempt_reservations_locked(AttemptRecord& attempt);

  // -- population construction (lock held) ---------------------------------

  /// Create a population record from a validated specification. Assumes the
  /// caller holds the exclusive lock; the public create_population() wrapper
  /// takes the lock and then calls this.
  Result<PopulationId> create_population_locked(const PopulationSpec& spec);

  /// Close a population whose closure contract is already satisfied. Assumes
  /// the exclusive lock is held.
  Status close_population_locked(PopulationId id);

  /// Register the carried-forward elite candidate in a successor population.
  /// The elite inherits lineage and artifacts but never inherits evaluation
  /// evidence: it must satisfy every mandatory gate again.
  Status add_elite_candidate_locked(PopulationRecord* successor, const CandidateRecord& winner);
};

/// Highest identity counter observed in one snapshot, per identity domain.
///
/// This is the single traversal that answers "what is the highest counter this
/// state already contains". The snapshot validator compares it against the
/// declared counters and recovery seeds the allocator from it, so the two can
/// never disagree about how far a numeric domain has already advanced.
[[nodiscard]] std::array<std::uint32_t, kIdKindCount> observed_identity_counters(
    const FoundrySnapshot& snapshot);

/// Canonical encoding helpers shared by the two implementation units.
[[nodiscard]] std::string encode_evaluation_for_digest(const EvaluationRecord& record);
[[nodiscard]] std::string encode_candidate_for_digest(
    const CandidateRecord& candidate, const std::vector<EvaluationRecord>& evaluations);

/// Numeric rendering used inside canonical encodings. Locale independent and
/// exact: the shortest representation that round-trips the double.
[[nodiscard]] std::string canonical_double(double value);

}  // namespace autonomous_foundry
