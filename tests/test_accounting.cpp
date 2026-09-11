// Accounting suite.
//
// Proves that reserve/consume/release balance exactly, that open reservations
// are reported, that closing a population with an open reservation is refused,
// that BudgetExhausted is reported at the limit, and that AccountingImbalance is
// never silently ignored.

#include <cstdint>
#include <string>

#include "autonomous_foundry/accounting.hpp"
#include "autonomous_foundry/error.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

[[nodiscard]] BudgetLimits tight_limits() {
  BudgetLimits limits;
  limits.max_candidate_attempts = 2;
  limits.max_worker_assignments = 2;
  limits.max_evaluation_attempts = 2;
  limits.max_retained_candidates = 4;
  limits.max_retries = 1;
  limits.max_candidates = 2;
  limits.max_workers = 4;
  limits.max_active_attempts = 2;
  limits.max_evaluation_concurrency = 1;
  return limits;
}

}  // namespace

AF_TEST_CASE(accounting, reserve_consume_and_release_balance_exactly) {
  af_ctx.phase("SETUP");
  BudgetLedger ledger(BudgetLimits{});
  const auto& attempts = ledger.counter(BudgetKind::CandidateAttempts);
  EXPECT_EQ(af_ctx, attempts.reserved, std::uint64_t{0});
  EXPECT_EQ(af_ctx, attempts.consumed, std::uint64_t{0});
  EXPECT_EQ(af_ctx, attempts.released, std::uint64_t{0});
  EXPECT_EQ(af_ctx, ledger.open_reservations(), std::uint64_t{0});
  EXPECT_OK(af_ctx, ledger.check_balanced());

  af_ctx.phase("VERIFY_RESERVE");
  EXPECT_OK(af_ctx, ledger.reserve(BudgetKind::CandidateAttempts));
  EXPECT_EQ(af_ctx, ledger.counter(BudgetKind::CandidateAttempts).reserved, std::uint64_t{1});
  EXPECT_EQ(af_ctx, ledger.in_use(BudgetKind::CandidateAttempts), std::uint64_t{1});
  EXPECT_EQ(af_ctx, ledger.open_reservations(), std::uint64_t{1});
  // An open reservation is an imbalance: the ledger refuses to report itself
  // balanced while capacity is still held.
  const Status open = ledger.check_balanced();
  EXPECT_STATUS_CODE(af_ctx, open, ErrorCode::AccountingImbalance);
  EXPECT_TRUE(af_ctx, open.message().find("CandidateAttempts") != std::string::npos);

  af_ctx.phase("VERIFY_CONSUME");
  EXPECT_OK(af_ctx, ledger.consume(BudgetKind::CandidateAttempts));
  EXPECT_EQ(af_ctx, ledger.counter(BudgetKind::CandidateAttempts).reserved, std::uint64_t{0});
  EXPECT_EQ(af_ctx, ledger.counter(BudgetKind::CandidateAttempts).consumed, std::uint64_t{1});
  EXPECT_EQ(af_ctx, ledger.in_use(BudgetKind::CandidateAttempts), std::uint64_t{1});
  EXPECT_EQ(af_ctx, ledger.open_reservations(), std::uint64_t{0});
  EXPECT_OK(af_ctx, ledger.check_balanced());

  af_ctx.phase("VERIFY_RELEASE_RETURNS_CAPACITY");
  EXPECT_OK(af_ctx, ledger.reserve(BudgetKind::WorkerAssignments));
  EXPECT_EQ(af_ctx, ledger.in_use(BudgetKind::WorkerAssignments), std::uint64_t{1});
  EXPECT_OK(af_ctx, ledger.release(BudgetKind::WorkerAssignments));
  EXPECT_EQ(af_ctx, ledger.counter(BudgetKind::WorkerAssignments).released, std::uint64_t{1});
  // Released capacity is returned to the pool rather than counted as used.
  EXPECT_EQ(af_ctx, ledger.in_use(BudgetKind::WorkerAssignments), std::uint64_t{0});
  EXPECT_OK(af_ctx, ledger.check_balanced());

  af_ctx.phase("VERIFY_NO_OPEN_RESERVATION");
  // Consuming or releasing without an open reservation is a state transition
  // error, not a silent no-op.
  EXPECT_STATUS_CODE(af_ctx, ledger.consume(BudgetKind::WorkerAssignments),
                     ErrorCode::IllegalStateTransition);
  EXPECT_STATUS_CODE(af_ctx, ledger.release(BudgetKind::WorkerAssignments),
                     ErrorCode::IllegalStateTransition);
  EXPECT_TRUE(af_ctx, ledger.counter(BudgetKind::WorkerAssignments).consumed == 0);

  af_ctx.phase("VERIFY_CONSUME_DIRECT");
  // A counter incremented where no cancellation window exists is consumed
  // directly, and still counts against the limit.
  EXPECT_OK(af_ctx, ledger.consume_direct(BudgetKind::EvaluationAttempts));
  EXPECT_EQ(af_ctx, ledger.counter(BudgetKind::EvaluationAttempts).consumed, std::uint64_t{1});
  EXPECT_EQ(af_ctx, ledger.counter(BudgetKind::EvaluationAttempts).reserved, std::uint64_t{0});
  EXPECT_OK(af_ctx, ledger.check_balanced());

  af_ctx.phase("VERIFY_COUNTER_NAMES");
  EXPECT_EQ(af_ctx, std::string(budget_kind_name(BudgetKind::CandidateAttempts)),
            std::string("CandidateAttempts"));
  EXPECT_EQ(af_ctx, std::string(budget_kind_name(BudgetKind::EvaluationConcurrency)),
            std::string("EvaluationConcurrency"));
  EXPECT_EQ(af_ctx, kBudgetKindCount, static_cast<std::size_t>(9));
}

AF_TEST_CASE(accounting, budget_exhausted_is_reported_at_the_limit) {
  af_ctx.phase("SETUP");
  BudgetLimits limits = tight_limits();
  BudgetLedger ledger(limits);

  af_ctx.phase("VERIFY_AT_THE_LIMIT");
  for (std::uint32_t index = 0; index < limits.max_candidate_attempts; ++index) {
    EXPECT_OK(af_ctx, ledger.reserve(BudgetKind::CandidateAttempts));
  }
  EXPECT_EQ(af_ctx, ledger.in_use(BudgetKind::CandidateAttempts),
            static_cast<std::uint64_t>(limits.max_candidate_attempts));
  EXPECT_FALSE(af_ctx, ledger.has_capacity(BudgetKind::CandidateAttempts));
  EXPECT_EQ(af_ctx, ledger.remaining(BudgetKind::CandidateAttempts), std::uint32_t{0});

  const Status exhausted = ledger.reserve(BudgetKind::CandidateAttempts);
  EXPECT_STATUS_CODE(af_ctx, exhausted, ErrorCode::BudgetExhausted);
  EXPECT_TRUE(af_ctx, exhausted.message().find("CandidateAttempts") != std::string::npos);
  // The refusal did not change the counter.
  EXPECT_EQ(af_ctx, ledger.in_use(BudgetKind::CandidateAttempts),
            static_cast<std::uint64_t>(limits.max_candidate_attempts));

  af_ctx.phase("VERIFY_DIRECT_CONSUMPTION_ALSO_RESPECTS_THE_LIMIT");
  EXPECT_STATUS_CODE(af_ctx, ledger.consume_direct(BudgetKind::CandidateAttempts),
                     ErrorCode::BudgetExhausted);

  af_ctx.phase("VERIFY_RELEASE_RESTORES_CAPACITY");
  EXPECT_OK(af_ctx, ledger.release(BudgetKind::CandidateAttempts));
  EXPECT_TRUE(af_ctx, ledger.has_capacity(BudgetKind::CandidateAttempts));
  EXPECT_EQ(af_ctx, ledger.remaining(BudgetKind::CandidateAttempts), std::uint32_t{1});
  EXPECT_OK(af_ctx, ledger.reserve(BudgetKind::CandidateAttempts));
  EXPECT_STATUS_CODE(af_ctx, ledger.reserve(BudgetKind::CandidateAttempts),
                     ErrorCode::BudgetExhausted);

  af_ctx.phase("VERIFY_PER_KIND_ISOLATION");
  // Exhausting one counter must not consume capacity from another.
  EXPECT_TRUE(af_ctx, ledger.has_capacity(BudgetKind::EvaluationAttempts));
  EXPECT_OK(af_ctx, ledger.reserve(BudgetKind::EvaluationAttempts));
  EXPECT_EQ(af_ctx, ledger.counter(BudgetKind::EvaluationAttempts).reserved, std::uint64_t{1});

  af_ctx.phase("VERIFY_LIMIT_LOOKUP");
  EXPECT_EQ(af_ctx, limits.limit_for(BudgetKind::CandidateAttempts), std::uint32_t{2});
  EXPECT_EQ(af_ctx, limits.limit_for(BudgetKind::EvaluationConcurrency), std::uint32_t{1});
  EXPECT_EQ(af_ctx, limits.limit_for(BudgetKind::Retries), std::uint32_t{1});
  EXPECT_EQ(af_ctx, static_cast<int>(BudgetKind::CandidateAttempts), 0);
  EXPECT_EQ(af_ctx, static_cast<int>(BudgetKind::EvaluationConcurrency), 8);

  af_ctx.phase("VERIFY_LIMIT_VALIDATION");
  // A limit of zero for a counter that must exist is a configuration defect.
  BudgetLimits zero_candidates;
  zero_candidates.max_candidates = 0;
  EXPECT_STATUS_CODE(af_ctx, zero_candidates.validate(), ErrorCode::InvalidArgument);
  BudgetLimits zero_workers;
  zero_workers.max_workers = 0;
  EXPECT_STATUS_CODE(af_ctx, zero_workers.validate(), ErrorCode::InvalidArgument);
  BudgetLimits zero_active;
  zero_active.max_active_attempts = 0;
  EXPECT_STATUS_CODE(af_ctx, zero_active.validate(), ErrorCode::InvalidArgument);
  BudgetLimits zero_concurrency;
  zero_concurrency.max_evaluation_concurrency = 0;
  EXPECT_STATUS_CODE(af_ctx, zero_concurrency.validate(), ErrorCode::InvalidArgument);
  EXPECT_OK(af_ctx, BudgetLimits{}.validate());
}

AF_TEST_CASE(accounting, ledger_state_restores_and_reports_imbalance) {
  af_ctx.phase("SETUP");
  BudgetLedger source(BudgetLimits{});
  EXPECT_OK(af_ctx, source.reserve(BudgetKind::Retries));
  EXPECT_OK(af_ctx, source.consume(BudgetKind::Retries));
  EXPECT_OK(af_ctx, source.reserve(BudgetKind::Candidates));
  EXPECT_EQ(af_ctx, source.open_reservations(), std::uint64_t{1});

  af_ctx.phase("VERIFY_RESTORE");
  // A restored counter array reproduces the ledger exactly, which is what makes
  // the accounting survive serialization without becoming part of the record
  // identity.
  BudgetLedger restored(BudgetLimits{});
  restored.restore(source.counters());
  EXPECT_EQ(af_ctx, restored.counter(BudgetKind::Retries).consumed, std::uint64_t{1});
  EXPECT_EQ(af_ctx, restored.open_reservations(), std::uint64_t{1});
  EXPECT_EQ(af_ctx, restored.counter(BudgetKind::CandidateAttempts),
            source.counter(BudgetKind::CandidateAttempts));

  af_ctx.phase("VERIFY_IMBALANCE_IS_NAMED");
  const Status imbalance = restored.check_balanced();
  EXPECT_STATUS_CODE(af_ctx, imbalance, ErrorCode::AccountingImbalance);
  EXPECT_TRUE(af_ctx, imbalance.message().find("Candidates") != std::string::npos);
  // The imbalance is not silently absorbed: it persists until it is resolved.
  EXPECT_STATUS_CODE(af_ctx, restored.check_balanced(), ErrorCode::AccountingImbalance);
  EXPECT_OK(af_ctx, restored.release(BudgetKind::Candidates));
  EXPECT_OK(af_ctx, restored.check_balanced());

  af_ctx.phase("VERIFY_MULTIPLE_OPEN_KINDS_ARE_ALL_REPORTED");
  BudgetLedger multiple(BudgetLimits{});
  EXPECT_OK(af_ctx, multiple.reserve(BudgetKind::CandidateAttempts));
  EXPECT_OK(af_ctx, multiple.reserve(BudgetKind::WorkerAssignments));
  EXPECT_EQ(af_ctx, multiple.open_reservations(), std::uint64_t{2});
  const Status named = multiple.check_balanced();
  EXPECT_STATUS_CODE(af_ctx, named, ErrorCode::AccountingImbalance);
  EXPECT_TRUE(af_ctx, named.message().find("CandidateAttempts") != std::string::npos);
  EXPECT_TRUE(af_ctx, named.message().find("WorkerAssignments") != std::string::npos);
}

AF_TEST_CASE(accounting, population_closure_refuses_an_open_reservation) {
  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(41));
  af_test::build_running_fixture(af_ctx, fixture, 2);

  af_ctx.phase("DISPATCH");
  // Authorizing an attempt reserves budgets and leaves them open until the
  // attempt reaches a terminal state.
  const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, 1);
  REQUIRE_VALUE(af_ctx, PendingDispatch, pending, fixture.core.authorize_attempt(session));
  const af_test::LedgerSnapshot before =
      af_test::ledger_snapshot(af_ctx, fixture.core, fixture.population);
  EXPECT_TRUE(af_ctx, before.open_reservations > 0);

  af_ctx.phase("VERIFY_BLOCKER");
  const std::vector<std::string> blockers =
      fixture.core.closure_blockers(fixture.population);
  EXPECT_FALSE(af_ctx, blockers.empty());
  bool named_open_capacity = false;
  for (const std::string& blocker : blockers) {
    if (blocker.find("reservation") != std::string::npos ||
        blocker.find("attempt") != std::string::npos) {
      named_open_capacity = true;
    }
  }
  EXPECT_TRUE(af_ctx, named_open_capacity);

  // Closing now is refused, and the refusal is a closure contract failure
  // rather than a silent imbalance.
  const Status refused = fixture.core.close_population(fixture.population);
  EXPECT_NOT_OK(af_ctx, refused);

  af_ctx.phase("COMMIT");
  // Resolving the attempt into a terminal state releases the reservation and
  // the accounting balances.
  EXPECT_OK(af_ctx, fixture.core.cancel_attempt(pending.attempt.id, "test cleanup"));
  const af_test::LedgerSnapshot after =
      af_test::ledger_snapshot(af_ctx, fixture.core, fixture.population);
  EXPECT_EQ(af_ctx, after.open_reservations, std::uint64_t{0});
  EXPECT_TRUE(af_ctx, after.consumed >= before.consumed);

  af_ctx.phase("VERIFY_BALANCED");
  const CandidateRecord candidate =
      af_test::load_candidate(af_ctx, fixture.core, pending.attempt.candidate);
  EXPECT_TRUE(af_ctx, candidate.state == CandidateState::ProductionCancelled ||
                          candidate.state == CandidateState::Registered);
  REQUIRE_VALUE(af_ctx, PopulationRecord, current, fixture.core.population(fixture.population));
  EXPECT_TRUE(af_ctx, current.candidates.size() >= 1);
}