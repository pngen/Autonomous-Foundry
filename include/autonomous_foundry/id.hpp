#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/export.hpp"

// Strongly typed identities.
//
// Every identity in Autonomous Foundry is a distinct C++ type. Two identities
// from different domains cannot be compared, assigned or passed to each other
// even though both are 64-bit values. The encoding also carries the identity
// domain in its high byte, so an identity that is smuggled through an untyped
// channel (persisted bytes, a wire frame, a user-supplied string) is still
// rejected instead of being silently accepted.
//
// Raw layout (64 bits):
//
//   bits 56..63 : IdKind      identity domain tag
//   bits 32..55 : salt        24-bit per-foundry-run salt
//   bits  0..31 : counter     monotonic allocation counter, never zero
//
// Raw value 0 is the null identity for every domain and is never allocated.

namespace autonomous_foundry {

enum class IdKind : std::uint8_t {
  None = 0,
  Foundry = 1,
  FoundryRun = 2,
  Population = 3,
  Task = 4,
  Candidate = 5,
  Lineage = 6,
  Worker = 7,
  WorkerBoot = 8,
  Attempt = 9,
  Assignment = 10,
  Evaluation = 11,
  Evaluator = 12,
  Policy = 13,
  Artifact = 14,
  PromotionRequest = 15,
  Session = 16,
  Controller = 17,
};

/// Number of distinct identity domains; used to size allocation tables.
inline constexpr std::size_t kIdKindCount = 18;

[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::string_view id_kind_name(IdKind kind) noexcept;

namespace detail {

inline constexpr std::uint64_t kIdCounterMask = 0xFFFFFFFFull;
inline constexpr std::uint64_t kIdSaltMask = 0xFFFFFFull;
inline constexpr std::uint64_t kIdKindMask = 0xFFull;
inline constexpr unsigned kIdCounterShift = 0;
inline constexpr unsigned kIdSaltShift = 32;
inline constexpr unsigned kIdKindShift = 56;

}  // namespace detail

/// A 64-bit identity tagged with a compile-time domain.
template <typename Tag>
class StrongId {
 public:
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;

  /// Construct from a raw 64-bit value. Domain membership is not validated
  /// here; use strong_id_from_raw_checked when the value came from an
  /// untrusted source such as a snapshot or a network frame.
  [[nodiscard]] static constexpr StrongId from_raw(std::uint64_t raw) noexcept {
    StrongId id;
    id.raw_ = raw;
    return id;
  }

  [[nodiscard]] static constexpr StrongId null() noexcept { return StrongId(); }

  [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return raw_ == 0; }
  [[nodiscard]] constexpr bool valid() const noexcept { return raw_ != 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return raw_ != 0; }

  [[nodiscard]] constexpr std::uint32_t counter() const noexcept {
    return static_cast<std::uint32_t>((raw_ >> detail::kIdCounterShift) & detail::kIdCounterMask);
  }
  [[nodiscard]] constexpr std::uint32_t salt() const noexcept {
    return static_cast<std::uint32_t>((raw_ >> detail::kIdSaltShift) & detail::kIdSaltMask);
  }
  [[nodiscard]] constexpr IdKind kind() const noexcept {
    return static_cast<IdKind>((raw_ >> detail::kIdKindShift) & detail::kIdKindMask);
  }

  /// Human readable form, for example "CandidateId:1234". Diagnostics only:
  /// parsing this text back into an identity is never how authority is
  /// established. The domain is spelled the way the identity type spells it; a
  /// value whose tag byte names a different domain is rendered with the domain
  /// it actually claims rather than the one that was requested.
  [[nodiscard]] std::string to_string() const {
    std::string text(kind() == Tag::kind ? std::string_view(Tag::name)
                                         : id_kind_name(kind()));
    text.push_back(':');
    text.append(std::to_string(raw_));
    return text;
  }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.raw_ == b.raw_; }
  friend constexpr std::strong_ordering operator<=>(StrongId a, StrongId b) noexcept {
    return a.raw_ <=> b.raw_;
  }

 private:
  std::uint64_t raw_{0};
};

/// Validate an untrusted raw value against the expected domain.
///
/// Raw value 0 is the null identity. So is any value whose domain tag byte is
/// IdKind::None: the allocator never mints that tag, so such a value was never
/// issued by any domain and cannot be a member of one. A value that carries
/// some other, real domain tag is a cross-domain identity and is refused as one
/// rather than being silently reinterpreted.
template <typename Tag>
[[nodiscard]] inline Result<StrongId<Tag>> strong_id_from_raw_checked(std::uint64_t raw,
                                                                     std::string_view field) {
  if (raw == 0) {
    return Status(ErrorCode::NullIdentity,
                  "identity field '" + std::string(field) + "' is the null identity");
  }
  StrongId<Tag> id = StrongId<Tag>::from_raw(raw);
  if (id.kind() == IdKind::None) {
    return Status(ErrorCode::NullIdentity,
                  "identity field '" + std::string(field) +
                      "' carries the None domain tag, so it was never minted by any domain and "
                      "is the null identity");
  }
  if (id.kind() != Tag::kind) {
    return Status(ErrorCode::CrossDomainIdentity,
                  "identity field '" + std::string(field) + "' carries domain '" +
                      std::string(id_kind_name(id.kind())) + "' but '" +
                      std::string(id_kind_name(Tag::kind)) + "' was required");
  }
  return id;
}

struct FoundryIdTag {
  static constexpr IdKind kind = IdKind::Foundry;
  static constexpr std::string_view name = "FoundryId";
};
struct FoundryRunIdTag {
  static constexpr IdKind kind = IdKind::FoundryRun;
  static constexpr std::string_view name = "FoundryRunId";
};
struct PopulationIdTag {
  static constexpr IdKind kind = IdKind::Population;
  static constexpr std::string_view name = "PopulationId";
};
struct TaskIdTag {
  static constexpr IdKind kind = IdKind::Task;
  static constexpr std::string_view name = "TaskId";
};
struct CandidateIdTag {
  static constexpr IdKind kind = IdKind::Candidate;
  static constexpr std::string_view name = "CandidateId";
};
struct LineageIdTag {
  static constexpr IdKind kind = IdKind::Lineage;
  static constexpr std::string_view name = "LineageId";
};
struct WorkerIdTag {
  static constexpr IdKind kind = IdKind::Worker;
  static constexpr std::string_view name = "WorkerId";
};
struct WorkerBootIdTag {
  static constexpr IdKind kind = IdKind::WorkerBoot;
  static constexpr std::string_view name = "WorkerBootId";
};
struct AttemptIdTag {
  static constexpr IdKind kind = IdKind::Attempt;
  static constexpr std::string_view name = "AttemptId";
};
struct AssignmentIdTag {
  static constexpr IdKind kind = IdKind::Assignment;
  static constexpr std::string_view name = "AssignmentId";
};
struct EvaluationIdTag {
  static constexpr IdKind kind = IdKind::Evaluation;
  static constexpr std::string_view name = "EvaluationId";
};
struct EvaluatorIdTag {
  static constexpr IdKind kind = IdKind::Evaluator;
  static constexpr std::string_view name = "EvaluatorId";
};
struct PolicyIdTag {
  static constexpr IdKind kind = IdKind::Policy;
  static constexpr std::string_view name = "PolicyId";
};
struct ArtifactIdTag {
  static constexpr IdKind kind = IdKind::Artifact;
  static constexpr std::string_view name = "ArtifactId";
};
struct PromotionRequestIdTag {
  static constexpr IdKind kind = IdKind::PromotionRequest;
  static constexpr std::string_view name = "PromotionRequestId";
};
struct SessionIdTag {
  static constexpr IdKind kind = IdKind::Session;
  static constexpr std::string_view name = "SessionId";
};
struct ControllerIdTag {
  static constexpr IdKind kind = IdKind::Controller;
  static constexpr std::string_view name = "ControllerId";
};

using FoundryId = StrongId<FoundryIdTag>;
using FoundryRunId = StrongId<FoundryRunIdTag>;
using PopulationId = StrongId<PopulationIdTag>;
using TaskId = StrongId<TaskIdTag>;
using CandidateId = StrongId<CandidateIdTag>;
using LineageId = StrongId<LineageIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using AssignmentId = StrongId<AssignmentIdTag>;
using EvaluationId = StrongId<EvaluationIdTag>;
using EvaluatorId = StrongId<EvaluatorIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using ArtifactId = StrongId<ArtifactIdTag>;
using PromotionRequestId = StrongId<PromotionRequestIdTag>;
using SessionId = StrongId<SessionIdTag>;
using ControllerId = StrongId<ControllerIdTag>;

/// True when the two identity types belong to different domains. Used by
/// tests and by adapters that accept opaque identity strings.
template <typename A, typename B>
[[nodiscard]] constexpr bool identities_are_cross_domain() noexcept {
  return A::tag_type::kind != B::tag_type::kind;
}

/// A monotonic generation counter scoped to one identity domain.
///
/// Generation 0 is the invalid/none generation. The first generation of a
/// durable object is 1. Generations never regress and never wrap: next()
/// reports failure at the numeric limit instead of silently rolling over.
template <typename Tag>
class Generation {
 public:
  using id_type = StrongId<Tag>;

  constexpr Generation() noexcept = default;

  [[nodiscard]] static constexpr Generation first() noexcept {
    Generation g;
    g.value_ = 1;
    return g;
  }

  [[nodiscard]] static constexpr Generation from_value(std::uint32_t value) noexcept {
    Generation g;
    g.value_ = value;
    return g;
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != 0; }

  [[nodiscard]] constexpr std::optional<Generation> next() const noexcept {
    if (value_ == 0xFFFFFFFFu) {
      return std::nullopt;
    }
    Generation g;
    g.value_ = value_ + 1;
    return g;
  }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  friend constexpr bool operator==(Generation a, Generation b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(Generation a, Generation b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::uint32_t value_{0};
};

using PopulationGeneration = Generation<PopulationIdTag>;
using TaskGeneration = Generation<TaskIdTag>;
using CandidateGeneration = Generation<CandidateIdTag>;
using PolicyGeneration = Generation<PolicyIdTag>;
using EvaluationGeneration = Generation<EvaluationIdTag>;
using AttemptGeneration = Generation<AttemptIdTag>;
using AssignmentGeneration = Generation<AssignmentIdTag>;
using WorkerSessionGeneration = Generation<SessionIdTag>;
using SelectionGeneration = Generation<PopulationIdTag>;
using EvidenceGeneration = Generation<EvaluationIdTag>;

/// Coordinator incarnation counter. Advances every time a coordinator process
/// takes ownership of a durable foundry run. Durability across restarts is the
/// only way for this value to be preserved; it is never derived from wall time.
class CoordinatorEpoch {
 public:
  constexpr CoordinatorEpoch() noexcept = default;

  [[nodiscard]] static constexpr CoordinatorEpoch from_value(std::uint64_t value) noexcept {
    CoordinatorEpoch e;
    e.value_ = value;
    return e;
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  [[nodiscard]] constexpr std::optional<CoordinatorEpoch> next() const noexcept {
    if (value_ == UINT64_MAX) {
      return std::nullopt;
    }
    return CoordinatorEpoch::from_value(value_ + 1);
  }

  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(CoordinatorEpoch, CoordinatorEpoch) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(CoordinatorEpoch a,
                                                    CoordinatorEpoch b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::uint64_t value_{0};
};

/// Salt assigned to one durable foundry run.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API std::uint32_t make_id_salt();

/// Monotonic identity allocator for a single foundry run.
///
/// The allocator guarantees that no identity issued by one run is ever equal
/// to another identity issued by the same run for the same domain: the counter
/// is monotonic and the salt is stable. Across independent runs the salt makes
/// collision probability negligible, and every ingest path additionally
/// rejects duplicate identities, so even an astronomically unlikely collision
/// surfaces as a hard error rather than as state corruption.
class AUTONOMOUS_FOUNDRY_API IdAllocator {
 public:
  IdAllocator() noexcept = default;
  explicit IdAllocator(std::uint32_t salt) noexcept;

  [[nodiscard]] std::uint32_t salt() const noexcept { return salt_; }

  template <typename Tag>
  [[nodiscard]] Result<StrongId<Tag>> next() {
    return next_kind<Tag>(Tag::kind);
  }

  /// Restore a counter that was read from durable state. The restored counter
  /// must not be lower than the current one, otherwise durable identity
  /// allocation could regress and re-issue an identity that already exists.
  Status restore_counter(IdKind kind, std::uint32_t value);

  [[nodiscard]] std::uint32_t peek_counter(IdKind kind) const noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= counters_.size()) {
      return 0;
    }
    return counters_[index];
  }

  [[nodiscard]] const std::array<std::uint32_t, kIdKindCount>& counters() const noexcept {
    return counters_;
  }

  /// Re-seed the salt. Only legal while no identity has been issued.
  Status adopt_salt(std::uint32_t salt) noexcept;

 private:
  template <typename Tag>
  [[nodiscard]] Result<StrongId<Tag>> next_kind(IdKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    if (index == 0 || index >= counters_.size()) {
      return Status(ErrorCode::Internal, "identity allocator used with an unknown domain");
    }
    if (counters_[index] == 0xFFFFFFFFu) {
      return Status(ErrorCode::IdentityExhausted,
                    "identity counter for domain '" + std::string(id_kind_name(kind)) +
                        "' is exhausted");
    }
    counters_[index] += 1;
    const std::uint64_t raw =
        (static_cast<std::uint64_t>(index) << detail::kIdKindShift) |
        ((static_cast<std::uint64_t>(salt_) & detail::kIdSaltMask) << detail::kIdSaltShift) |
        (static_cast<std::uint64_t>(counters_[index]) & detail::kIdCounterMask);
    return StrongId<Tag>::from_raw(raw);
  }

  std::uint32_t salt_{0};
  std::array<std::uint32_t, kIdKindCount> counters_{};
};

/// Mint a fresh WorkerBootId.
///
/// Boot identities are not allocated from the run allocator: each worker
/// incarnation must be distinguishable even when the worker process restarts
/// between coordinator restarts, so the value is drawn from process entropy
/// and is never reused.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API WorkerBootId make_worker_boot_id();

/// Mint a fresh FoundryRunId for a brand new durable run.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API FoundryRunId make_foundry_run_id();

/// Parse a decimal raw identity, for example "145071396001". Used by the CLI,
/// which resolves identities from the operator command line rather than from a
/// trusted in-process value.
[[nodiscard]] AUTONOMOUS_FOUNDRY_API Result<std::uint64_t> parse_raw_identity(
    std::string_view text);

}  // namespace autonomous_foundry

namespace std {

template <typename Tag>
struct hash<autonomous_foundry::StrongId<Tag>> {
  [[nodiscard]] std::size_t operator()(autonomous_foundry::StrongId<Tag> id) const noexcept {
    // splitmix64 finalizer: cheap, deterministic, well distributed over the
    // dense counter range that dominates real populations.
    std::uint64_t x = id.raw() + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x = x ^ (x >> 31);
    return static_cast<std::size_t>(x);
  }
};

template <typename Tag>
struct hash<autonomous_foundry::Generation<Tag>> {
  [[nodiscard]] std::size_t operator()(autonomous_foundry::Generation<Tag> g) const noexcept {
    return static_cast<std::size_t>(g.value());
  }
};

}  // namespace std
