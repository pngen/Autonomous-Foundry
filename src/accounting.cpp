#include "autonomous_foundry/accounting.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace autonomous_foundry {

namespace {

std::string exhaustion_message(BudgetKind kind, std::uint32_t limit, std::uint64_t in_use) {
  return std::string("budget '") + std::string(budget_kind_name(kind)) +
         "' has no capacity left: limit " + std::to_string(limit) + ", in use " +
         std::to_string(in_use);
}

std::string no_open_reservation_message(BudgetKind kind, std::string_view action) {
  return std::string("budget '") + std::string(budget_kind_name(kind)) + "' cannot be " +
         std::string(action) + " because no reservation is open";
}

}  // namespace

std::string_view budget_kind_name(BudgetKind kind) noexcept {
  switch (kind) {
    case BudgetKind::CandidateAttempts:
      return "CandidateAttempts";
    case BudgetKind::WorkerAssignments:
      return "WorkerAssignments";
    case BudgetKind::EvaluationAttempts:
      return "EvaluationAttempts";
    case BudgetKind::RetainedCandidates:
      return "RetainedCandidates";
    case BudgetKind::Retries:
      return "Retries";
    case BudgetKind::Candidates:
      return "Candidates";
    case BudgetKind::Workers:
      return "Workers";
    case BudgetKind::ActiveAttempts:
      return "ActiveAttempts";
    case BudgetKind::EvaluationConcurrency:
      return "EvaluationConcurrency";
    default:
      return "BudgetKindUnknown";
  }
}

std::uint32_t BudgetLimits::limit_for(BudgetKind kind) const noexcept {
  switch (kind) {
    case BudgetKind::CandidateAttempts:
      return max_candidate_attempts;
    case BudgetKind::WorkerAssignments:
      return max_worker_assignments;
    case BudgetKind::EvaluationAttempts:
      return max_evaluation_attempts;
    case BudgetKind::RetainedCandidates:
      return max_retained_candidates;
    case BudgetKind::Retries:
      return max_retries;
    case BudgetKind::Candidates:
      return max_candidates;
    case BudgetKind::Workers:
      return max_workers;
    case BudgetKind::ActiveAttempts:
      return max_active_attempts;
    case BudgetKind::EvaluationConcurrency:
      return max_evaluation_concurrency;
    default:
      return 0;
  }
}

Status BudgetLimits::validate() const {
  if (max_candidates == 0) {
    return Status(ErrorCode::InvalidArgument,
                  std::string_view("budget limit 'max_candidates' must be greater than zero"));
  }
  if (max_workers == 0) {
    return Status(ErrorCode::InvalidArgument,
                  std::string_view("budget limit 'max_workers' must be greater than zero"));
  }
  if (max_active_attempts == 0) {
    return Status(ErrorCode::InvalidArgument,
                  std::string_view("budget limit 'max_active_attempts' must be greater than zero"));
  }
  if (max_evaluation_concurrency == 0) {
    return Status(
        ErrorCode::InvalidArgument,
        std::string_view("budget limit 'max_evaluation_concurrency' must be greater than zero"));
  }
  return Status();
}

Status BudgetLedger::reserve(BudgetKind kind) {
  Counter& entry = counters_[static_cast<std::size_t>(kind)];
  const std::uint32_t limit = limits_.limit_for(kind);
  if (entry.in_use() >= limit) {
    return Status(ErrorCode::BudgetExhausted, exhaustion_message(kind, limit, entry.in_use()));
  }
  entry.reserved += 1;
  return Status();
}

Status BudgetLedger::consume(BudgetKind kind) {
  Counter& entry = counters_[static_cast<std::size_t>(kind)];
  if (entry.reserved == 0) {
    return Status(ErrorCode::IllegalStateTransition,
                  no_open_reservation_message(kind, std::string_view("consumed")));
  }
  entry.reserved -= 1;
  entry.consumed += 1;
  return Status();
}

Status BudgetLedger::release(BudgetKind kind) {
  Counter& entry = counters_[static_cast<std::size_t>(kind)];
  if (entry.reserved == 0) {
    return Status(ErrorCode::IllegalStateTransition,
                  no_open_reservation_message(kind, std::string_view("released")));
  }
  entry.reserved -= 1;
  entry.released += 1;
  return Status();
}

Status BudgetLedger::consume_direct(BudgetKind kind) {
  Counter& entry = counters_[static_cast<std::size_t>(kind)];
  const std::uint32_t limit = limits_.limit_for(kind);
  if (entry.in_use() >= limit) {
    return Status(ErrorCode::BudgetExhausted, exhaustion_message(kind, limit, entry.in_use()));
  }
  entry.consumed += 1;
  return Status();
}

std::uint64_t BudgetLedger::open_reservations() const noexcept {
  std::uint64_t total = 0;
  for (const Counter& entry : counters_) {
    total += entry.reserved;
  }
  return total;
}

Status BudgetLedger::check_balanced() const {
  std::string open_kinds;
  for (std::size_t index = 0; index < counters_.size(); ++index) {
    const Counter& entry = counters_[index];
    if (entry.reserved == 0) {
      continue;
    }
    if (!open_kinds.empty()) {
      open_kinds.append(", ");
    }
    open_kinds.append(budget_kind_name(static_cast<BudgetKind>(index)));
    open_kinds.push_back('=');
    open_kinds.append(std::to_string(entry.reserved));
  }
  if (!open_kinds.empty()) {
    return Status(ErrorCode::AccountingImbalance,
                  std::string("budget ledger is not balanced: open reservations for ") + open_kinds);
  }
  return Status();
}

}  // namespace autonomous_foundry
