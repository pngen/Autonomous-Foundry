#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "autonomous_foundry/export.hpp"

// Typed failure surface.
//
// Autonomous Foundry never collapses distinct failures into a generic false or
// an exception string. Every operation returns either a value or a Status that
// carries a stable machine-readable code plus a human-readable diagnostic.

namespace autonomous_foundry {

enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // identity
  NullIdentity = 100,
  CrossDomainIdentity = 101,
  DuplicateIdentity = 102,
  UnknownIdentity = 103,
  IdentityExhausted = 104,

  // arguments and encoding
  InvalidArgument = 200,
  MalformedEncoding = 201,
  LengthOutOfRange = 202,
  IntegerOverflow = 203,
  InvalidUnicode = 204,

  // lifecycle
  IllegalStateTransition = 300,
  LifecycleClosed = 301,
  NotReady = 302,
  AlreadyExists = 303,

  // authority and generations
  StaleCoordinatorEpoch = 400,
  StaleWorkerBoot = 401,
  StaleSession = 402,
  StaleAuthority = 403,
  StalePopulationGeneration = 404,
  StaleTaskGeneration = 405,
  StaleCandidateGeneration = 406,
  StaleEvaluationGeneration = 407,
  StalePolicyGeneration = 408,
  StaleAttempt = 409,
  StaleAssignment = 410,
  StaleSelection = 411,
  SupersededCandidate = 412,
  RetiredLineage = 413,
  CancelledAttempt = 414,
  DuplicateResult = 415,
  LateCompletion = 416,
  UnknownWorkerIncarnation = 417,
  AuthorityWithdrawn = 418,
  RevalidationRequired = 419,
  GenerationRegression = 420,

  // lineage
  LineageCycle = 500,
  LineageSelfParent = 501,
  LineageOrphan = 502,
  LineageReparentForbidden = 503,

  // evaluation, selection, retention
  EvaluationIncomplete = 600,
  HardRequirementFailed = 601,
  MandatoryEvidenceMissing = 602,
  SelectionImpossible = 603,
  PolicyConflict = 604,
  RankingFactorUnknown = 605,
  RetentionLimitExceeded = 606,
  PromotionNotEligible = 607,

  // budgets and resources
  BudgetExhausted = 700,
  ResourceExhausted = 701,
  QueueCapacityExceeded = 702,
  OutputLimitExceeded = 703,
  AccountingImbalance = 704,

  // persistence
  PersistenceIoFailure = 800,
  PersistenceCorrupt = 801,
  PersistenceVersionUnsupported = 802,
  PersistenceTruncated = 803,
  PersistenceIntegrityMismatch = 804,
  PersistenceTrailingGarbage = 805,
  PersistenceRejectedContent = 806,

  // transport and protocol
  TransportFailure = 900,
  ProtocolViolation = 901,
  FrameTooLarge = 902,
  FrameCorrupt = 903,
  FrameTruncated = 904,
  ConnectionClosed = 905,
  NotConnected = 906,

  // filesystem and workspaces
  PathEscape = 1000,
  UnsafePath = 1001,
  ReparsePointRejected = 1002,
  WorkspaceFailure = 1003,
  TransactionAborted = 1004,
  IoFailure = 1005,

  // process and lifecycle
  ProcessSpawnFailure = 1100,
  ProcessWaitFailure = 1101,
  OutcomeUnknown = 1102,
  CoordinatorFailure = 1103,
  ShutdownInProgress = 1104,
  Cancelled = 1105,
  AmbiguousCompletion = 1106,

  // capability
  Unsupported = 1200,
  Unavailable = 1201,

  Internal = 1300,
};

/// Stable textual name of an error code. Used by the CLI, the protocol and the
/// persistence layer; the strings are part of the observable contract.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view error_code_name(ErrorCode code) noexcept;

/// True when the code denotes a stale-authority rejection. Exposed so that
/// callers can implement deterministic retry/revalidate behaviour without
/// matching on individual codes.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool is_stale_authority(ErrorCode code) noexcept;

/// True when the code denotes a persistence integrity/format rejection.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API bool is_persistence_rejection(ErrorCode code) noexcept;

class AUTONOMOUS_FOUNDRY_API Status {
 public:
  Status() noexcept = default;

  // A single string_view overload on purpose. Two overloads (std::string and
  // std::string_view) make every string literal ambiguous, because a literal
  // converts equally well to both.
  Status(ErrorCode code, std::string_view message);

  // A default-constructed Status is success. There is deliberately no static
  // Status::ok() factory. A static and a non-static member function with the
  // same name and the same parameter-type-list cannot coexist
  // ([basic.scope.scope]/3); MSVC reports C2686 for such a pair regardless of
  // any attribute, and GCC and Clang accept it, so the conflict is easy to
  // miss. The predicate member is the spelling every call site depends on.
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  [[nodiscard]] std::string to_string() const;

  /// Prefix the diagnostic with additional context while preserving the code.
  [[nodiscard]] Status with_context(std::string_view context) const;

 private:
  ErrorCode code_{ErrorCode::Ok};
  std::string message_;
};

/// Result of an operation that produces a value.
template <typename T>
class Result {
 public:
  using value_type = T;

  Result() = default;
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  [[nodiscard]] T& value() & noexcept { return value_; }
  [[nodiscard]] const T& value() const& noexcept { return value_; }
  [[nodiscard]] T&& value() && noexcept { return std::move(value_); }

  [[nodiscard]] T* operator->() noexcept { return &value_; }
  [[nodiscard]] const T* operator->() const noexcept { return &value_; }
  [[nodiscard]] T& operator*() noexcept { return value_; }
  [[nodiscard]] const T& operator*() const noexcept { return value_; }

 private:
  T value_{};
  Status status_{};
};

/// Bind a Status reference to either a Status or a Result without copying
/// through a dangling temporary.
[[nodiscard]] inline const Status& to_status(const Status& status) noexcept { return status; }

template <typename T>
[[nodiscard]] const Status& to_status(const Result<T>& result) noexcept {
  return result.status();
}

}  // namespace autonomous_foundry

// Propagate the first failure out of the enclosing function. The macro is a
// single statement and introduces its own scope, so it can be used repeatedly
// inside one block without shadowing diagnostics.
#define AF_TRY(expr)                                                              \
  do {                                                                            \
    const ::autonomous_foundry::Status af_status_probe__ =                        \
        ::autonomous_foundry::to_status(expr);                                    \
    if (!af_status_probe__.ok()) {                                                \
      return af_status_probe__;                                                   \
    }                                                                             \
  } while (false)

// Assign the value of a Result to an existing variable, propagating failure.
#define AF_TRY_ASSIGN(target, expr)                                               \
  do {                                                                            \
    auto af_result_probe__ = (expr);                                              \
    if (!af_result_probe__.ok()) {                                                \
      return af_result_probe__.status();                                          \
    }                                                                             \
    target = std::move(af_result_probe__).value();                                \
  } while (false)
