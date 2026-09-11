// Identity suite.
//
// Proves that every identity domain is a distinct C++ type, that an identity
// smuggled through an untyped channel is rejected against its declared domain,
// that allocation is monotonic and collision free, that generation counters
// refuse to regress or wrap, and that the operator-facing decimal parser has a
// precise accept/reject matrix.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/id.hpp"
#include "test_support.hpp"

namespace {

using namespace autonomous_foundry;

/// Every identity type the runtime defines, as the static assert table below
/// needs them enumerated explicitly.
using AllIdentities = std::tuple<FoundryId, FoundryRunId, PopulationId, TaskId, CandidateId,
                                 LineageId, WorkerId, WorkerBootId, AttemptId, AssignmentId,
                                 EvaluationId, EvaluatorId, PolicyId, ArtifactId,
                                 PromotionRequestId, SessionId, ControllerId>;

/// Compile-time proof that two identity types are different types. This is a
/// static assert rather than a runtime check because a shared type would be a
/// silent aliasing bug in every translation unit, not a runtime event.
template <typename A, typename B>
constexpr bool distinct_types() {
  return !std::is_same_v<A, B>;
}

static_assert((distinct_types<FoundryId, FoundryRunId>()));
static_assert((distinct_types<PopulationId, TaskId>()));
static_assert((distinct_types<TaskId, CandidateId>()));
static_assert((distinct_types<CandidateId, LineageId>()));
static_assert((distinct_types<LineageId, WorkerId>()));
static_assert((distinct_types<WorkerId, WorkerBootId>()));
static_assert((distinct_types<WorkerBootId, AttemptId>()));
static_assert((distinct_types<AttemptId, AssignmentId>()));
static_assert((distinct_types<AssignmentId, EvaluationId>()));
static_assert((distinct_types<EvaluationId, EvaluatorId>()));
static_assert((distinct_types<EvaluatorId, PolicyId>()));
static_assert((distinct_types<PolicyId, ArtifactId>()));
static_assert((distinct_types<ArtifactId, PromotionRequestId>()));
static_assert((distinct_types<PromotionRequestId, SessionId>()));
static_assert((distinct_types<SessionId, ControllerId>()));
static_assert((distinct_types<PopulationGeneration, TaskGeneration>()));
static_assert((distinct_types<PopulationGeneration, CandidateGeneration>()));
static_assert((distinct_types<CandidateGeneration, PolicyGeneration>()));

/// Identity types are not interchangeable with the raw 64-bit value they
/// encode, and each domain tag is distinct.
static_assert(!std::is_convertible_v<std::uint64_t, FoundryId>);
static_assert((identities_are_cross_domain<FoundryId, FoundryRunId>()));
static_assert((identities_are_cross_domain<PopulationId, CandidateId>()));
static_assert(!(identities_are_cross_domain<PolicyId, PolicyId>()));
static_assert(kIdKindCount == static_cast<std::size_t>(IdKind::Controller) + 1u);

/// Distinct C++ types are still required to carry distinct domain tags: two
/// domains that shared a tag would collide on the wire and in a snapshot.
[[nodiscard]] bool all_domain_tags_distinct() {
  const std::array<IdKind, 17> kinds{IdKind::Foundry,     IdKind::FoundryRun, IdKind::Population,
                                     IdKind::Task,        IdKind::Candidate,  IdKind::Lineage,
                                     IdKind::Worker,      IdKind::WorkerBoot, IdKind::Attempt,
                                     IdKind::Assignment,  IdKind::Evaluation, IdKind::Evaluator,
                                     IdKind::Policy,      IdKind::Artifact,   IdKind::PromotionRequest,
                                     IdKind::Session,     IdKind::Controller};
  for (std::size_t left = 0; left < kinds.size(); ++left) {
    if (kinds[left] == IdKind::None) {
      return false;
    }
    for (std::size_t right = left + 1; right < kinds.size(); ++right) {
      if (kinds[left] == kinds[right]) {
        return false;
      }
    }
  }
  return true;
}

/// Raw value with the requested domain tag, a non-zero salt and a non-zero
/// counter, built without going through the allocator.
[[nodiscard]] std::uint64_t raw_for(IdKind kind, std::uint32_t counter) {
  const std::uint64_t kind_bits = static_cast<std::uint64_t>(kind) << 56;
  const std::uint64_t salt_bits = 0x000005ull << 32;
  return kind_bits | salt_bits | static_cast<std::uint64_t>(counter);
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

}  // namespace

AF_TEST_CASE(ids, identity_domains_are_distinct_types) {
  af_ctx.phase("VERIFY");
  EXPECT_TRUE(af_ctx, all_domain_tags_distinct());
  EXPECT_TRUE(af_ctx, (identities_are_cross_domain<WorkerId, WorkerBootId>()));
  EXPECT_FALSE(af_ctx, (identities_are_cross_domain<AttemptId, AttemptId>()));

  // A null identity is a valid object with a null raw value, never a valid
  // identity.
  const CandidateId empty;
  EXPECT_TRUE(af_ctx, empty.is_null());
  EXPECT_FALSE(af_ctx, empty.valid());
  EXPECT_FALSE(af_ctx, static_cast<bool>(empty));
  EXPECT_EQ(af_ctx, empty.raw(), std::uint64_t{0});
  EXPECT_EQ(af_ctx, CandidateId::null().raw(), std::uint64_t{0});
  EXPECT_TRUE(af_ctx, CandidateId::from_raw(raw_for(IdKind::Candidate, 7)) != empty);
}

AF_TEST_CASE(ids, cross_domain_raw_values_are_rejected) {
  af_ctx.phase("VERIFY");
  const std::uint64_t population_bits = raw_for(IdKind::Population, 12);

  // The value is well formed, but it is not a TaskId.
  const Result<TaskId> rejected = strong_id_from_raw_checked<TaskIdTag>(population_bits, "task");
  EXPECT_STATUS_CODE(af_ctx, rejected, ErrorCode::CrossDomainIdentity);
  EXPECT_TRUE(af_ctx, contains(rejected.status().message(), "task"));
  EXPECT_TRUE(af_ctx, contains(rejected.status().message(), "Population"));
  EXPECT_TRUE(af_ctx, contains(rejected.status().message(), "Task"));

  // The same value is accepted by its own domain.
  const Result<PopulationId> accepted =
      strong_id_from_raw_checked<PopulationIdTag>(population_bits, "population");
  EXPECT_OK(af_ctx, accepted);
  EXPECT_EQ(af_ctx, accepted.value().raw(), population_bits);

  // Zero is never accepted by any domain, even with an otherwise valid tag.
  const Result<CandidateId> null_value = strong_id_from_raw_checked<CandidateIdTag>(0, "candidate");
  EXPECT_STATUS_CODE(af_ctx, null_value, ErrorCode::NullIdentity);
  EXPECT_TRUE(af_ctx, contains(null_value.status().message(), "candidate"));

  // A raw value whose tag byte is zero is also null.
  const Result<WorkerId> tagless = strong_id_from_raw_checked<WorkerIdTag>(0x0000000000000042ull, "worker");
  EXPECT_STATUS_CODE(af_ctx, tagless, ErrorCode::NullIdentity);

  // Documented accessors agree with the encoding.
  const CandidateId candidate = CandidateId::from_raw(raw_for(IdKind::Candidate, 4096));
  EXPECT_EQ(af_ctx, static_cast<int>(candidate.kind()), static_cast<int>(IdKind::Candidate));
  EXPECT_EQ(af_ctx, candidate.counter(), std::uint32_t{4096});
  EXPECT_EQ(af_ctx, candidate.salt(), std::uint32_t{5});
  EXPECT_TRUE(af_ctx, contains(candidate.to_string(), "CandidateId:"));
}

AF_TEST_CASE(ids, null_identity_is_rejected_in_every_domain) {
  af_ctx.phase("VERIFY");
  EXPECT_STATUS_CODE(af_ctx, strong_id_from_raw_checked<FoundryIdTag>(0, "foundry"),
                     ErrorCode::NullIdentity);
  EXPECT_STATUS_CODE(af_ctx, strong_id_from_raw_checked<PolicyIdTag>(0, "policy"),
                     ErrorCode::NullIdentity);
  EXPECT_STATUS_CODE(af_ctx, strong_id_from_raw_checked<SessionIdTag>(0, "session"),
                     ErrorCode::NullIdentity);
  EXPECT_STATUS_CODE(af_ctx, strong_id_from_raw_checked<ControllerIdTag>(0, "controller"),
                     ErrorCode::NullIdentity);

  // A null identity carries the None kind, so it can never masquerade as a
  // member of a domain even if the null check were removed.
  const AttemptId null_attempt;
  EXPECT_EQ(af_ctx, static_cast<int>(null_attempt.kind()), static_cast<int>(IdKind::None));
  EXPECT_FALSE(af_ctx, null_attempt.valid());
}

AF_TEST_CASE(ids, allocator_counters_are_monotonic_and_collision_free) {
  af_ctx.phase("SETUP");
  constexpr std::uint32_t kAllocationsPerDomain = 100000;
  IdAllocator allocator(make_id_salt());

  af_ctx.phase("VERIFY");
  {
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(kAllocationsPerDomain * 2u);
    std::uint64_t previous = 0;
    for (std::uint32_t index = 0; index < kAllocationsPerDomain; ++index) {
      const Result<CandidateId> next = allocator.next<CandidateIdTag>();
      if (!next.ok()) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("candidate allocation failed at index ") +
                           std::to_string(index) + ": " + next.status().message());
      }
      const std::uint64_t raw = next.value().raw();
      if (raw <= previous) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("candidate counter did not advance at index ") +
                           std::to_string(index));
      }
      previous = raw;
      if (!seen.insert(raw).second) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("candidate identity collision at index ") +
                           std::to_string(index));
      }
      if (next.value().kind() != IdKind::Candidate) {
        af_ctx.fail_at(__FILE__, __LINE__, "candidate allocation minted a foreign domain tag");
      }
    }
    EXPECT_EQ(af_ctx, seen.size(), static_cast<std::size_t>(kAllocationsPerDomain));
    EXPECT_EQ(af_ctx, allocator.peek_counter(IdKind::Candidate), kAllocationsPerDomain);
  }

  af_ctx.phase("VERIFY_ATTEMPT_DOMAIN");
  {
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(kAllocationsPerDomain * 2u);
    for (std::uint32_t index = 0; index < kAllocationsPerDomain; ++index) {
      const Result<AttemptId> next = allocator.next<AttemptIdTag>();
      if (!next.ok()) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("attempt allocation failed at index ") +
                           std::to_string(index) + ": " + next.status().message());
      }
      if (!seen.insert(next.value().raw()).second) {
        af_ctx.fail_at(__FILE__, __LINE__,
                       std::string("attempt identity collision at index ") + std::to_string(index));
      }
    }
    EXPECT_EQ(af_ctx, seen.size(), static_cast<std::size_t>(kAllocationsPerDomain));
    EXPECT_EQ(af_ctx, allocator.peek_counter(IdKind::Attempt), kAllocationsPerDomain);
    // Counters are per domain: candidate allocation did not move the attempt
    // counter and vice versa.
    EXPECT_EQ(af_ctx, allocator.peek_counter(IdKind::Candidate), kAllocationsPerDomain);
  }

  af_ctx.phase("VERIFY_DOMAIN_SEPARATION");
  // Interleaved allocation across domains never produces a shared raw value,
  // because the domain tag occupies the high byte.
  std::unordered_set<std::uint64_t> interleaved;
  for (std::uint32_t index = 0; index < 1000; ++index) {
    const Result<PopulationId> population = allocator.next<PopulationIdTag>();
    const Result<TaskId> task = allocator.next<TaskIdTag>();
    const Result<LineageId> lineage = allocator.next<LineageIdTag>();
    EXPECT_OK(af_ctx, population);
    EXPECT_OK(af_ctx, task);
    EXPECT_OK(af_ctx, lineage);
    EXPECT_TRUE(af_ctx, interleaved.insert(population.value().raw()).second);
    EXPECT_TRUE(af_ctx, interleaved.insert(task.value().raw()).second);
    EXPECT_TRUE(af_ctx, interleaved.insert(lineage.value().raw()).second);
  }
}

AF_TEST_CASE(ids, restore_counter_rejects_regression_and_accepts_progress) {
  af_ctx.phase("SETUP");
  IdAllocator allocator(0x001234u);
  REQUIRE_VALUE(af_ctx, CandidateId, first, allocator.next<CandidateIdTag>());
  REQUIRE_VALUE(af_ctx, CandidateId, second, allocator.next<CandidateIdTag>());
  EXPECT_EQ(af_ctx, allocator.peek_counter(IdKind::Candidate), std::uint32_t{2});
  EXPECT_TRUE(af_ctx, first != second);

  af_ctx.phase("VERIFY");
  // Restoring a lower counter would re-issue an identity that already exists.
  const Status regression = allocator.restore_counter(IdKind::Candidate, 1);
  EXPECT_STATUS_CODE(af_ctx, regression, ErrorCode::GenerationRegression);
  EXPECT_TRUE(af_ctx, contains(regression.message(), "Candidate"));
  EXPECT_EQ(af_ctx, allocator.peek_counter(IdKind::Candidate), std::uint32_t{2});

  // An equal counter is a replay of durable state and is absorbed.
  EXPECT_OK(af_ctx, allocator.restore_counter(IdKind::Candidate, 2));
  EXPECT_EQ(af_ctx, allocator.peek_counter(IdKind::Candidate), std::uint32_t{2});

  // A higher counter is progress and is adopted.
  EXPECT_OK(af_ctx, allocator.restore_counter(IdKind::Candidate, 9000));
  EXPECT_EQ(af_ctx, allocator.peek_counter(IdKind::Candidate), std::uint32_t{9000});
  REQUIRE_VALUE(af_ctx, CandidateId, next, allocator.next<CandidateIdTag>());
  EXPECT_EQ(af_ctx, next.counter(), std::uint32_t{9001});
  EXPECT_TRUE(af_ctx, next != second);

  // The null domain and out-of-range domains are not restorable.
  EXPECT_STATUS_CODE(af_ctx, allocator.restore_counter(IdKind::None, 5), ErrorCode::Internal);
  EXPECT_STATUS_CODE(af_ctx, allocator.restore_counter(static_cast<IdKind>(200), 5),
                     ErrorCode::Internal);

  af_ctx.phase("VERIFY_SALT_RULES");
  // The salt may only be adopted before any identity exists.
  EXPECT_STATUS_CODE(af_ctx, allocator.adopt_salt(0x000042u), ErrorCode::IllegalStateTransition);
  IdAllocator fresh(0x000007u);
  EXPECT_EQ(af_ctx, fresh.salt(), std::uint32_t{0x000007u});
  EXPECT_OK(af_ctx, fresh.adopt_salt(0x00ABCDu));
  EXPECT_EQ(af_ctx, fresh.salt(), std::uint32_t{0x00ABCDu});
  const Result<SessionId> session = fresh.next<SessionIdTag>();
  EXPECT_OK(af_ctx, session);
  EXPECT_EQ(af_ctx, session.value().salt(), std::uint32_t{0x00ABCDu});
}

AF_TEST_CASE(ids, generation_counters_refuse_to_wrap) {
  af_ctx.phase("VERIFY");
  const CandidateGeneration first = CandidateGeneration::first();
  EXPECT_EQ(af_ctx, first.value(), std::uint32_t{1});
  EXPECT_TRUE(af_ctx, first.valid());

  const CandidateGeneration zero;
  EXPECT_FALSE(af_ctx, zero.valid());
  EXPECT_EQ(af_ctx, zero.value(), std::uint32_t{0});

  // next() reports failure at the numeric limit rather than rolling over, so a
  // generation can never be re-issued after the counter saturates.
  const CandidateGeneration last = CandidateGeneration::from_value(0xFFFFFFFFu);
  EXPECT_TRUE(af_ctx, !last.next().has_value());

  const CandidateGeneration penultimate = CandidateGeneration::from_value(0xFFFFFFFEu);
  const std::optional<CandidateGeneration> stepped = penultimate.next();
  EXPECT_TRUE(af_ctx, stepped.has_value());
  EXPECT_EQ(af_ctx, stepped.value().value(), std::uint32_t{0xFFFFFFFFu});
  EXPECT_TRUE(af_ctx, !stepped.value().next().has_value());

  // Generations order as unsigned values, so a regression is observable.
  EXPECT_TRUE(af_ctx, CandidateGeneration::from_value(2) < CandidateGeneration::from_value(3));
  EXPECT_TRUE(af_ctx, CandidateGeneration::from_value(0) < first);

  // Task and candidate generations are distinct types with the same rules.
  const TaskGeneration task_last = TaskGeneration::from_value(0xFFFFFFFFu);
  EXPECT_TRUE(af_ctx, !task_last.next().has_value());
  const std::optional<TaskGeneration> task_stepped = TaskGeneration::from_value(41).next();
  EXPECT_TRUE(af_ctx, task_stepped.has_value());
  EXPECT_EQ(af_ctx, task_stepped.value().value(), std::uint32_t{42});

  af_ctx.phase("VERIFY_COORDINATOR_EPOCH");
  const CoordinatorEpoch epoch_zero;
  EXPECT_FALSE(af_ctx, epoch_zero.valid());
  EXPECT_EQ(af_ctx, epoch_zero.value(), std::uint64_t{0});
  const CoordinatorEpoch epoch_one = CoordinatorEpoch::from_value(1);
  EXPECT_TRUE(af_ctx, epoch_one.valid());
  EXPECT_TRUE(af_ctx, epoch_zero < epoch_one);
  EXPECT_TRUE(af_ctx, !CoordinatorEpoch::from_value(UINT64_MAX).next().has_value());
  const std::optional<CoordinatorEpoch> epoch_next =
      CoordinatorEpoch::from_value(UINT64_MAX - 1ull).next();
  EXPECT_TRUE(af_ctx, epoch_next.has_value());
  EXPECT_EQ(af_ctx, epoch_next.value().value(), UINT64_MAX);
  EXPECT_EQ(af_ctx, epoch_next.value().to_string(), std::string("18446744073709551615"));
  EXPECT_EQ(af_ctx, epoch_one.to_string(), std::string("1"));
}

AF_TEST_CASE(ids, parse_raw_identity_accept_and_reject_matrix) {
  af_ctx.phase("VERIFY");
  // Accepted: a run of decimal digits describing a non-null 64-bit value.
  REQUIRE_VALUE(af_ctx, std::uint64_t, one, parse_raw_identity("1"));
  EXPECT_EQ(af_ctx, one, std::uint64_t{1});
  REQUIRE_VALUE(af_ctx, std::uint64_t, large, parse_raw_identity("18446744073709551615"));
  EXPECT_EQ(af_ctx, large, UINT64_MAX);
  REQUIRE_VALUE(af_ctx, std::uint64_t, leading_zero, parse_raw_identity("007"));
  EXPECT_EQ(af_ctx, leading_zero, std::uint64_t{7});
  REQUIRE_VALUE(af_ctx, std::uint64_t, mid, parse_raw_identity("145071396001"));
  EXPECT_EQ(af_ctx, mid, std::uint64_t{145071396001ull});

  // Rejected: empty, null, non-digits, signs, whitespace, separators, prefix
  // markers and overflow.
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity(""), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("0"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("00"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("18446744073709551616"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("99999999999999999999999"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("12a"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("a12"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("-1"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("+1"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity(" 1"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("1 "), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("1_000"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("0x10"), ErrorCode::InvalidArgument);
  EXPECT_STATUS_CODE(af_ctx, parse_raw_identity("CandidateId:12"), ErrorCode::InvalidArgument);

  // Every rejection names the offending text.
  const Result<std::uint64_t> rejected = parse_raw_identity("12x4");
  EXPECT_TRUE(af_ctx, contains(rejected.status().message(), "12x4"));

  af_ctx.phase("VERIFY_REFINEMENT");
  // A parsed value only becomes an identity after a second, domain-checked
  // refinement step: the parser deliberately does not guess a domain. A decimal
  // string that carries no domain tag was never minted by any domain, so the
  // refinement step refuses it as the null identity rather than accepting it.
  REQUIRE_VALUE(af_ctx, std::uint64_t, parsed, parse_raw_identity("145071396001"));
  EXPECT_STATUS_CODE(af_ctx, strong_id_from_raw_checked<CandidateIdTag>(parsed, "candidate"),
                     ErrorCode::NullIdentity);
}

AF_TEST_CASE(ids, minted_run_and_boot_identities_are_usable) {
  af_ctx.phase("VERIFY");
  const FoundryRunId run_a = make_foundry_run_id();
  const FoundryRunId run_b = make_foundry_run_id();
  EXPECT_TRUE(af_ctx, run_a.valid());
  EXPECT_TRUE(af_ctx, run_b.valid());
  EXPECT_TRUE(af_ctx, run_a != run_b);
  EXPECT_EQ(af_ctx, static_cast<int>(run_a.kind()), static_cast<int>(IdKind::FoundryRun));
  EXPECT_TRUE(af_ctx, run_a.salt() != 0);

  const WorkerBootId boot_a = make_worker_boot_id();
  const WorkerBootId boot_b = make_worker_boot_id();
  EXPECT_TRUE(af_ctx, boot_a.valid());
  EXPECT_TRUE(af_ctx, boot_a != boot_b);
  EXPECT_EQ(af_ctx, static_cast<int>(boot_a.kind()), static_cast<int>(IdKind::WorkerBoot));

  // A boot identity is never the null identity even if entropy is unlucky.
  EXPECT_FALSE(af_ctx, make_worker_boot_id().is_null());

  const std::uint32_t salt = make_id_salt();
  EXPECT_TRUE(af_ctx, salt != 0);
  EXPECT_TRUE(af_ctx, salt <= 0x00FFFFFFu);
}