// Property suite.
//
// Deterministic seeded property testing. Every case uses one fixed literal seed
// per case, prints it before it generates anything, and reports the seed and the
// generated input in every failure, so a failing run names the exact input that
// broke the invariant.
//
// The properties are stated against an independent model of the documented
// contract: the state machines are re-derived from their documentation rather
// than read out of the production table, the lineage invariants are recomputed
// from the generated DAG, and the selection invariants are checked against the
// durable evidence rather than against the decision's own ranking.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/lineage.hpp"
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/selection.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

// ---------------------------------------------------------------------------
// Failure reporting: every message carries the seed and the generated input.
// ---------------------------------------------------------------------------

[[noreturn]] void fail_seeded(const af_test::TestContext& context, std::uint64_t seed,
                              const std::string& detail) {
  context.fail_at(__FILE__, __LINE__, "seed=" + std::to_string(seed) + " " + detail);
}

[[nodiscard]] std::string state_text(CandidateState state) {
  return std::string(candidate_state_name(state));
}

[[nodiscard]] std::string state_text(PopulationState state) {
  return std::string(population_state_name(state));
}

// ---------------------------------------------------------------------------
// Independent statements of the documented state machines
// ---------------------------------------------------------------------------

[[nodiscard]] bool model_candidate_legal(CandidateState from, CandidateState to) {
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
      return to == S::Selected || to == S::Retained || to == S::Retired || to == S::Disqualified ||
             to == S::Superseded || to == S::RevalidationRequired;
    case S::RevalidationRequired:
      return to == S::Evaluating || to == S::Evaluated || to == S::Retired || to == S::Superseded ||
             to == S::Disqualified;
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

[[nodiscard]] bool model_population_legal(PopulationState from, PopulationState to) {
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

// ---------------------------------------------------------------------------
// Lineage helpers
// ---------------------------------------------------------------------------

/// A candidate identity whose counter is its 1-based ordinal, so a generated
/// node can be indexed directly by its identity.
[[nodiscard]] CandidateId ordinal_candidate(std::uint32_t ordinal) {
  return CandidateId::from_raw((static_cast<std::uint64_t>(IdKind::Candidate) << 56) |
                               (static_cast<std::uint64_t>(ordinal) + 1ull));
}

[[nodiscard]] LineageId ordinal_lineage(std::uint32_t ordinal) {
  return LineageId::from_raw((static_cast<std::uint64_t>(IdKind::Lineage) << 56) |
                             (static_cast<std::uint64_t>(ordinal) + 1ull));
}

[[nodiscard]] std::size_t ordinal_of(CandidateId id) {
  return static_cast<std::size_t>(id.counter()) - 1u;
}

[[nodiscard]] bool holds_id(const std::vector<CandidateId>& values, CandidateId probe) {
  return std::find(values.begin(), values.end(), probe) != values.end();
}

[[nodiscard]] std::vector<CandidateId> sorted_unique(std::vector<CandidateId> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

/// Transitive ancestor closure recomputed from the generated parent lists.
[[nodiscard]] std::vector<CandidateId> model_ancestors(
    const std::vector<std::vector<CandidateId>>& parents, CandidateId node) {
  std::vector<CandidateId> closure;
  std::vector<CandidateId> frontier = parents[ordinal_of(node)];
  while (!frontier.empty()) {
    const CandidateId current = frontier.back();
    frontier.pop_back();
    if (current == node || holds_id(closure, current)) {
      continue;
    }
    closure.push_back(current);
    for (const CandidateId parent : parents[ordinal_of(current)]) {
      frontier.push_back(parent);
    }
  }
  return sorted_unique(closure);
}

/// Every node whose primary-parent chain reaches the probe, recomputed from the
/// generated parent lists.
[[nodiscard]] std::vector<CandidateId> model_primary_descendants(
    const std::vector<std::vector<CandidateId>>& parents, CandidateId probe) {
  std::vector<CandidateId> result;
  for (std::size_t index = 0; index < parents.size(); ++index) {
    CandidateId current = parents[index].empty() ? CandidateId() : parents[index].front();
    std::vector<CandidateId> walked;
    while (current.valid()) {
      if (current == probe) {
        result.push_back(ordinal_candidate(static_cast<std::uint32_t>(index)));
        break;
      }
      if (holds_id(walked, current)) {
        break;
      }
      walked.push_back(current);
      const std::vector<CandidateId>& parents_of_current = parents[ordinal_of(current)];
      current = parents_of_current.empty() ? CandidateId() : parents_of_current.front();
    }
  }
  return sorted_unique(result);
}

[[nodiscard]] bool same_ids(const std::vector<CandidateId>& left,
                            const std::vector<CandidateId>& right) {
  return sorted_unique(left) == sorted_unique(right);
}

[[nodiscard]] std::string render_ids(const std::vector<CandidateId>& values) {
  std::string text;
  for (const CandidateId value : values) {
    if (!text.empty()) {
      text.push_back(',');
    }
    text.append(value.to_string());
  }
  return text;
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string render_ranking(const SelectionDecision& decision) {
  std::string text;
  for (const RankingEntry& entry : decision.ranking) {
    text.append(entry.candidate.to_string());
    text.push_back('@');
    text.append(std::to_string(entry.rank));
    text.push_back('=');
    text.append(std::to_string(entry.total_score));
    text.push_back(';');
  }
  return text;
}

[[nodiscard]] const RankingEntry* find_ranking(const SelectionDecision& decision, CandidateId id) {
  for (const RankingEntry& entry : decision.ranking) {
    if (entry.candidate == id) {
      return &entry;
    }
  }
  return nullptr;
}

/// True when every mandatory requirement of the task holds for this candidate
/// in the durable evidence, independent of the decision under test.
[[nodiscard]] bool mandatory_gates_satisfied(FoundryCore& core, const TaskSpec& task,
                                             const CandidateRecord& candidate) {
  for (const EvaluationRequirement& requirement : task.requirements) {
    if (requirement.requirement_class != RequirementClass::Mandatory) {
      continue;
    }
    bool satisfied = false;
    for (const EvaluationRecord& record : core.candidate_evaluations(candidate.id)) {
      if (record.evaluator_key != requirement.evaluator_key) {
        continue;
      }
      if (record.candidate_generation != candidate.generation) {
        continue;
      }
      if (!record.complete || !evaluator_kind_is_authoritative(record.kind)) {
        continue;
      }
      if (!outcome_satisfies_mandatory(record.outcome)) {
        continue;
      }
      satisfied = true;
      break;
    }
    if (!satisfied) {
      return false;
    }
  }
  return true;
}

/// One generated evidence decision, expressed by ordinal so the identical plan
/// can be replayed against a second, independently built population.
struct EvidenceStep {
  std::size_t candidate_index;
  std::size_t requirement_index;
  EvaluationOutcome outcome;
  bool has_score;
  double score;
};

struct RequirementPlan {
  std::string_view key;
  EvaluatorKind kind;
  RequirementClass requirement_class;
};

/// The requirement set af_test::make_task declares, in declaration order.
[[nodiscard]] std::vector<RequirementPlan> requirement_plan() {
  return std::vector<RequirementPlan>{
      RequirementPlan{std::string_view(kEvaluatorCompileAndRun), EvaluatorKind::ProcessCommand,
                      RequirementClass::Mandatory},
      RequirementPlan{std::string_view(kEvaluatorSourcePolicy), EvaluatorKind::SourcePolicy,
                      RequirementClass::Mandatory},
      RequirementPlan{std::string_view(kEvaluatorPerformance), EvaluatorKind::ProcessCommand,
                      RequirementClass::Optional},
      RequirementPlan{std::string_view(kEvaluatorParsimony), EvaluatorKind::PortableReference,
                      RequirementClass::Optional},
  };
}

/// Drive one complete production attempt for one worker session.
///
/// This repeats the primitive sequence of af_test::produce_candidate because the
/// multi-candidate cycle below has to interleave the population round
/// bookkeeping with production, and the frozen fixture helper cannot.
[[nodiscard]] CandidateId produce_one(const af_test::TestContext& context, FoundryCore& core,
                                      const WorkerSessionAuthority& session,
                                      std::string_view solution_source,
                                      std::string_view declared_strategy) {
  const PendingDispatch pending = af_test::require_value(
      context, core.authorize_attempt(session), "authorize_attempt", __FILE__, __LINE__);
  const AttemptPackage package = af_test::require_value(
      context, core.confirm_dispatch(session, pending.attempt.id, pending.attempt.generation),
      "confirm_dispatch", __FILE__, __LINE__);

  WorkerOperationAuthority authority;
  authority.session = session;
  authority.population = package.population;
  authority.population_generation = package.population_generation;
  authority.task = package.task;
  authority.task_generation = package.task_generation;
  authority.candidate = package.candidate;
  authority.candidate_generation = package.candidate_generation;
  authority.attempt = package.attempt;
  authority.attempt_generation = package.attempt_generation;
  authority.assignment = package.assignment;

  const Status acknowledged = core.acknowledge_attempt(authority);
  if (!acknowledged.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("acknowledge_attempt failed: ") +
                        std::string(error_code_name(acknowledged.code())) + " '" +
                        acknowledged.message() + "'");
  }

  std::vector<ArtifactRef> artifacts;
  ArtifactRef solution;
  solution.name = std::string(kReferenceTaskSourceArtifact);
  solution.size_bytes = static_cast<std::uint64_t>(solution_source.size());
  solution.content_digest = sha256_hex(solution_source);
  artifacts.push_back(solution);

  const PublicationOutcome outcome = af_test::require_value(
      context, core.publish_candidate(authority, artifacts, std::string(declared_strategy)),
      "publish_candidate", __FILE__, __LINE__);
  return outcome.candidate;
}

/// Close one production round and return the population to Running.
///
/// A publication moves the population to Evaluating, which is the runtime's
/// statement that the round's candidate is now the population's authoritative
/// work and that no further slot may be opened until the round is closed.
/// Revalidation is that closure, and it refuses to reopen production while any
/// candidate is still Published or Evaluating, so every candidate of the round
/// must have complete evidence first.
void reopen_for_production(const af_test::TestContext& context, FoundryCore& core,
                           PopulationId population) {
  const Result<PopulationRecord> before = core.population(population);
  if (!before.ok() || before.value().state != PopulationState::Evaluating) {
    context.fail_at(__FILE__, __LINE__,
                    "reopen_for_production requires a population in Evaluating");
  }
  const Status requested = core.request_population_revalidation(
      population, "the production round is closed; the next slot needs fresh authority");
  if (!requested.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("request_population_revalidation failed: ") +
                        std::string(error_code_name(requested.code())) + " '" +
                        requested.message() + "'");
  }
  const Result<PopulationRecord> pending = core.population(population);
  if (!pending.ok()) {
    context.fail_at(__FILE__, __LINE__, "the population disappeared during revalidation");
  }
  const Status applied = core.revalidate_population(population, pending.value().generation,
                                                    "the production round is closed");
  if (!applied.ok()) {
    context.fail_at(__FILE__, __LINE__,
                    std::string("revalidate_population failed: ") +
                        std::string(error_code_name(applied.code())) + " '" + applied.message() +
                        "'");
  }
  const Result<PopulationRecord> after = core.population(population);
  if (!after.ok() || after.value().state != PopulationState::Running) {
    context.fail_at(__FILE__, __LINE__,
                    "revalidation did not return the population to Running, so the next "
                    "production slot cannot be dispatched");
  }
}

/// Record the generated evidence that belongs to one candidate ordinal.
void apply_plan_for(const af_test::TestContext& context, FoundryCore& core,
                    const std::vector<CandidateId>& candidates,
                    const std::vector<EvidenceStep>& plan, std::size_t candidate_index) {
  const std::vector<RequirementPlan> requirements = requirement_plan();
  const CandidateRecord candidate =
      af_test::load_candidate(context, core, candidates[candidate_index]);
  for (const EvidenceStep& step : plan) {
    if (step.candidate_index != candidate_index) {
      continue;
    }
    const RequirementPlan& requirement = requirements[step.requirement_index];
    af_test::record_evidence(context, core, candidate, requirement.key, step.outcome,
                             requirement.kind, requirement.requirement_class, step.has_score,
                             step.score);
  }
}

}  // namespace

AF_TEST_CASE(property, random_transition_sequences_agree_with_the_documented_machines) {
  af_ctx.phase("SEED");
  const std::uint64_t seed = 0x5EED000000000001ull;
  af_ctx.note("SEED " + std::to_string(seed));
  std::mt19937_64 engine(seed);

  af_ctx.phase("VERIFY_COUNTS");
  EXPECT_EQ(af_ctx, kCandidateStateCount, static_cast<std::size_t>(13));
  EXPECT_EQ(af_ctx, kPopulationStateCount, static_cast<std::size_t>(11));

  af_ctx.phase("WALK");
  const int candidate_state_count = static_cast<int>(kCandidateStateCount);
  const int population_state_count = static_cast<int>(kPopulationStateCount);
  std::uniform_int_distribution<int> candidate_pick(0, candidate_state_count - 1);
  std::uniform_int_distribution<int> population_pick(0, population_state_count - 1);

  std::uint64_t candidate_moves = 0;
  std::uint64_t population_moves = 0;
  std::uint64_t candidate_refusals = 0;
  std::uint64_t population_refusals = 0;
  for (int round = 0; round < 96; ++round) {
    CandidateState candidate_state = static_cast<CandidateState>(candidate_pick(engine));
    PopulationState population_state = static_cast<PopulationState>(population_pick(engine));
    for (int step = 0; step < 16; ++step) {
      const CandidateState candidate_target = static_cast<CandidateState>(candidate_pick(engine));
      const bool candidate_expected = model_candidate_legal(candidate_state, candidate_target);
      const bool candidate_actual =
          candidate_transition_is_legal(candidate_state, candidate_target);
      if (candidate_actual != candidate_expected) {
        fail_seeded(af_ctx, seed,
                    "candidate transition " + state_text(candidate_state) + " -> " +
                        state_text(candidate_target) + " was " +
                        (candidate_actual ? "accepted" : "refused") +
                        " but the documented machine " +
                        (candidate_expected ? "accepts" : "refuses") + " it");
      }
      if (candidate_state_is_terminal(candidate_state)) {
        if (candidate_target != candidate_state && candidate_actual) {
          fail_seeded(af_ctx, seed,
                      "terminal candidate state " + state_text(candidate_state) +
                          " accepted transition to " + state_text(candidate_target));
        }
      }
      if (candidate_actual) {
        candidate_state = candidate_target;
        ++candidate_moves;
      } else {
        ++candidate_refusals;
      }

      const PopulationState population_target =
          static_cast<PopulationState>(population_pick(engine));
      const bool population_expected = model_population_legal(population_state, population_target);
      const bool population_actual =
          population_transition_is_legal(population_state, population_target);
      if (population_actual != population_expected) {
        fail_seeded(af_ctx, seed,
                    "population transition " + state_text(population_state) + " -> " +
                        state_text(population_target) + " was " +
                        (population_actual ? "accepted" : "refused") +
                        " but the documented machine " +
                        (population_expected ? "accepts" : "refuses") + " it");
      }
      if (population_state_is_terminal(population_state)) {
        if (population_target != population_state && population_actual) {
          fail_seeded(af_ctx, seed,
                      "terminal population state " + state_text(population_state) +
                          " accepted transition to " + state_text(population_target));
        }
      }
      if (population_actual) {
        population_state = population_target;
        ++population_moves;
      } else {
        ++population_refusals;
      }
    }
  }
  // The walk must actually have exercised both verdicts, otherwise the property
  // would be vacuously true.
  EXPECT_TRUE(af_ctx, candidate_moves > 0);
  EXPECT_TRUE(af_ctx, candidate_refusals > 0);
  EXPECT_TRUE(af_ctx, population_moves > 0);
  EXPECT_TRUE(af_ctx, population_refusals > 0);

  af_ctx.phase("VERIFY_POPULATION_REFUSAL_FROM_A_REAL_CORE");
  af_test::FoundryFixture fixture(af_test::make_config(3101));
  af_test::build_running_fixture(af_ctx, fixture, 2);
  REQUIRE_VALUE(af_ctx, PopulationRecord, running, fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, running.state, PopulationState::Running);

  const Status cancelled =
      fixture.core.cancel_population(fixture.population, running.generation, "property walk");
  EXPECT_OK(af_ctx, cancelled);
  REQUIRE_VALUE(af_ctx, PopulationRecord, terminal, fixture.core.population(fixture.population));
  EXPECT_EQ(af_ctx, terminal.state, PopulationState::Cancelled);
  EXPECT_TRUE(af_ctx, population_state_is_terminal(terminal.state));
  EXPECT_FALSE(af_ctx, model_population_legal(terminal.state, PopulationState::Ready));
  const Status restarted =
      fixture.core.start_population(fixture.population, terminal.generation);
  EXPECT_STATUS_CODE(af_ctx, restarted, ErrorCode::IllegalStateTransition);

  af_ctx.phase("VERIFY_CANDIDATE_REFUSAL_FROM_A_REAL_CORE");
  af_test::FoundryFixture second(af_test::make_config(3102));
  af_test::build_running_fixture(af_ctx, second, 2);
  const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, second.core, 1);
  REQUIRE_VALUE(af_ctx, PendingDispatch, pending, second.core.authorize_attempt(session));
  REQUIRE_VALUE(af_ctx, CandidateRecord, producing,
                second.core.candidate(pending.attempt.candidate));
  EXPECT_EQ(af_ctx, producing.state, CandidateState::Producing);
  // Releasing an authorized attempt returns the candidate slot to Registered;
  // that edge is legal, and the model must agree that it is.
  EXPECT_TRUE(af_ctx, model_candidate_legal(CandidateState::Producing, CandidateState::Registered));

  // An ambiguous outcome leaves the slot retryable: the candidate returns to
  // Registered, and Registered -> Evaluating is not a legal edge.
  const Status ambiguous =
      second.core.force_attempt_outcome_unknown(pending.attempt.id, "property walk");
  EXPECT_OK(af_ctx, ambiguous);
  REQUIRE_VALUE(af_ctx, CandidateRecord, released,
                second.core.candidate(pending.attempt.candidate));
  EXPECT_EQ(af_ctx, released.state, CandidateState::Registered);
  EXPECT_FALSE(af_ctx, model_candidate_legal(released.state, CandidateState::Evaluating));
  const Status premature = second.core.begin_evaluation(released.id);
  EXPECT_STATUS_CODE(af_ctx, premature, ErrorCode::IllegalStateTransition);

  // An explicit cancellation is final for the candidate instead: it lands in a
  // terminal state with no edge back into evaluation.
  af_test::FoundryFixture third(af_test::make_config(3103));
  af_test::build_running_fixture(af_ctx, third, 2);
  const WorkerSessionAuthority third_session = af_test::connect_worker(af_ctx, third.core, 1);
  REQUIRE_VALUE(af_ctx, PendingDispatch, third_pending,
                third.core.authorize_attempt(third_session));
  const Status cancelled_attempt =
      third.core.cancel_attempt(third_pending.attempt.id, "property walk");
  EXPECT_OK(af_ctx, cancelled_attempt);
  REQUIRE_VALUE(af_ctx, CandidateRecord, cancelled_candidate,
                third.core.candidate(third_pending.attempt.candidate));
  EXPECT_EQ(af_ctx, cancelled_candidate.state, CandidateState::ProductionCancelled);
  EXPECT_TRUE(af_ctx, candidate_state_is_terminal(cancelled_candidate.state));
  EXPECT_FALSE(af_ctx,
               model_candidate_legal(cancelled_candidate.state, CandidateState::Evaluating));
  const Status after_cancellation = third.core.begin_evaluation(cancelled_candidate.id);
  EXPECT_STATUS_CODE(af_ctx, after_cancellation, ErrorCode::IllegalStateTransition);
}

AF_TEST_CASE(property, random_lineage_dags_stay_acyclic_and_depth_consistent) {
  af_ctx.phase("SEED");
  const std::uint64_t seed = 0x5EED000000000002ull;
  af_ctx.note("SEED " + std::to_string(seed));
  std::mt19937_64 engine(seed);

  af_ctx.phase("GENERATE");
  std::uniform_int_distribution<int> node_count(6, 40);
  std::uniform_int_distribution<int> parent_count(1, 3);
  const int total = node_count(engine);
  const int roots = 1 + static_cast<int>(engine() % 2ull);

  LineageGraph graph;
  std::vector<std::vector<CandidateId>> parents(static_cast<std::size_t>(total));
  std::vector<std::uint32_t> depths(static_cast<std::size_t>(total), 0);
  const CandidateGeneration generation = CandidateGeneration::first();
  const CoordinatorEpoch epoch = CoordinatorEpoch::from_value(1);

  for (int index = 0; index < total; ++index) {
    const CandidateId child = ordinal_candidate(static_cast<std::uint32_t>(index));
    std::vector<CandidateId> chosen;
    if (index >= roots) {
      const int wanted = std::min<int>(parent_count(engine), index);
      while (static_cast<int>(chosen.size()) < wanted) {
        const std::size_t pick =
            static_cast<std::size_t>(engine() % static_cast<std::uint64_t>(index));
        const CandidateId candidate = ordinal_candidate(static_cast<std::uint32_t>(pick));
        if (!holds_id(chosen, candidate)) {
          chosen.push_back(candidate);
        }
      }
    }
    parents[static_cast<std::size_t>(index)] = chosen;

    std::uint32_t expected_depth = 0;
    for (const CandidateId parent : chosen) {
      expected_depth = std::max(expected_depth, depths[ordinal_of(parent)] + 1u);
    }
    depths[static_cast<std::size_t>(index)] = expected_depth;

    const LineageId lineage = ordinal_lineage(static_cast<std::uint32_t>(engine() % 4ull));
    const Result<LineageNode> built =
        graph.make_child(child, generation, chosen, lineage, epoch);
    if (!built.ok()) {
      fail_seeded(af_ctx, seed,
                  "make_child refused generated node " + std::to_string(index) + " with " +
                      std::to_string(chosen.size()) + " parents: " +
                      std::string(error_code_name(built.status().code())) + " '" +
                      built.status().message() + "'");
    }
    if (built.value().depth != expected_depth) {
      fail_seeded(af_ctx, seed,
                  "make_child placed node " + std::to_string(index) + " at depth " +
                      std::to_string(built.value().depth) + " but its parents place it at " +
                      std::to_string(expected_depth));
    }
    const Status inserted = graph.insert(built.value());
    if (!inserted.ok()) {
      fail_seeded(af_ctx, seed,
                  "insert refused generated node " + std::to_string(index) + ": " +
                      std::string(error_code_name(inserted.code())) + " '" + inserted.message() +
                      "'");
    }
  }

  af_ctx.phase("VERIFY_STRUCTURE");
  EXPECT_EQ(af_ctx, graph.size(), static_cast<std::size_t>(total));
  const Status validated = graph.validate();
  if (!validated.ok()) {
    fail_seeded(af_ctx, seed,
                "generated lineage DAG failed validate(): " +
                    std::string(error_code_name(validated.code())) + " '" +
                    validated.message() + "'");
  }

  for (int index = 0; index < total; ++index) {
    const CandidateId node = ordinal_candidate(static_cast<std::uint32_t>(index));
    const LineageNode* found = graph.find(node);
    if (found == nullptr) {
      fail_seeded(af_ctx, seed, "node " + std::to_string(index) + " is missing from the graph");
    }
    if (found->depth != depths[static_cast<std::size_t>(index)]) {
      fail_seeded(af_ctx, seed,
                  "node " + std::to_string(index) + " has depth " + std::to_string(found->depth) +
                      " but max(parent depth)+1 is " +
                      std::to_string(depths[static_cast<std::size_t>(index)]));
    }
    if (found->depth != 0) {
      std::uint32_t deepest_parent = 0;
      for (const CandidateId parent : found->parents) {
        deepest_parent = std::max(deepest_parent, graph.find(parent)->depth);
      }
      if (found->depth != deepest_parent + 1u) {
        fail_seeded(af_ctx, seed,
                    "node " + std::to_string(index) + " depth " + std::to_string(found->depth) +
                        " is not one more than its deepest parent " +
                        std::to_string(deepest_parent));
      }
    }
  }

  af_ctx.phase("VERIFY_ANCESTRY");
  for (int index = 0; index < total; ++index) {
    const CandidateId node = ordinal_candidate(static_cast<std::uint32_t>(index));
    const std::vector<CandidateId> ancestors = graph.ancestors_of(node);
    if (holds_id(ancestors, node)) {
      fail_seeded(af_ctx, seed,
                  "node " + std::to_string(index) + " is its own ancestor, so the graph is cyclic");
    }
    const std::vector<CandidateId> expected = model_ancestors(parents, node);
    if (!same_ids(ancestors, expected)) {
      fail_seeded(af_ctx, seed,
                  "ancestors_of(node " + std::to_string(index) + ") is {" + render_ids(ancestors) +
                      "} but the generated DAG says {" + render_ids(expected) + "}");
    }
    for (const CandidateId parent : parents[static_cast<std::size_t>(index)]) {
      if (!holds_id(ancestors, parent)) {
        fail_seeded(af_ctx, seed,
                    "direct parent " + parent.to_string() + " of node " + std::to_string(index) +
                        " is missing from ancestors_of");
      }
    }
  }

  af_ctx.phase("VERIFY_PRIMARY_DESCENDANTS");
  for (int index = 0; index < total; ++index) {
    const CandidateId node = ordinal_candidate(static_cast<std::uint32_t>(index));
    const std::vector<CandidateId> descendants = graph.descendants_of(node);
    if (holds_id(descendants, node)) {
      fail_seeded(af_ctx, seed, "node " + std::to_string(index) + " is its own descendant");
    }
    const std::vector<CandidateId> expected = model_primary_descendants(parents, node);
    if (!same_ids(descendants, expected)) {
      fail_seeded(af_ctx, seed,
                  "descendants_of(node " + std::to_string(index) + ") is {" +
                      render_ids(descendants) + "} but the primary-parent chains say {" +
                      render_ids(expected) + "}");
    }
  }

  af_ctx.phase("VERIFY_PATH_TO_ROOT");
  for (int index = 0; index < total; ++index) {
    const CandidateId node = ordinal_candidate(static_cast<std::uint32_t>(index));
    const std::vector<CandidateId> path = graph.path_to_root(node);
    if (path.empty() || path.back() != node) {
      fail_seeded(af_ctx, seed,
                  "path_to_root(node " + std::to_string(index) + ") does not end at the node");
    }
    if (!path.empty()) {
      const LineageNode* root = graph.find(path.front());
      if (root == nullptr || !root->parents.empty()) {
        fail_seeded(af_ctx, seed,
                    "path_to_root(node " + std::to_string(index) + ") does not start at a root");
      }
      // The path follows the primary-parent chain, which is one specific route
      // to a root and therefore never longer than the node's depth.
      if (path.size() > static_cast<std::size_t>(graph.find(node)->depth) + 1u) {
        fail_seeded(af_ctx, seed,
                    "path_to_root(node " + std::to_string(index) + ") has " +
                        std::to_string(path.size()) + " entries but the node sits at depth " +
                        std::to_string(graph.find(node)->depth));
      }
      std::vector<CandidateId> expected_path;
      for (CandidateId current = node; current.valid();) {
        expected_path.push_back(current);
        const LineageNode* step = graph.find(current);
        if (step == nullptr || step->parents.empty()) {
          break;
        }
        current = step->parents.front();
      }
      std::reverse(expected_path.begin(), expected_path.end());
      if (path != expected_path) {
        fail_seeded(af_ctx, seed,
                    "path_to_root(node " + std::to_string(index) + ") is {" + render_ids(path) +
                        "} but the primary-parent chain is {" + render_ids(expected_path) + "}");
      }
      for (std::size_t step = 1; step < path.size(); ++step) {
        const LineageNode* child = graph.find(path[step]);
        if (child == nullptr || child->parents.empty() || child->parents.front() != path[step - 1]) {
          fail_seeded(af_ctx, seed,
                      "path_to_root(node " + std::to_string(index) + ") is not a primary-parent chain");
        }
      }
    }
  }

  af_ctx.phase("VERIFY_REFUSALS");
  {
    // A byte-identical re-insert is absorbed: it is a replay of history, not a
    // change to it.
    const LineageNode copy = *graph.find(ordinal_candidate(0));
    const Status replayed = graph.insert(copy);
    EXPECT_OK(af_ctx, replayed);

    // The same node with different ancestry is refused rather than rewritten.
    int mutable_index = -1;
    for (int index = 0; index < total; ++index) {
      if (!parents[static_cast<std::size_t>(index)].empty()) {
        mutable_index = index;
        break;
      }
    }
    EXPECT_TRUE(af_ctx, mutable_index >= 0);
    if (mutable_index >= 0) {
      LineageNode altered = *graph.find(ordinal_candidate(static_cast<std::uint32_t>(mutable_index)));
      altered.parents.push_back(ordinal_candidate(static_cast<std::uint32_t>(total - 1)));
      const Status rewritten = graph.insert(altered);
      EXPECT_STATUS_CODE(af_ctx, rewritten, ErrorCode::AlreadyExists);
    }

    LineageNode orphan;
    orphan.candidate = ordinal_candidate(static_cast<std::uint32_t>(total + 1));
    orphan.candidate_generation = generation;
    orphan.lineage = ordinal_lineage(0);
    orphan.parents = {ordinal_candidate(static_cast<std::uint32_t>(total + 2))};
    orphan.depth = 1;
    orphan.created_epoch = epoch;
    const Status orphan_status = graph.insert(orphan);
    EXPECT_STATUS_CODE(af_ctx, orphan_status, ErrorCode::LineageOrphan);

    LineageNode wrong_depth;
    wrong_depth.candidate = ordinal_candidate(static_cast<std::uint32_t>(total + 3));
    wrong_depth.candidate_generation = generation;
    wrong_depth.lineage = ordinal_lineage(0);
    wrong_depth.depth = depths[0] + 5u;
    wrong_depth.created_epoch = epoch;
    const Status depth_status = graph.insert(wrong_depth);
    EXPECT_STATUS_CODE(af_ctx, depth_status, ErrorCode::GenerationRegression);

    LineageNode self_parent;
    self_parent.candidate = ordinal_candidate(static_cast<std::uint32_t>(total + 4));
    self_parent.candidate_generation = generation;
    self_parent.lineage = ordinal_lineage(0);
    self_parent.parents = {self_parent.candidate};
    self_parent.created_epoch = epoch;
    const Status self_status = graph.insert(self_parent);
    EXPECT_STATUS_CODE(af_ctx, self_status, ErrorCode::LineageSelfParent);

    // The refusals changed nothing: the graph is exactly what the model says.
    EXPECT_EQ(af_ctx, graph.size(), static_cast<std::size_t>(total));
    EXPECT_OK(af_ctx, graph.validate());
  }
}

AF_TEST_CASE(property, random_candidate_sets_respect_the_selection_invariants) {
  af_ctx.phase("SEED");
  const std::uint64_t seed = 0x5EED000000000003ull;
  af_ctx.note("SEED " + std::to_string(seed));
  std::mt19937_64 engine(seed);

  const std::vector<RequirementPlan> requirements = requirement_plan();
  std::uniform_int_distribution<int> candidate_count(1, 4);
  std::uniform_int_distribution<int> chance(0, 99);
  std::uniform_real_distribution<double> score_distribution(0.0, 10.0);

  std::uint64_t committed_rounds = 0;
  std::uint64_t refused_rounds = 0;
  std::uint64_t impossible_rounds = 0;
  for (int round = 0; round < 6; ++round) {
    const std::uint32_t salt = 4000u + static_cast<std::uint32_t>(round);
    const std::size_t population_size =
        static_cast<std::size_t>(candidate_count(engine));

    // The evidence plan is generated once, from the seed, before either
    // population exists, so both populations observe exactly the same evidence.
    // The final round is deliberately built with every mandatory gate passing,
    // so the fully eligible path (every candidate ranked, exactly one selected)
    // is always exercised alongside the rounds that exclude candidates.
    const bool force_eligible = (round == 5);
    std::vector<EvidenceStep> plan;
    for (std::size_t candidate_index = 0; candidate_index < population_size; ++candidate_index) {
      for (std::size_t requirement_index = 0; requirement_index < requirements.size();
           ++requirement_index) {
        const bool mandatory =
            requirements[requirement_index].requirement_class == RequirementClass::Mandatory;
        const int roll = chance(engine);
        EvaluationOutcome outcome = EvaluationOutcome::Pass;
        if (mandatory && force_eligible) {
          outcome = EvaluationOutcome::Pass;
        } else if (mandatory) {
          if (roll >= 78 && roll < 92) {
            outcome = EvaluationOutcome::Fail;
          } else if (roll >= 92) {
            outcome = EvaluationOutcome::Unknown;
          }
        } else {
          if (roll >= 70 && roll < 88) {
            outcome = EvaluationOutcome::Fail;
          } else if (roll >= 88) {
            outcome = EvaluationOutcome::Unknown;
          }
        }
        const bool has_score = !mandatory && outcome == EvaluationOutcome::Pass;
        plan.push_back(EvidenceStep{candidate_index, requirement_index, outcome, has_score,
                                    has_score ? score_distribution(engine) : 0.0});
      }
    }

    af_ctx.note("ROUND " + std::to_string(round) + " candidates " +
                std::to_string(population_size) + " evidence steps " +
                std::to_string(plan.size()));

    af_ctx.phase("DISPATCH");
    af_test::FoundryFixture fixture(af_test::make_config(salt));
    af_test::build_running_fixture(af_ctx, fixture,
                                   static_cast<std::uint32_t>(population_size));
    // One production round at a time: a publication moves the population to
    // Evaluating, so a round's candidate must be evaluated and the round closed
    // before the next slot can be dispatched.
    std::vector<CandidateId> candidates;
    for (std::size_t slot = 1; slot <= population_size; ++slot) {
      const WorkerSessionAuthority session = af_test::connect_worker(
          af_ctx, fixture.core, static_cast<std::uint32_t>(slot));
      candidates.push_back(produce_one(
          af_ctx, fixture.core, session,
          af_test::reference_solution_source(static_cast<int>(slot - 1)),
          "strategy-" + std::to_string(slot)));
      const Status begun = fixture.core.begin_evaluation(candidates.back());
      if (!begun.ok()) {
        fail_seeded(af_ctx, seed,
                    "round " + std::to_string(round) + " begin_evaluation failed: " +
                        std::string(error_code_name(begun.code())) + " '" + begun.message() + "'");
      }
      apply_plan_for(af_ctx, fixture.core, candidates, plan, slot - 1);
      const CandidateRecord settled =
          af_test::load_candidate(af_ctx, fixture.core, candidates.back());
      if (settled.state != CandidateState::Evaluated) {
        fail_seeded(af_ctx, seed,
                    "round " + std::to_string(round) + " candidate " + std::to_string(slot) +
                        " is " + state_text(settled.state) +
                        " even though every declared requirement has a complete record");
      }
      if (slot < population_size) {
        reopen_for_production(af_ctx, fixture.core, fixture.population);
      }
    }

    REQUIRE_VALUE(af_ctx, TaskSpec, task, fixture.core.task(fixture.task));

    af_ctx.phase("SELECT");
    const Result<SelectionDecision> prepared = fixture.core.prepare_selection(fixture.population);
    if (!prepared.ok()) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " prepare_selection failed: " +
                      std::string(error_code_name(prepared.status().code())) + " '" +
                      prepared.status().message() + "'");
    }
    const SelectionDecision& decision = prepared.value();
    const Result<SelectionDecision> repeated = fixture.core.prepare_selection(fixture.population);
    if (!repeated.ok()) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " second prepare_selection failed: " +
                      std::string(error_code_name(repeated.status().code())));
    }

    // (c) identical canonical state and policy yields the identical winner and
    // the identical ranking order, on this core and on an independently built
    // twin population that observed exactly the same generated plan.
    if (render_ranking(decision) != render_ranking(repeated.value())) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " ranking changed between two prepares: '" +
                      render_ranking(decision) + "' vs '" + render_ranking(repeated.value()) + "'");
    }
    if (!(decision.selected == repeated.value().selected)) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " winner changed between two prepares: " +
                      decision.selected.to_string() + " vs " +
                      repeated.value().selected.to_string());
    }
    if (decision.canonical_state_digest != repeated.value().canonical_state_digest) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " canonical state digest changed between two prepares");
    }

    af_test::FoundryFixture twin(af_test::make_config(salt));
    af_test::build_running_fixture(af_ctx, twin, static_cast<std::uint32_t>(population_size));
    std::vector<CandidateId> twin_candidates;
    for (std::size_t slot = 1; slot <= population_size; ++slot) {
      const WorkerSessionAuthority session = af_test::connect_worker(
          af_ctx, twin.core, static_cast<std::uint32_t>(slot));
      twin_candidates.push_back(produce_one(
          af_ctx, twin.core, session,
          af_test::reference_solution_source(static_cast<int>(slot - 1)),
          "strategy-" + std::to_string(slot)));
      const Status begun = twin.core.begin_evaluation(twin_candidates.back());
      if (!begun.ok()) {
        fail_seeded(af_ctx, seed,
                    "round " + std::to_string(round) + " twin begin_evaluation failed: " +
                        std::string(error_code_name(begun.code())) + " '" + begun.message() + "'");
      }
      apply_plan_for(af_ctx, twin.core, twin_candidates, plan, slot - 1);
      if (slot < population_size) {
        reopen_for_production(af_ctx, twin.core, twin.population);
      }
    }
    REQUIRE_VALUE(af_ctx, SelectionDecision, twin_decision,
                  twin.core.prepare_selection(twin.population));
    if (render_ranking(twin_decision) != render_ranking(decision)) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) +
                      " the twin population produced a different ranking: '" +
                      render_ranking(twin_decision) + "' vs '" + render_ranking(decision) + "'");
    }
    if (!(twin_decision.selected == decision.selected)) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " the twin population produced winner " +
                      twin_decision.selected.to_string() + " but this one produced " +
                      decision.selected.to_string());
    }

    af_ctx.phase("VERIFY_INVARIANTS");
    if (decision.has_winner()) {
      // (b) every ranked candidate satisfies every mandatory gate, checked
      // against the durable evidence rather than against the decision.
      for (const RankingEntry& entry : decision.ranking) {
        const CandidateRecord candidate =
            af_test::load_candidate(af_ctx, fixture.core, entry.candidate);
        if (!mandatory_gates_satisfied(fixture.core, task, candidate)) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) + " ranked candidate " +
                          entry.candidate.to_string() +
                          " does not satisfy every mandatory gate");
        }
      }
      for (const ExclusionEntry& exclusion : decision.excluded) {
        if (find_ranking(decision, exclusion.candidate) != nullptr) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) + " excluded candidate " +
                          exclusion.candidate.to_string() + " also appears in the ranking");
        }
      }
      for (std::size_t index = 1; index < decision.ranking.size(); ++index) {
        if (decision.ranking[index - 1].total_score < decision.ranking[index].total_score) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) +
                          " the ranking is not ordered by non-increasing total score");
        }
      }
      for (const RankingEntry& entry : decision.ranking) {
        const CandidateRecord candidate =
            af_test::load_candidate(af_ctx, fixture.core, entry.candidate);
        if (!(candidate.state == CandidateState::Evaluated ||
              candidate.state == CandidateState::Selected)) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) + " ranked candidate " +
                          entry.candidate.to_string() + " is " +
                          state_text(candidate.state) + " rather than Evaluated");
        }
      }

      af_ctx.phase("COMMIT");
      // The prepared decision is bound to the canonical candidate/evidence state
      // it was derived from. On the mutating rounds something durable changes
      // between prepare and commit: one more complete authoritative evaluation
      // record is filed for the winner, so the decision describes evidence that
      // no longer matches the durable state. The commit must be refused as stale
      // rather than applied to a state the decision never saw. The final round is
      // left untouched so that the clean commit path is exercised as well.
      const bool mutated = round != 5;
      if (mutated) {
        const CandidateRecord winner =
            af_test::load_candidate(af_ctx, fixture.core, decision.selected);
        af_test::record_evidence(af_ctx, fixture.core, winner, "evidence-after-prepare",
                                 EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                                 RequirementClass::Optional, false, 0.0);
      }
      const Result<SelectionDecision> committed_result = fixture.core.commit_selection(decision);
      if (committed_result.ok()) {
        if (mutated) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) +
                          " committed a decision whose canonical evidence had changed since "
                          "it was prepared");
        }
        // (a) at most one selected candidate per population generation.
        std::size_t selected = 0;
        for (const CandidateId id : candidates) {
          const CandidateRecord candidate = af_test::load_candidate(af_ctx, fixture.core, id);
          if (candidate.selected) {
            ++selected;
            if (!(id == committed_result.value().selected)) {
              fail_seeded(af_ctx, seed,
                          "round " + std::to_string(round) + " candidate " + id.to_string() +
                              " is flagged selected but the decision selected " +
                              committed_result.value().selected.to_string());
            }
          }
        }
        if (selected != 1) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) + " has " + std::to_string(selected) +
                          " selected candidates, not exactly one");
        }
        ++committed_rounds;
      } else {
        // Only a round whose canonical state really moved may be refused: this
        // refusal must be the stale-decision check, not an accident of the
        // ranking, and it must be a refusal rather than a partial application.
        if (!mutated) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) +
                          " refused a selection that no mutation had invalidated: " +
                          std::string(error_code_name(committed_result.status().code())) + " '" +
                          committed_result.status().message() + "'");
        }
        if (committed_result.status().code() != ErrorCode::StaleSelection) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) + " refused the commit with " +
                          std::string(error_code_name(committed_result.status().code())) +
                          " rather than StaleSelection");
        }
        std::size_t selected = 0;
        for (const CandidateId id : candidates) {
          if (af_test::load_candidate(af_ctx, fixture.core, id).selected) {
            ++selected;
          }
        }
        if (selected != 0) {
          fail_seeded(af_ctx, seed,
                      "round " + std::to_string(round) + " refused the commit but left " +
                          std::to_string(selected) + " candidate(s) selected");
        }
        ++refused_rounds;
      }
    } else {
      ++impossible_rounds;
      EXPECT_EQ(af_ctx, decision.state, SelectionDecisionState::Impossible);
      EXPECT_TRUE(af_ctx, decision.ranking.empty());
    }
  }
  // Both verdicts must be reachable from the generated plans.
  EXPECT_TRUE(af_ctx, committed_rounds > 0);
  EXPECT_TRUE(af_ctx, refused_rounds > 0);
  af_ctx.note("ROUNDS committed " + std::to_string(committed_rounds) + " refused " +
              std::to_string(refused_rounds) + " impossible " + std::to_string(impossible_rounds));
}

AF_TEST_CASE(property, persistence_round_trips_observe_every_mutation) {
  af_ctx.phase("SEED");
  const std::uint64_t seed = 0x5EED000000000004ull;
  af_ctx.note("SEED " + std::to_string(seed));
  std::mt19937_64 engine(seed);

  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(4242));
  af_test::build_running_fixture(af_ctx, fixture, 3);

  af_ctx.phase("DISPATCH");
  std::vector<CandidateId> candidates;
  for (std::uint32_t slot = 1; slot <= 2; ++slot) {
    const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, slot);
    candidates.push_back(produce_one(
        af_ctx, fixture.core, session,
        af_test::reference_solution_source(static_cast<int>(slot - 1)),
        "strategy-" + std::to_string(slot)));
    const Status begun = fixture.core.begin_evaluation(candidates.back());
    if (!begun.ok()) {
      af_ctx.fail_at(__FILE__, __LINE__,
                     std::string("begin_evaluation failed: ") +
                         std::string(error_code_name(begun.code())) + " '" + begun.message() + "'");
    }
    const CandidateRecord candidate =
        af_test::load_candidate(af_ctx, fixture.core, candidates.back());
    af_test::record_evidence(af_ctx, fixture.core, candidate, kEvaluatorCompileAndRun,
                             EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                             RequirementClass::Mandatory, false, 0.0);
    af_test::record_evidence(af_ctx, fixture.core, candidate, kEvaluatorSourcePolicy,
                             EvaluationOutcome::Pass, EvaluatorKind::SourcePolicy,
                             RequirementClass::Mandatory, false, 0.0);
    af_test::record_evidence(af_ctx, fixture.core, candidate, kEvaluatorPerformance,
                             EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                             RequirementClass::Optional, true, 2.0);
    af_test::record_evidence(af_ctx, fixture.core, candidate, kEvaluatorParsimony,
                             EvaluationOutcome::Pass, EvaluatorKind::PortableReference,
                             RequirementClass::Optional, true, 2.0);
    const CandidateRecord settled =
        af_test::load_candidate(af_ctx, fixture.core, candidates.back());
    EXPECT_EQ(af_ctx, settled.state, CandidateState::Evaluated);
    if (slot < 2) {
      reopen_for_production(af_ctx, fixture.core, fixture.population);
    }
  }

  af_ctx.phase("COMMIT");
  REQUIRE_VALUE(af_ctx, SelectionDecision, prepared,
                fixture.core.prepare_selection(fixture.population));
  REQUIRE_VALUE(af_ctx, SelectionDecision, committed,
                fixture.core.commit_selection(prepared));
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention_prepared,
                fixture.core.prepare_retention(fixture.population));
  REQUIRE_VALUE(af_ctx, RetentionDecision, retention,
                fixture.core.commit_retention(retention_prepared));
  EXPECT_TRUE(af_ctx, retention.committed);

  af_ctx.phase("SERIALIZE");
  // The core accepts caller-minted worker identities and never advances its own
  // Worker counter, so a snapshot that holds workers declares the counter its
  // state requires and adopts it through the documented recovery path before an
  // image of it can be restored.
  FoundrySnapshot declared = fixture.core.snapshot();
  std::uint32_t worker_counter = declared.id_counters[static_cast<std::size_t>(IdKind::Worker)];
  for (const auto& entry : declared.workers) {
    worker_counter = std::max(worker_counter, entry.first.counter());
  }
  declared.id_counters[static_cast<std::size_t>(IdKind::Worker)] = worker_counter;
  const Status recovered = fixture.core.recover(declared);
  EXPECT_OK(af_ctx, recovered);

  FoundrySnapshot snapshot = fixture.core.snapshot();
  EXPECT_TRUE(af_ctx, snapshot.revision > 0);
  EXPECT_EQ(af_ctx, snapshot.id_counters[static_cast<std::size_t>(IdKind::Worker)],
            worker_counter);
  REQUIRE_VALUE(af_ctx, std::string, image, serialize_snapshot(snapshot));
  EXPECT_TRUE(af_ctx, image.size() > kSnapshotHeaderBytes);

  af_ctx.phase("VERIFY_BASELINE_ROUND_TRIP");
  REQUIRE_VALUE(af_ctx, FoundrySnapshot, baseline, deserialize_snapshot(image));
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, baseline));
  // The diagnostic reports the absence of a difference explicitly rather than
  // returning an empty string.
  EXPECT_EQ(af_ctx, first_snapshot_difference(snapshot, baseline), std::string("no difference found"));

  af_ctx.phase("MUTATE");
  FoundrySnapshot current = baseline;
  for (int round = 0; round < 8; ++round) {
    const std::uint64_t bump = 1ull + (engine() % 100000ull);
    const std::string marker =
        "property-round-" + std::to_string(round) + "-" + std::to_string(bump);
    current.revision += bump;
    current.statistics.evaluations_recorded += bump;
    const bool marker_applied = !current.populations.empty();
    if (marker_applied) {
      current.populations.begin()->second.status_detail = marker;
    }
    EXPECT_TRUE(af_ctx, marker_applied);

    REQUIRE_VALUE(af_ctx, std::string, mutated_image, serialize_snapshot(current));
    REQUIRE_VALUE(af_ctx, FoundrySnapshot, observed, deserialize_snapshot(mutated_image));

    if (observed.revision != current.revision) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " mutated revision " +
                      std::to_string(current.revision) + " but the reloaded snapshot reports " +
                      std::to_string(observed.revision));
    }
    if (observed.statistics.evaluations_recorded != current.statistics.evaluations_recorded) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) +
                      " the mutated statistics counter was silently dropped");
    }
    bool marker_observed = false;
    for (const auto& entry : observed.populations) {
      if (entry.second.status_detail == marker) {
        marker_observed = true;
        break;
      }
    }
    if (!marker_observed) {
      fail_seeded(af_ctx, seed,
                  "round " + std::to_string(round) + " the mutated durable field '" + marker +
                      "' was silently dropped");
    }
    EXPECT_TRUE(af_ctx, snapshots_are_equal(current, observed));
    EXPECT_FALSE(af_ctx, first_snapshot_difference(snapshot, observed).empty());

    // The generator is mutating: the next round builds on the observed value,
    // so a field that survives one round but not the next is still caught.
    current = observed;
  }

  af_ctx.phase("VERIFY_HISTORY_IS_UNCHANGED");
  REQUIRE_VALUE(af_ctx, FoundrySnapshot, pristine, deserialize_snapshot(image));
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, pristine));
  EXPECT_EQ(af_ctx, pristine.revision, snapshot.revision);
  EXPECT_TRUE(af_ctx, first_snapshot_difference(pristine, current).size() > 0);
}
