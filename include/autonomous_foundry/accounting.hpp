#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/limits.hpp"

// Foundry level accounting.
//
// Autonomous Foundry constrains only what it can actually measure: candidate
// attempts, worker assignments, evaluation attempts, retained candidates and
// retries. It does not fabricate wall-clock, token, dollar or accelerator
// budgets, because the standalone runtime cannot measure them. A reservation
// is always resolved exactly once into either consumption or release, and the
// ledger refuses to close while a reservation is still open.

namespace autonomous_foundry {

enum class BudgetKind : std::uint8_t {
  CandidateAttempts = 0,
  WorkerAssignments = 1,
  EvaluationAttempts = 2,
  RetainedCandidates = 3,
  Retries = 4,
  Candidates = 5,
  Workers = 6,
  ActiveAttempts = 7,
  EvaluationConcurrency = 8,
};

inline constexpr std::size_t kBudgetKindCount = 9;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view budget_kind_name(BudgetKind kind) noexcept;

struct BudgetLimits {
  std::uint32_t max_candidate_attempts{4096};
  std::uint32_t max_worker_assignments{4096};
  std::uint32_t max_evaluation_attempts{8192};
  std::uint32_t max_retained_candidates{64};
  std::uint32_t max_retries{8};
  std::uint32_t max_candidates{1024};
  std::uint32_t max_workers{64};
  std::uint32_t max_active_attempts{64};
  std::uint32_t max_evaluation_concurrency{8};

  [[nodiscard]] std::uint32_t limit_for(BudgetKind kind) const noexcept;
  [[nodiscard]] Status validate() const;
  friend bool operator==(const BudgetLimits&, const BudgetLimits&) noexcept = default;
};

/// Per-kind reservation/consumption/release accounting.
class AUTONOMOUS_FOUNDRY_API BudgetLedger {
 public:
  struct Counter {
    std::uint64_t reserved{0};
    std::uint64_t consumed{0};
    std::uint64_t released{0};

    /// Capacity used against the configured limit. Released capacity is
    /// returned to the pool, so it no longer counts against the limit.
    [[nodiscard]] std::uint64_t in_use() const noexcept { return reserved + consumed; }
    friend bool operator==(const Counter&, const Counter&) noexcept = default;
  };

  BudgetLedger() = default;
  explicit BudgetLedger(BudgetLimits limits) : limits_(limits) {}

  [[nodiscard]] const BudgetLimits& limits() const noexcept { return limits_; }
  void set_limits(BudgetLimits limits) noexcept { limits_ = limits; }

  /// Reserve one unit. Fails with BudgetExhausted when the limit is reached.
  Status reserve(BudgetKind kind);

  /// Convert an open reservation into consumption. Fails when nothing is open.
  Status consume(BudgetKind kind);

  /// Give an open reservation back without consuming it.
  Status release(BudgetKind kind);

  /// Consume directly without a prior reservation. Used for counters that are
  /// incremented at a point where no cancellation window exists.
  Status consume_direct(BudgetKind kind);

  [[nodiscard]] const Counter& counter(BudgetKind kind) const noexcept {
    return counters_[static_cast<std::size_t>(kind)];
  }

  [[nodiscard]] std::uint64_t in_use(BudgetKind kind) const noexcept {
    return counters_[static_cast<std::size_t>(kind)].in_use();
  }

  [[nodiscard]] bool has_capacity(BudgetKind kind) const noexcept {
    return in_use(kind) < limits_.limit_for(kind);
  }

  [[nodiscard]] std::uint32_t remaining(BudgetKind kind) const noexcept {
    const std::uint64_t used = in_use(kind);
    const std::uint32_t limit = limits_.limit_for(kind);
    return used >= limit ? 0u : static_cast<std::uint32_t>(limit - used);
  }

  /// Number of reservations still open across every counter.
  [[nodiscard]] std::uint64_t open_reservations() const noexcept;

  /// Verify that no reservation is left open and that every counter is
  /// internally consistent. Called at population closure and at shutdown.
  [[nodiscard]] Status check_balanced() const;

  [[nodiscard]] const std::array<Counter, kBudgetKindCount>& counters() const noexcept {
    return counters_;
  }

  void restore(const std::array<Counter, kBudgetKindCount>& counters) noexcept {
    counters_ = counters;
  }

 private:
  BudgetLimits limits_{};
  std::array<Counter, kBudgetKindCount> counters_{};
};

}  // namespace autonomous_foundry
