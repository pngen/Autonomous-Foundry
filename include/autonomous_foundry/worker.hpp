#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "autonomous_foundry/authority.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"
#include "autonomous_foundry/id.hpp"
#include "autonomous_foundry/limits.hpp"

// Worker incarnation state.
//
// A worker is addressed by WorkerId plus WorkerBootId. WorkerId is durable and
// stable: it is the operator's name for "the thing that runs work". WorkerBootId
// identifies one process incarnation of that worker. Every authorization is
// bound to both, so a restarted worker can never replay the authority of the
// incarnation it replaced.

namespace autonomous_foundry {

enum class WorkerState : std::uint8_t {
  /// A connection exists but no registration has been accepted.
  Connecting = 0,
  /// Registered under the current coordinator epoch, not yet ready.
  Registered = 1,
  /// Ready to accept assignments.
  Ready = 2,
  /// Holding an open assignment.
  Busy = 3,
  /// Reconnected after a coordinator restart or an epoch change; must
  /// revalidate before it may accept new work.
  RevalidationRequired = 4,
  /// Finishing an explicitly permitted boundary; no new work.
  Draining = 5,
  /// Not connected.
  Offline = 6,
  /// Rejected: stale boot, duplicate incarnation, or protocol violation.
  Rejected = 7,
};

inline constexpr std::size_t kWorkerStateCount = 8;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view worker_state_name(
    WorkerState state) noexcept;

/// Reference worker strategies. These names are part of the reference
/// deployment contract, not of the core runtime: any worker that speaks the
/// protocol may register with an arbitrary strategy label.
enum class ReferenceStrategy : std::uint8_t {
  ClosedForm = 0,
  Iterative = 1,
  OffByOne = 2,
  SelfReportedPass = 3,
};

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view reference_strategy_name(
    ReferenceStrategy strategy) noexcept;
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<ReferenceStrategy> parse_reference_strategy(
    std::string_view text);

struct WorkerRecord {
  WorkerId id;
  WorkerBootId boot;
  std::string label;

  /// Operating system process identifier of the incarnation. Diagnostic only:
  /// identity never depends on a pid, which the operating system may reuse.
  std::uint32_t process_id{0};

  WorkerState state{WorkerState::Offline};
  CoordinatorEpoch registered_epoch;
  CoordinatorEpoch state_epoch;

  SessionId session;
  WorkerSessionGeneration session_generation;

  /// Free-form capability label reported by the worker, bounded and validated.
  std::string capability;

  std::uint64_t assignments_accepted{0};
  std::uint64_t assignments_completed{0};
  std::uint64_t assignments_failed{0};
  std::uint64_t stale_messages_rejected{0};

  AttemptId active_attempt;
  AttemptGeneration active_attempt_generation;
  AssignmentId active_assignment;

  std::string last_diagnostic;

  friend bool operator==(const WorkerRecord&, const WorkerRecord&) noexcept = default;
};

/// Validate a worker-authored label or capability string: bounded length,
/// printable ASCII only, no control characters.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Status validate_worker_text(std::string_view text,
                                                                 std::string_view field);

}  // namespace autonomous_foundry
