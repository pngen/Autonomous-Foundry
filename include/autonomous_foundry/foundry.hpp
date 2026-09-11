#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/artifact.hpp"
#include "autonomous_foundry/attempt.hpp"
#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/evaluation.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/lineage.hpp"
#include "autonomous_foundry/policy.hpp"
#include "autonomous_foundry/population.hpp"
#include "autonomous_foundry/promotion.hpp"
#include "autonomous_foundry/retention.hpp"
#include "autonomous_foundry/selection.hpp"
#include "autonomous_foundry/task.hpp"
#include "autonomous_foundry/worker.hpp"

// The foundry state machine.
//
// FoundryCore is a pure, in-process state machine. It performs no I/O, spawns
// no threads, calls no user hooks and never blocks. Every public method either
// takes a shared lock and returns an immutable copy, or takes an exclusive lock
// and applies one validated transition. This is what makes the concurrency
// contract auditable: there is exactly one lock, and it is never held across a
// boundary that could block.

namespace autonomous_foundry {

struct FoundryConfig {
  FoundryId foundry;
  FoundryRunId run;
  CoordinatorEpoch epoch;
  std::uint32_t id_salt{0};

  /// Root below which per-attempt workspaces are created. The core never
  /// touches the filesystem; the coordinator uses this value to build the
  /// package it hands to a worker.
  std::filesystem::path workspace_root{};

  /// Default budgets applied to tasks and policies that do not override them.
  BudgetLimits default_budgets{};
};

struct FoundryStatistics {
  std::uint64_t task_revisions{0};
  std::uint64_t policy_revisions{0};
  std::uint64_t populations_created{0};
  std::uint64_t populations_closed{0};
  std::uint64_t candidate_slots_created{0};
  std::uint64_t candidates_published{0};
  std::uint64_t attempts_authorized{0};
  std::uint64_t attempts_completed{0};
  std::uint64_t attempts_failed{0};
  std::uint64_t attempts_cancelled{0};
  std::uint64_t attempts_outcome_unknown{0};
  std::uint64_t evaluations_recorded{0};
  std::uint64_t selections_prepared{0};
  std::uint64_t selections_committed{0};
  std::uint64_t retentions_committed{0};
  std::uint64_t promotion_requests{0};

  std::uint64_t stale_authority_rejections{0};
  std::uint64_t duplicate_rejections{0};
  std::uint64_t late_rejections{0};
  std::uint64_t illegal_transition_rejections{0};

  friend bool operator==(const FoundryStatistics&, const FoundryStatistics&) noexcept = default;
};

/// A complete immutable copy of durable state.
struct FoundrySnapshot {
  FoundryId foundry;
  FoundryRunId run;
  CoordinatorEpoch epoch;
  std::uint64_t revision{0};

  std::uint32_t id_salt{0};
  std::array<std::uint32_t, kIdKindCount> id_counters{};

  std::unordered_map<TaskId, TaskSpec> tasks;
  std::unordered_map<PolicyId, FoundryPolicy> policies;
  std::unordered_map<PopulationId, PopulationRecord> populations;
  std::unordered_map<CandidateId, CandidateRecord> candidates;
  std::unordered_map<WorkerId, WorkerRecord> workers;
  std::unordered_map<AttemptId, AttemptRecord> attempts;
  std::unordered_map<AssignmentId, AssignmentRecord> assignments;
  std::unordered_map<EvaluationId, EvaluationRecord> evaluations;
  /// Only committed or prepared decisions are kept; the latest per population.
  std::unordered_map<PopulationId, SelectionDecision> selections;
  std::unordered_map<PopulationId, RetentionDecision> retentions;
  std::unordered_map<PromotionRequestId, PromotionRequest> promotion_requests;
  std::unordered_map<PopulationId, BudgetLedger> ledgers;

  LineageGraph lineage;
  FoundryStatistics statistics;
};

struct WorkerRegistrationRequest {
  WorkerId worker;
  WorkerBootId boot;
  std::string label;
  std::string capability;
  std::uint32_t process_id{0};
};

/// An attempt that exists durably but has not yet been handed to a worker.
struct PendingDispatch {
  WorkerSessionAuthority session;
  AttemptRecord attempt;
  AssignmentRecord assignment;
};

/// Result of a worker publication, returned to the coordinator so that it can
/// schedule evaluation and persist before acknowledging.
struct PublicationOutcome {
  CandidateId candidate;
  CandidateGeneration candidate_generation;
  EvaluationGeneration evidence_generation;
  bool replaced_previous_output{false};
};

class AUTONOMOUS_FOUNDRY_API FoundryCore {
 public:
  explicit FoundryCore(FoundryConfig config);
  ~FoundryCore();

  FoundryCore(const FoundryCore&) = delete;
  FoundryCore& operator=(const FoundryCore&) = delete;

  // -- identity and lifecycle ------------------------------------------------

  [[nodiscard]] FoundryId foundry() const;
  [[nodiscard]] FoundryRunId run() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] std::uint64_t revision() const;
  [[nodiscard]] const FoundryConfig& config() const noexcept;

  /// Adopt a recovered snapshot and advance the coordinator epoch. Applied by
  /// the coordinator at startup before any worker may connect.
  Status recover(FoundrySnapshot snapshot);

  // -- read paths (shared lock, immutable copies) ---------------------------

  [[nodiscard]] Result<TaskSpec> task(TaskId id) const;
  [[nodiscard]] Result<FoundryPolicy> policy(PolicyId id) const;
  [[nodiscard]] Result<PopulationRecord> population(PopulationId id) const;
  [[nodiscard]] Result<CandidateRecord> candidate(CandidateId id) const;
  [[nodiscard]] Result<WorkerRecord> worker(WorkerId id) const;
  [[nodiscard]] Result<AttemptRecord> attempt(AttemptId id) const;
  [[nodiscard]] Result<AssignmentRecord> assignment(AssignmentId id) const;
  [[nodiscard]] Result<EvaluationRecord> evaluation(EvaluationId id) const;
  [[nodiscard]] Result<SelectionDecision> selection(PopulationId id) const;
  [[nodiscard]] Result<RetentionDecision> retention(PopulationId id) const;
  [[nodiscard]] Result<PromotionRequest> promotion_request(PromotionRequestId id) const;

  [[nodiscard]] std::vector<PopulationId> population_ids() const;
  [[nodiscard]] std::vector<CandidateId> population_candidates(PopulationId id) const;
  [[nodiscard]] std::vector<EvaluationRecord> candidate_evaluations(CandidateId id) const;
  [[nodiscard]] std::vector<WorkerRecord> workers() const;
  [[nodiscard]] std::vector<AttemptRecord> attempts() const;
  [[nodiscard]] std::vector<LineageNode> lineage_nodes() const;
  [[nodiscard]] std::vector<PromotionRequestId> promotion_request_ids() const;

  [[nodiscard]] FoundryStatistics statistics() const;
  [[nodiscard]] FoundrySnapshot snapshot() const;

  /// Concrete reasons the population may not close. Empty means it may.
  [[nodiscard]] std::vector<std::string> closure_blockers(PopulationId id) const;

  /// Full self-consistency audit of current state. Returns one string per
  /// violation; empty means the state is consistent.
  [[nodiscard]] std::vector<std::string> audit() const;

  // -- controller operations -------------------------------------------------

  [[nodiscard]] Result<TaskId> define_task(TaskSpec spec);
  [[nodiscard]] Result<TaskGeneration> revise_task(TaskId id, TaskSpec replacement);
  [[nodiscard]] Result<PolicyId> define_policy(FoundryPolicy policy);
  [[nodiscard]] Result<PolicyGeneration> revise_policy(PolicyId id, FoundryPolicy replacement);
  [[nodiscard]] Result<PopulationId> create_population(const PopulationSpec& spec);
  Status start_population(PopulationId id, PopulationGeneration generation);
  Status cancel_population(PopulationId id, PopulationGeneration generation, std::string reason);
  Status request_population_revalidation(PopulationId id, std::string reason);
  Status revalidate_population(PopulationId id, PopulationGeneration generation,
                               std::string detail);

  [[nodiscard]] Result<SelectionDecision> prepare_selection(PopulationId id);
  [[nodiscard]] Result<SelectionDecision> commit_selection(const SelectionDecision& prepared);
  [[nodiscard]] Result<RetentionDecision> prepare_retention(PopulationId id);
  [[nodiscard]] Result<RetentionDecision> commit_retention(const RetentionDecision& prepared);

  [[nodiscard]] Result<PromotionRequest> prepare_promotion_request(PopulationId id);
  Status record_promotion_receipt(PromotionRequestId id, PromotionReceipt receipt);

  Status close_population(PopulationId id);
  [[nodiscard]] Result<PopulationId> advance_population(PopulationId id, std::string next_name);

  Status cancel_attempt(AttemptId id, std::string reason);
  Status force_attempt_outcome_unknown(AttemptId id, std::string reason);

  // -- worker session operations --------------------------------------------

  [[nodiscard]] Result<WorkerSessionAuthority> register_worker(
      const WorkerRegistrationRequest& request);
  Status worker_ready(const WorkerSessionAuthority& session, std::string capability);

  /// Complete revalidation of a reconnecting incarnation. Issues a fresh
  /// session and session generation so that no authority issued before the
  /// coordinator restart can ever be replayed.
  [[nodiscard]] Result<WorkerSessionAuthority> revalidate_worker(
      const WorkerSessionAuthority& session, std::string detail);

  Status worker_disconnected(const WorkerSessionAuthority& session, std::string reason);
  Status reject_worker_session(const WorkerSessionAuthority& session, std::string reason);

  // -- dispatch --------------------------------------------------------------

  /// Create an attempt for the next eligible slot and reserve its budgets.
  /// The attempt is durable-but-unsent: the coordinator must persist, then
  /// call confirm_dispatch, and only then send the package.
  [[nodiscard]] Result<PendingDispatch> authorize_attempt(const WorkerSessionAuthority& session);
  [[nodiscard]] Result<AttemptPackage> confirm_dispatch(const WorkerSessionAuthority& session,
                                                       AttemptId attempt,
                                                       AttemptGeneration generation);
  Status abandon_dispatch(const WorkerSessionAuthority& session, AttemptId attempt,
                          std::string reason);

  Status acknowledge_attempt(const WorkerOperationAuthority& authority);
  [[nodiscard]] Result<PublicationOutcome> publish_candidate(
      const WorkerOperationAuthority& authority, std::vector<ArtifactRef> artifacts,
      std::string declared_strategy);
  Status report_attempt_failure(const WorkerOperationAuthority& authority, std::string reason);

  // -- evaluation ------------------------------------------------------------

  /// Record an evaluator result. Rejects stale, duplicate and late records.
  Status record_evaluation(EvaluationRecord record);

  /// Ask the core which evaluations a candidate still needs. The coordinator
  /// then runs them outside the lock.
  [[nodiscard]] Result<std::vector<EvaluationRequirement>> pending_evaluations(
      CandidateId id) const;

  /// Candidates whose declared requirements are not yet fully covered by
  /// complete authoritative-capable records, in deterministic order.
  [[nodiscard]] std::vector<CandidateId> candidates_awaiting_evaluation() const;

  /// Mark a published candidate as under evaluation. Separate from
  /// pending_evaluations so that a read path never mutates state.
  Status begin_evaluation(CandidateId id_);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// SHA-256 over the canonical encoding of every input a selection decision
/// uses. Exposed so that callers can independently reproduce the digest.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string compute_selection_state_digest(
    const PopulationRecord& population, const TaskSpec& task, const FoundryPolicy& policy,
    const std::vector<CandidateRecord>& candidates,
    const std::vector<EvaluationRecord>& evaluations);

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string compute_retention_state_digest(
    const PopulationRecord& population, const FoundryPolicy& policy,
    const SelectionDecision& selection);

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string compute_candidate_state_digest(
    const CandidateRecord& candidate, const std::vector<EvaluationRecord>& evaluations);

}  // namespace autonomous_foundry
