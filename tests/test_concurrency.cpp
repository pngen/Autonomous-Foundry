// Concurrency suite.
//
// Proves, with real synchronisation and real work, that:
//
//   * the core's read paths stay individually consistent while another thread
//     drives real transitions, and that no read path is a torn view of state;
//   * a snapshot taken during heavy mutation is a complete immutable copy:
//     it is internally coherent, it serializes, and it deserializes back to an
//     equal value;
//   * repeated construction and destruction of the state machine is stable;
//   * selection raced by a writer and two readers never yields two winners for
//     one population generation, and every observed decision is internally
//     consistent and ranks only candidates that satisfy every mandatory gate.
//
// Every thread is joined. No sleep, deadline or execution-duration control is
// used anywhere: progress is proven by a real stop flag written by the writer
// after a fixed amount of work.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "autonomous_foundry/attempt.hpp"
#include "autonomous_foundry/candidate.hpp"
#include "autonomous_foundry/error.hpp"
#include "autonomous_foundry/foundry.hpp"
#include "autonomous_foundry/persistence.hpp"
#include "autonomous_foundry/reference_task.hpp"
#include "autonomous_foundry/selection.hpp"
#include "test_fixture.hpp"

namespace {

using namespace autonomous_foundry;

/// Failures observed on a worker thread. A thread never throws: the case body
/// owns the verdict, so every problem is recorded and asserted after the join.
class ProblemLog {
 public:
  void add(std::string text) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (problems_.size() < 16) {
      problems_.push_back(std::move(text));
    }
  }

  [[nodiscard]] std::vector<std::string> take() {
    std::lock_guard<std::mutex> guard(mutex_);
    return problems_;
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> problems_;
};

[[nodiscard]] std::string code_of(const Status& status) {
  return std::string(error_code_name(status.code()));
}

[[nodiscard]] std::string render(const std::vector<std::string>& problems) {
  std::string text;
  for (std::size_t index = 0; index < problems.size(); ++index) {
    if (index != 0) {
      text.append(" | ");
    }
    text.append(problems[index]);
  }
  return text;
}

// ---------------------------------------------------------------------------
// Raw driver helpers. These return a Status instead of failing a case, because
// they are called from worker threads where throwing would abort the process.
// ---------------------------------------------------------------------------

[[nodiscard]] Status connect_raw(FoundryCore& core, std::uint32_t slot,
                                 WorkerSessionAuthority* out) {
  WorkerRegistrationRequest request;
  request.worker = af_test::worker_identity(slot);
  request.boot = af_test::boot_identity(slot);
  request.label = "worker";
  request.capability = "reference";
  request.process_id = 5000u + slot;
  const Result<WorkerSessionAuthority> session = core.register_worker(request);
  if (!session.ok()) {
    return session.status();
  }
  const Status ready = core.worker_ready(session.value(), "reference");
  if (!ready.ok()) {
    return ready;
  }
  *out = session.value();
  return Status();
}

[[nodiscard]] Status produce_raw(FoundryCore& core, const WorkerSessionAuthority& session,
                                 std::string_view source, std::string_view strategy, CandidateId* out,
                                 WorkerOperationAuthority* authority_out) {
  const Result<PendingDispatch> pending = core.authorize_attempt(session);
  if (!pending.ok()) {
    return pending.status();
  }
  const Result<AttemptPackage> package =
      core.confirm_dispatch(session, pending.value().attempt.id, pending.value().attempt.generation);
  if (!package.ok()) {
    return package.status();
  }

  WorkerOperationAuthority authority;
  authority.session = session;
  authority.population = package.value().population;
  authority.population_generation = package.value().population_generation;
  authority.task = package.value().task;
  authority.task_generation = package.value().task_generation;
  authority.candidate = package.value().candidate;
  authority.candidate_generation = package.value().candidate_generation;
  authority.attempt = package.value().attempt;
  authority.attempt_generation = package.value().attempt_generation;
  authority.assignment = package.value().assignment;

  const Status acknowledged = core.acknowledge_attempt(authority);
  if (!acknowledged.ok()) {
    return acknowledged;
  }

  std::vector<ArtifactRef> artifacts;
  ArtifactRef solution;
  solution.name = std::string(kReferenceTaskSourceArtifact);
  solution.size_bytes = static_cast<std::uint64_t>(source.size());
  solution.content_digest = sha256_hex(source);
  artifacts.push_back(solution);

  const Result<PublicationOutcome> outcome =
      core.publish_candidate(authority, artifacts, std::string(strategy));
  if (!outcome.ok()) {
    return outcome.status();
  }
  *out = outcome.value().candidate;
  if (authority_out != nullptr) {
    *authority_out = authority;
  }
  return Status();
}

[[nodiscard]] Status record_raw(FoundryCore& core, CandidateId candidate_id, std::string_view key,
                                EvaluationOutcome outcome, EvaluatorKind kind,
                                RequirementClass requirement_class, bool has_score, double score) {
  const Result<CandidateRecord> candidate = core.candidate(candidate_id);
  if (!candidate.ok()) {
    return candidate.status();
  }
  const Result<PopulationRecord> population = core.population(candidate.value().population);
  if (!population.ok()) {
    return population.status();
  }

  EvaluationRecord record;
  record.candidate = candidate.value().id;
  record.candidate_generation = candidate.value().generation;
  record.task = candidate.value().task;
  record.task_generation = candidate.value().task_generation;
  record.population = candidate.value().population;
  record.population_generation = population.value().generation;
  record.evaluator = EvaluatorId::from_raw((static_cast<std::uint64_t>(IdKind::Evaluator) << 56) |
                                           0x0000C9ull << 32 |
                                           static_cast<std::uint64_t>(candidate.value().generation.value()));
  record.evaluator_key = std::string(key);
  record.kind = kind;
  record.requirement_class = requirement_class;
  record.outcome = outcome;
  record.complete = true;
  record.has_score = has_score;
  record.score = score;
  record.evidence_digest = "9f2c4c0c1f5a3d4e5b6c7d8e9f00112233445566778899aabbccddeeff001122";
  return core.record_evaluation(record);
}

/// Record the complete reference evidence set for one published candidate.
[[nodiscard]] Status record_full_evidence(FoundryCore& core, CandidateId candidate, double score) {
  AF_TRY(record_raw(core, candidate, kEvaluatorCompileAndRun, EvaluationOutcome::Pass,
                    EvaluatorKind::ProcessCommand, RequirementClass::Mandatory, false, 0.0));
  AF_TRY(record_raw(core, candidate, kEvaluatorSourcePolicy, EvaluationOutcome::Pass,
                    EvaluatorKind::SourcePolicy, RequirementClass::Mandatory, false, 0.0));
  AF_TRY(record_raw(core, candidate, kEvaluatorPerformance, EvaluationOutcome::Pass,
                    EvaluatorKind::ProcessCommand, RequirementClass::Optional, true, score));
  AF_TRY(record_raw(core, candidate, kEvaluatorParsimony, EvaluationOutcome::Pass,
                    EvaluatorKind::PortableReference, RequirementClass::Optional, true, score));
  return Status();
}

/// Drive the population through the documented revalidation cycle so that a
/// settled population accepts the next production attempt.
[[nodiscard]] Status cycle_population(FoundryCore& core, PopulationId population) {
  AF_TRY(core.request_population_revalidation(population, "resume production"));
  const Result<PopulationRecord> record = core.population(population);
  if (!record.ok()) {
    return record.status();
  }
  return core.revalidate_population(population, record.value().generation, "resume production");
}

[[nodiscard]] Status writer_round(FoundryCore& core, PopulationId population, std::uint32_t slot,
                                  std::uint32_t ordinal, bool cycle_after) {
  WorkerSessionAuthority session;
  AF_TRY(connect_raw(core, slot, &session));
  CandidateId candidate;
  AF_TRY(produce_raw(core, session, generate_reference_solution(ReferenceStrategy::ClosedForm),
                     "closed-form", &candidate, nullptr));
  AF_TRY(core.begin_evaluation(candidate));
  AF_TRY(record_full_evidence(core, candidate, 1.0 + static_cast<double>(ordinal)));
  if (cycle_after) {
    AF_TRY(cycle_population(core, population));
  }
  return Status();
}

// ---------------------------------------------------------------------------
// Consistency checks shared by the racing cases
// ---------------------------------------------------------------------------

/// Every mandatory requirement of the task must be satisfied by a complete,
/// authoritative, passing record for this candidate.
[[nodiscard]] bool satisfies_mandatory_gates(FoundryCore& core, const TaskSpec& task,
                                             CandidateId candidate) {
  const std::vector<EvaluationRecord> records = core.candidate_evaluations(candidate);
  for (const EvaluationRequirement& requirement : task.requirements) {
    if (requirement.requirement_class != RequirementClass::Mandatory) {
      continue;
    }
    bool satisfied = false;
    for (const EvaluationRecord& record : records) {
      if (record.evaluator_key == requirement.evaluator_key && record.complete &&
          record.authoritative_for_mandatory()) {
        satisfied = true;
        break;
      }
    }
    if (!satisfied) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool contains_id(const std::vector<CandidateId>& values, CandidateId probe) {
  for (const CandidateId value : values) {
    if (value == probe) {
      return true;
    }
  }
  return false;
}

/// One observed selection decision must be a total, gated ranking.
void check_decision(FoundryCore& core, TaskId task_id, PopulationId population_id,
                    const SelectionDecision& decision, ProblemLog& problems) {
  if (decision.population != population_id) {
    problems.add("selection decision names a different population");
    return;
  }
  if (decision.task != task_id) {
    problems.add("selection decision names a different task");
    return;
  }
  if (decision.canonical_state_digest.empty()) {
    problems.add("selection decision carries no canonical state digest");
  }
  if (!decision.population_generation.valid()) {
    problems.add("selection decision carries an invalid population generation");
  }

  if (decision.ranking.empty()) {
    if (decision.has_winner()) {
      problems.add("selection decision ranks nothing yet names a winner");
    }
    return;
  }
  if (decision.selected != decision.ranking.front().candidate) {
    problems.add("selection decision winner is not the top ranked candidate");
  }
  for (std::size_t index = 0; index < decision.ranking.size(); ++index) {
    const RankingEntry& entry = decision.ranking[index];
    if (entry.rank != static_cast<std::uint32_t>(index + 1)) {
      problems.add("ranking is not contiguous from rank 1");
      break;
    }
    if (index != 0 && decision.ranking[index - 1].total_score < entry.total_score) {
      problems.add("ranking is not ordered by descending total score");
      break;
    }
    for (const ExclusionEntry& exclusion : decision.excluded) {
      if (exclusion.candidate == entry.candidate) {
        problems.add("candidate is both ranked and excluded");
        break;
      }
    }
  }

  const Result<TaskSpec> task = core.task(task_id);
  if (!task.ok()) {
    problems.add("task lookup failed while checking a decision: " + code_of(task.status()));
    return;
  }
  for (const RankingEntry& entry : decision.ranking) {
    if (!satisfies_mandatory_gates(core, task.value(), entry.candidate)) {
      problems.add("a ranked candidate does not satisfy every mandatory gate");
      break;
    }
  }
}

/// Internal coherence of one immutable snapshot: every reference resolves
/// inside the same snapshot, so the copy is complete rather than partial.
void check_snapshot_coherence(const FoundrySnapshot& snapshot, ProblemLog& problems) {
  if (!snapshot.foundry.valid() || !snapshot.run.valid() || !snapshot.epoch.valid()) {
    problems.add("snapshot carries an invalid run identity");
  }
  for (const auto& population_entry : snapshot.populations) {
    const PopulationRecord& population = population_entry.second;
    if (!population.generation.valid()) {
      problems.add("snapshot population has an invalid generation");
    }
    if (population.candidates.size() > population.candidate_budget) {
      problems.add("snapshot population lists more candidates than its budget");
    }
    for (const CandidateId candidate_id : population.candidates) {
      const auto candidate = snapshot.candidates.find(candidate_id);
      if (candidate == snapshot.candidates.end()) {
        problems.add("snapshot population references a missing candidate");
        continue;
      }
      if (candidate->second.population != population.id) {
        problems.add("snapshot candidate names a different population");
      }
    }
  }
  for (const auto& candidate_entry : snapshot.candidates) {
    const CandidateRecord& candidate = candidate_entry.second;
    if (!candidate.generation.valid()) {
      problems.add("snapshot candidate has an invalid generation");
    }
    if (snapshot.populations.find(candidate.population) == snapshot.populations.end()) {
      problems.add("snapshot candidate names a missing population");
    }
  }
  for (const auto& attempt_entry : snapshot.attempts) {
    const AttemptRecord& attempt = attempt_entry.second;
    if (snapshot.candidates.find(attempt.candidate) == snapshot.candidates.end()) {
      problems.add("snapshot attempt names a missing candidate");
    }
    if (snapshot.populations.find(attempt.population) == snapshot.populations.end()) {
      problems.add("snapshot attempt names a missing population");
    }
    if (snapshot.assignments.find(attempt.assignment) == snapshot.assignments.end()) {
      problems.add("snapshot attempt names a missing assignment");
    }
  }
  for (const auto& evaluation_entry : snapshot.evaluations) {
    if (snapshot.candidates.find(evaluation_entry.second.candidate) == snapshot.candidates.end()) {
      problems.add("snapshot evaluation names a missing candidate");
    }
  }
  for (const auto& selection_entry : snapshot.selections) {
    if (snapshot.populations.find(selection_entry.first) == snapshot.populations.end()) {
      problems.add("snapshot selection names a missing population");
    }
  }
  if (snapshot.statistics.candidates_published > snapshot.candidates.size()) {
    problems.add("snapshot publishes more candidates than it holds");
  }
}

// ---------------------------------------------------------------------------
// Reader progress
// ---------------------------------------------------------------------------

/// Progress signal for one reader thread of a case whose writer is bounded.
///
/// A writer that performs a fixed number of rounds makes the durable counts such
/// a case asserts exact, and says nothing about whether a reader ever got the
/// lock: under contention a reader can lose every race and reach the end with
/// zero rounds. That zero is then indistinguishable from a read path that ran
/// and observed nothing, so a case that asserts reader progress waits for it
/// rather than assuming it.
struct ReaderProgress {
  std::atomic<bool> progressed{false};
  std::atomic<bool> finished{false};
};

/// Wait until every reader has completed a round or has stopped on its own.
///
/// This is progress, not a deadline: nothing here bounds how long a reader may
/// take. A reader that exits without completing a round is then reported by the
/// case's own progress assertion - a verdict - instead of being silently
/// tolerated by it.
void await_reader_progress(const std::vector<ReaderProgress>& progress) {
  for (;;) {
    bool waiting = false;
    for (const ReaderProgress& entry : progress) {
      if (!entry.progressed.load(std::memory_order_acquire) &&
          !entry.finished.load(std::memory_order_acquire)) {
        waiting = true;
        break;
      }
    }
    if (!waiting) {
      return;
    }
    std::this_thread::yield();
  }
}

// ---------------------------------------------------------------------------
// Reader bodies
// ---------------------------------------------------------------------------

void read_paths_reader(FoundryCore& core, PopulationId population, const std::atomic<bool>& stop,
                       ProblemLog& problems, std::uint64_t* iterations,
                       ReaderProgress* progress = nullptr) {
  std::uint64_t revision = core.revision();
  FoundryStatistics statistics = core.statistics();
  std::size_t candidate_count = 0;
  std::size_t lineage_count = 0;
  std::uint64_t rounds = 0;
  while (!stop.load(std::memory_order_acquire)) {
    ++rounds;

    // Every read path takes the lock on its own, so each call is checked for the
    // invariants it must satisfy by itself. Invariants that span several
    // entities are checked against one snapshot instead, because a second read
    // may legitimately observe a later revision.
    const Result<PopulationRecord> record = core.population(population);
    if (!record.ok()) {
      problems.add("population() failed: " + code_of(record.status()));
      break;
    }
    if (!record.value().generation.valid()) {
      problems.add("population() returned an invalid generation");
      break;
    }

    const std::vector<CandidateId> candidates = core.population_candidates(population);
    if (candidates.size() > record.value().candidate_budget) {
      problems.add("population_candidates() exceeds the candidate budget");
      break;
    }
    if (candidates.size() < candidate_count) {
      problems.add("population_candidates() dropped a candidate slot");
      break;
    }
    candidate_count = candidates.size();
    for (const CandidateId candidate_id : candidates) {
      const Result<CandidateRecord> candidate = core.candidate(candidate_id);
      if (!candidate.ok()) {
        problems.add("candidate() failed for a listed candidate: " + code_of(candidate.status()));
        break;
      }
      if (candidate.value().population != population) {
        problems.add("candidate() returned a candidate of another population");
        break;
      }
      if (!candidate.value().generation.valid()) {
        problems.add("candidate() returned an invalid generation");
        break;
      }
    }

    // A candidate may settle between the two calls, so only the identity is
    // checked here: a candidate that was returned must still exist. The full
    // state invariant is checked against one snapshot below.
    const std::vector<CandidateId> awaiting = core.candidates_awaiting_evaluation();
    for (const CandidateId candidate_id : awaiting) {
      if (!core.candidate(candidate_id).ok()) {
        problems.add("candidates_awaiting_evaluation() named a missing candidate");
        break;
      }
    }

    const std::vector<LineageNode> nodes = core.lineage_nodes();
    if (nodes.size() < lineage_count) {
      problems.add("lineage_nodes() dropped a node");
      break;
    }
    lineage_count = nodes.size();
    for (const LineageNode& node : nodes) {
      if (!core.candidate(node.candidate).ok()) {
        problems.add("lineage_nodes() named a missing candidate");
        break;
      }
    }

    const FoundryStatistics observed = core.statistics();
    if (observed.candidates_published < statistics.candidates_published ||
        observed.attempts_authorized < statistics.attempts_authorized ||
        observed.candidate_slots_created < statistics.candidate_slots_created ||
        observed.evaluations_recorded < statistics.evaluations_recorded) {
      problems.add("a monotonic statistics counter regressed");
      break;
    }
    if (observed.candidates_published > observed.candidate_slots_created) {
      problems.add("statistics publish more candidates than slots exist");
      break;
    }
    statistics = observed;

    const std::uint64_t observed_revision = core.revision();
    if (observed_revision < revision) {
      problems.add("revision() regressed");
      break;
    }
    revision = observed_revision;

    // One atomic copy: now every cross-entity reference must resolve inside the
    // same revision.
    const FoundrySnapshot snapshot = core.snapshot();
    const auto population_entry = snapshot.populations.find(population);
    if (population_entry == snapshot.populations.end()) {
      problems.add("snapshot() omitted the live population");
      break;
    }
    for (const CandidateId candidate_id : population_entry->second.candidates) {
      if (snapshot.candidates.find(candidate_id) == snapshot.candidates.end()) {
        problems.add("snapshot population references a candidate the snapshot does not hold");
        break;
      }
    }
    if (snapshot.lineage.nodes().size() != snapshot.candidates.size()) {
      problems.add("snapshot lineage does not cover every candidate exactly once");
      break;
    }
    if (snapshot.statistics.candidates_published > snapshot.candidates.size()) {
      problems.add("snapshot publishes more candidates than it holds");
      break;
    }
    if (progress != nullptr && !progress->progressed.load(std::memory_order_relaxed)) {
      progress->progressed.store(true, std::memory_order_release);
    }
  }
  *iterations = rounds;
  if (progress != nullptr) {
    progress->finished.store(true, std::memory_order_release);
  }
}

struct SnapshotTally {
  std::uint64_t taken{0};
  std::uint64_t round_tripped{0};
};

void snapshot_reader(FoundryCore& core, const std::atomic<bool>& stop, ProblemLog& problems,
                     SnapshotTally* tally, ReaderProgress* progress = nullptr) {
  SnapshotTally local;
  while (!stop.load(std::memory_order_acquire)) {
    const FoundrySnapshot snapshot = core.snapshot();
    ++local.taken;
    if (progress != nullptr) {
      progress->progressed.store(true, std::memory_order_release);
    }
    check_snapshot_coherence(snapshot, problems);

    const Result<std::string> image = serialize_snapshot(snapshot);
    if (!image.ok()) {
      problems.add("serialize_snapshot() failed: " + code_of(image.status()));
      continue;
    }
    const Result<FoundrySnapshot> restored = deserialize_snapshot(image.value());
    if (!restored.ok()) {
      problems.add("deserialize_snapshot() failed: " + code_of(restored.status()) + " '" +
                   restored.status().message() + "'");
      continue;
    }
    if (!snapshots_are_equal(snapshot, restored.value())) {
      problems.add("round trip changed the snapshot: " +
                   first_snapshot_difference(snapshot, restored.value()));
      continue;
    }
    ++local.round_tripped;
  }
  *tally = local;
  if (progress != nullptr) {
    progress->finished.store(true, std::memory_order_release);
  }
}

/// A reader runs a fixed number of iterations, so its own termination never
/// depends on how far the writer got. The first successful observation is
/// published to the writer: the writer must not commit before a reader has seen
/// the state the commit is derived from.
void selection_reader(FoundryCore& core, TaskId task_id, PopulationId population,
                      std::uint32_t iterations, std::atomic<bool>& observed, ProblemLog& problems,
                      std::uint64_t* prepared_count, std::uint64_t* refused_count) {
  std::uint64_t prepared = 0;
  std::uint64_t refused = 0;
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    const Result<SelectionDecision> decision = core.prepare_selection(population);
    if (!decision.ok()) {
      ++refused;
      continue;
    }
    ++prepared;
    observed.store(true, std::memory_order_release);
    check_decision(core, task_id, population, decision.value(), problems);
  }
  *prepared_count = prepared;
  *refused_count = refused;
}

}  // namespace

AF_TEST_CASE(concurrency, read_paths_stay_consistent_while_a_writer_mutates) {
  constexpr std::uint32_t kRounds = 8;
  constexpr std::size_t kReaders = 8;

  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(31));
  af_test::build_running_fixture(af_ctx, fixture, 24);

  std::atomic<bool> stop{false};
  ProblemLog problems;
  std::vector<std::uint64_t> reader_rounds(kReaders, 0);
  std::vector<ReaderProgress> progress(kReaders);
  std::uint64_t writer_rounds = 0;
  std::string writer_failure;

  af_ctx.phase("DISPATCH");
  std::thread writer([&]() {
    for (std::uint32_t round = 0; round < kRounds; ++round) {
      const Status status = writer_round(fixture.core, fixture.population, round,
                                         round, round + 1 < kRounds);
      if (!status.ok()) {
        writer_failure = "writer round " + std::to_string(round) + " failed: " + code_of(status) +
                         " '" + status.message() + "'";
        break;
      }
      ++writer_rounds;
    }
    // The readers are not stopped here. The writer's rounds are bounded so the
    // durable counts the case asserts are exact, and that bound says nothing
    // about whether a reader ever got the lock; the case stops the readers only
    // once each has completed a round.
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (std::size_t index = 0; index < kReaders; ++index) {
    readers.emplace_back([&, index]() {
      read_paths_reader(fixture.core, fixture.population, stop, problems, &reader_rounds[index],
                        &progress[index]);
    });
  }

  af_ctx.phase("WAIT");
  writer.join();
  await_reader_progress(progress);
  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  af_ctx.phase("VERIFY");
  EXPECT_TRUE(af_ctx, writer_failure.empty());
  if (!writer_failure.empty()) {
    af_ctx.note(writer_failure);
  }
  EXPECT_EQ(af_ctx, writer_rounds, static_cast<std::uint64_t>(kRounds));
  for (std::size_t index = 0; index < kReaders; ++index) {
    EXPECT_TRUE(af_ctx, reader_rounds[index] > 0);
  }
  const std::vector<std::string> observed = problems.take();
  if (!observed.empty()) {
    af_ctx.note(render(observed));
  }
  EXPECT_TRUE(af_ctx, observed.empty());

  af_ctx.phase("VERIFY_AUDIT");
  const std::vector<std::string> violations = fixture.core.audit();
  EXPECT_TRUE(af_ctx, violations.empty());
  if (!violations.empty()) {
    af_ctx.note(render(violations));
  }

  af_ctx.phase("VERIFY_STATISTICS");
  const FoundryStatistics statistics = fixture.core.statistics();
  EXPECT_EQ(af_ctx, statistics.candidate_slots_created, static_cast<std::uint64_t>(kRounds));
  EXPECT_EQ(af_ctx, statistics.candidates_published, static_cast<std::uint64_t>(kRounds));
  EXPECT_EQ(af_ctx, statistics.attempts_authorized, static_cast<std::uint64_t>(kRounds));
  EXPECT_EQ(af_ctx, statistics.attempts_failed, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.attempts_cancelled, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.attempts_outcome_unknown, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.populations_created, static_cast<std::uint64_t>(1));
  EXPECT_EQ(af_ctx, statistics.populations_closed, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.selections_prepared, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.stale_authority_rejections, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.duplicate_rejections, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.late_rejections, static_cast<std::uint64_t>(0));
  EXPECT_EQ(af_ctx, statistics.illegal_transition_rejections, static_cast<std::uint64_t>(0));
  // Every recorded evaluation is durable evidence for exactly one candidate, and
  // each of the four reference requirements produced exactly one record.
  const std::vector<CandidateId> candidates =
      fixture.core.population_candidates(fixture.population);
  EXPECT_EQ(af_ctx, candidates.size(), static_cast<std::size_t>(kRounds));
  std::size_t records = 0;
  for (const CandidateId candidate_id : candidates) {
    const std::vector<EvaluationRecord> evaluations = fixture.core.candidate_evaluations(candidate_id);
    EXPECT_EQ(af_ctx, evaluations.size(), static_cast<std::size_t>(4));
    records += evaluations.size();
    const Result<CandidateRecord> candidate = fixture.core.candidate(candidate_id);
    EXPECT_OK(af_ctx, candidate);
    if (candidate.ok()) {
      EXPECT_EQ(af_ctx, candidate.value().state, CandidateState::Evaluated);
    }
  }
  EXPECT_EQ(af_ctx, records, static_cast<std::size_t>(kRounds) * 4);
  EXPECT_TRUE(af_ctx, statistics.evaluations_recorded >= static_cast<std::uint64_t>(records));
  EXPECT_EQ(af_ctx, fixture.core.workers().size(), static_cast<std::size_t>(kRounds));

  af_ctx.phase("SHUTDOWN");
  EXPECT_TRUE(af_ctx, stop.load(std::memory_order_acquire));
}

AF_TEST_CASE(concurrency, snapshots_are_complete_immutable_copies_under_mutation) {
  constexpr std::uint32_t kRounds = 6;
  constexpr std::size_t kReaders = 3;

  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(32));
  af_test::build_running_fixture(af_ctx, fixture, 24);

  std::atomic<bool> stop{false};
  ProblemLog problems;
  std::vector<SnapshotTally> tallies(kReaders);
  std::vector<ReaderProgress> progress(kReaders);
  std::uint64_t writer_rounds = 0;
  std::string writer_failure;

  af_ctx.phase("DISPATCH");
  std::thread writer([&]() {
    for (std::uint32_t round = 0; round < kRounds; ++round) {
      const Status status = writer_round(fixture.core, fixture.population, round,
                                         round, round + 1 < kRounds);
      if (!status.ok()) {
        writer_failure = "writer round " + std::to_string(round) + " failed: " + code_of(status) +
                         " '" + status.message() + "'";
        break;
      }
      ++writer_rounds;
    }
    // The readers are not stopped here. The writer's rounds are bounded so the
    // durable counts the case asserts are exact, and that bound says nothing
    // about whether a reader ever took a snapshot; the case stops the readers
    // only once each has taken one.
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (std::size_t index = 0; index < kReaders; ++index) {
    readers.emplace_back([&, index]() {
      snapshot_reader(fixture.core, stop, problems, &tallies[index], &progress[index]);
    });
  }

  af_ctx.phase("WAIT");
  writer.join();
  await_reader_progress(progress);
  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  af_ctx.phase("VERIFY");
  EXPECT_TRUE(af_ctx, writer_failure.empty());
  if (!writer_failure.empty()) {
    af_ctx.note(writer_failure);
  }
  EXPECT_EQ(af_ctx, writer_rounds, static_cast<std::uint64_t>(kRounds));

  std::uint64_t taken = 0;
  std::uint64_t round_tripped = 0;
  for (const SnapshotTally& tally : tallies) {
    EXPECT_TRUE(af_ctx, tally.taken > 0);
    taken += tally.taken;
    round_tripped += tally.round_tripped;
  }
  af_ctx.note("snapshots taken " + std::to_string(taken) + " round tripped " +
              std::to_string(round_tripped));

  const std::vector<std::string> observed = problems.take();
  if (!observed.empty()) {
    af_ctx.note(render(observed));
  }
  EXPECT_TRUE(af_ctx, observed.empty());
  // Every snapshot that was taken must have survived the round trip: a snapshot
  // is a complete immutable copy, so serializing and reloading it cannot change
  // one durable field.
  EXPECT_EQ(af_ctx, round_tripped, taken);

  af_ctx.phase("VERIFY_FINAL_SNAPSHOT");
  const FoundrySnapshot snapshot = fixture.core.snapshot();
  check_snapshot_coherence(snapshot, problems);
  const std::vector<std::string> final_problems = problems.take();
  if (!final_problems.empty()) {
    af_ctx.note(render(final_problems));
  }
  EXPECT_TRUE(af_ctx, final_problems.empty());
  EXPECT_EQ(af_ctx, snapshot.populations.size(), static_cast<std::size_t>(1));
  EXPECT_EQ(af_ctx, snapshot.candidates.size(), static_cast<std::size_t>(kRounds));
  EXPECT_EQ(af_ctx, snapshot.foundry, fixture.core.foundry());
  EXPECT_EQ(af_ctx, snapshot.run, fixture.core.run());
  EXPECT_EQ(af_ctx, snapshot.epoch, fixture.core.epoch());
  REQUIRE_VALUE(af_ctx, std::string, image, serialize_snapshot(snapshot));
  EXPECT_FALSE(af_ctx, image.empty());
  REQUIRE_VALUE(af_ctx, FoundrySnapshot, restored, deserialize_snapshot(image));
  EXPECT_TRUE(af_ctx, snapshots_are_equal(snapshot, restored));

  af_ctx.phase("SHUTDOWN");
  EXPECT_TRUE(af_ctx, stop.load(std::memory_order_acquire));
}

AF_TEST_CASE(concurrency, repeated_construction_and_destruction_is_stable) {
  constexpr std::uint32_t kIterations = 200;

  af_ctx.phase("SETUP");
  std::uint64_t completed = 0;
  for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
    {
      af_test::FoundryFixture fixture(af_test::make_config(300 + iteration));
      af_test::build_running_fixture(af_ctx, fixture, 1);

      const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, 0);
      const CandidateId candidate = af_test::produce_candidate(
          af_ctx, fixture.core, session, af_test::reference_solution_source(0), "closed-form");
      EXPECT_OK(af_ctx, fixture.core.begin_evaluation(candidate));
      const CandidateRecord published = af_test::load_candidate(af_ctx, fixture.core, candidate);
      af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorCompileAndRun,
                               EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                               RequirementClass::Mandatory, false, 0.0);
      af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorSourcePolicy,
                               EvaluationOutcome::Pass, EvaluatorKind::SourcePolicy,
                               RequirementClass::Mandatory, false, 0.0);
      af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorPerformance,
                               EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                               RequirementClass::Optional, true, 4.0);
      af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorParsimony,
                               EvaluationOutcome::Pass, EvaluatorKind::PortableReference,
                               RequirementClass::Optional, true, 4.0);

      af_ctx.phase("VERIFY");
      const std::vector<CandidateId> candidates =
          fixture.core.population_candidates(fixture.population);
      EXPECT_EQ(af_ctx, candidates.size(), static_cast<std::size_t>(1));
      EXPECT_EQ(af_ctx, fixture.core.population_ids().size(), static_cast<std::size_t>(1));
      EXPECT_EQ(af_ctx, fixture.core.workers().size(), static_cast<std::size_t>(1));
      EXPECT_EQ(af_ctx, fixture.core.lineage_nodes().size(), static_cast<std::size_t>(1));
      if (!candidates.empty()) {
        const CandidateRecord settled = af_test::load_candidate(af_ctx, fixture.core, candidates[0]);
        EXPECT_EQ(af_ctx, settled.state, CandidateState::Evaluated);
      }
      const FoundryStatistics statistics = fixture.core.statistics();
      EXPECT_EQ(af_ctx, statistics.candidate_slots_created, static_cast<std::uint64_t>(1));
      EXPECT_EQ(af_ctx, statistics.candidates_published, static_cast<std::uint64_t>(1));
      EXPECT_EQ(af_ctx, statistics.populations_created, static_cast<std::uint64_t>(1));
      EXPECT_EQ(af_ctx, statistics.illegal_transition_rejections, static_cast<std::uint64_t>(0));
      EXPECT_TRUE(af_ctx, fixture.core.revision() > 0);
      const std::vector<std::string> violations = fixture.core.audit();
      EXPECT_TRUE(af_ctx, violations.empty());
      if (!violations.empty()) {
        af_ctx.note(render(violations));
      }
      ++completed;
    }
    af_ctx.phase("DESTROY");
    (void)iteration;
  }

  af_ctx.phase("SHUTDOWN");
  EXPECT_EQ(af_ctx, completed, static_cast<std::uint64_t>(kIterations));
}

AF_TEST_CASE(concurrency, racing_selection_produces_at_most_one_winner) {
  constexpr std::uint32_t kCandidates = 3;
  constexpr std::uint32_t kWriterIterations = 40;
  constexpr std::uint32_t kReaderIterations = 200;
  constexpr std::size_t kReaderCount = 2;

  af_ctx.phase("SETUP");
  af_test::FoundryFixture fixture(af_test::make_config(33));
  af_test::build_running_fixture(af_ctx, fixture, 8);

  af_ctx.phase("DISPATCH");
  // One candidate at a time: publishing settles the population into Evaluating,
  // so the documented revalidation cycle must return it to Running before the
  // next production attempt can be authorized.
  std::vector<CandidateId> candidates;
  for (std::uint32_t slot = 0; slot < kCandidates; ++slot) {
    const WorkerSessionAuthority session = af_test::connect_worker(af_ctx, fixture.core, slot);
    const CandidateId candidate = af_test::produce_candidate(
        af_ctx, fixture.core, session,
        af_test::reference_solution_source(static_cast<int>(slot)), "strategy-" + std::to_string(slot));
    candidates.push_back(candidate);
    EXPECT_OK(af_ctx, fixture.core.begin_evaluation(candidate));
    const CandidateRecord published = af_test::load_candidate(af_ctx, fixture.core, candidate);
    af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorCompileAndRun,
                             EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                             RequirementClass::Mandatory, false, 0.0);
    af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorSourcePolicy,
                             EvaluationOutcome::Pass, EvaluatorKind::SourcePolicy,
                             RequirementClass::Mandatory, false, 0.0);
    af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorPerformance,
                             EvaluationOutcome::Pass, EvaluatorKind::ProcessCommand,
                             RequirementClass::Optional, true, 1.0 + static_cast<double>(slot));
    af_test::record_evidence(af_ctx, fixture.core, published, kEvaluatorParsimony,
                             EvaluationOutcome::Pass, EvaluatorKind::PortableReference,
                             RequirementClass::Optional, true, 1.0 + static_cast<double>(slot));
    if (slot + 1 < kCandidates) {
      EXPECT_OK(af_ctx, cycle_population(fixture.core, fixture.population));
    }
  }
  for (const CandidateId candidate_id : candidates) {
    const CandidateRecord record = af_test::load_candidate(af_ctx, fixture.core, candidate_id);
    EXPECT_EQ(af_ctx, record.state, CandidateState::Evaluated);
  }

  std::atomic<bool> stop{false};
  std::atomic<bool> reader_observed{false};
  ProblemLog problems;
  std::mutex decision_mutex;
  SelectionDecision last_committed;
  bool have_committed = false;
  std::uint64_t commits = 0;
  std::uint64_t commit_refusals = 0;
  std::vector<std::uint64_t> prepared_counts(kReaderCount, 0);
  std::vector<std::uint64_t> refused_counts(kReaderCount, 0);

  af_ctx.phase("COMMIT");
  std::thread writer([&]() {
    bool waiting_for_a_reader = true;
    for (std::uint32_t iteration = 0; iteration < kWriterIterations; ++iteration) {
      const Result<SelectionDecision> prepared = fixture.core.prepare_selection(fixture.population);
      if (!prepared.ok()) {
        ++commit_refusals;
        continue;
      }
      if (waiting_for_a_reader) {
        // Real synchronisation, not a delay: the readers are running and their
        // next prepare cannot fail here, because preparing only records a
        // decision and leaves the population in a state that admits another
        // prepare. The writer therefore cannot commit before a reader has
        // observed the same state it is about to commit.
        while (!reader_observed.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        waiting_for_a_reader = false;
      }
      const Result<SelectionDecision> committed =
          fixture.core.commit_selection(prepared.value());
      if (!committed.ok()) {
        ++commit_refusals;
        continue;
      }
      ++commits;
      std::lock_guard<std::mutex> guard(decision_mutex);
      last_committed = committed.value();
      have_committed = true;
    }
    stop.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  readers.reserve(kReaderCount);
  for (std::size_t index = 0; index < kReaderCount; ++index) {
    readers.emplace_back([&, index]() {
      selection_reader(fixture.core, fixture.task, fixture.population, kReaderIterations,
                       reader_observed, problems, &prepared_counts[index], &refused_counts[index]);
    });
  }

  af_ctx.phase("WAIT");
  writer.join();
  for (std::thread& reader : readers) {
    reader.join();
  }

  af_ctx.phase("VERIFY");
  af_ctx.note("commits " + std::to_string(commits) + " refused " +
              std::to_string(commit_refusals) + " readers prepared " +
              std::to_string(prepared_counts[0] + prepared_counts[1]) + " refused " +
              std::to_string(refused_counts[0] + refused_counts[1]));
  const std::vector<std::string> observed = problems.take();
  if (!observed.empty()) {
    af_ctx.note(render(observed));
  }
  EXPECT_TRUE(af_ctx, observed.empty());
  EXPECT_TRUE(af_ctx, commits > 0);
  EXPECT_TRUE(af_ctx, reader_observed.load(std::memory_order_acquire));
  EXPECT_TRUE(af_ctx, prepared_counts[0] + prepared_counts[1] > 0);

  af_ctx.phase("VERIFY_SINGLE_WINNER");
  std::size_t winners = 0;
  CandidateId winner;
  for (const CandidateId candidate_id : candidates) {
    const CandidateRecord record = af_test::load_candidate(af_ctx, fixture.core, candidate_id);
    if (record.selected) {
      ++winners;
      winner = candidate_id;
    }
  }
  EXPECT_EQ(af_ctx, winners, static_cast<std::size_t>(1));

  const Result<PopulationRecord> population = fixture.core.population(fixture.population);
  EXPECT_OK(af_ctx, population);
  if (population.ok()) {
    EXPECT_TRUE(af_ctx, population.value().committed_selection.valid());
  }

  std::lock_guard<std::mutex> guard(decision_mutex);
  if (have_committed) {
    check_decision(fixture.core, fixture.task, fixture.population, last_committed, problems);
    const std::vector<std::string> decision_problems = problems.take();
    if (!decision_problems.empty()) {
      af_ctx.note(render(decision_problems));
    }
    EXPECT_TRUE(af_ctx, decision_problems.empty());
    EXPECT_EQ(af_ctx, last_committed.selected, winner);
    REQUIRE_VALUE(af_ctx, TaskSpec, task, fixture.core.task(fixture.task));
    EXPECT_TRUE(af_ctx, satisfies_mandatory_gates(fixture.core, task, last_committed.selected));
  }

  af_ctx.phase("VERIFY_AUDIT");
  const std::vector<std::string> violations = fixture.core.audit();
  EXPECT_TRUE(af_ctx, violations.empty());
  if (!violations.empty()) {
    af_ctx.note(render(violations));
  }

  af_ctx.phase("SHUTDOWN");
  EXPECT_TRUE(af_ctx, stop.load(std::memory_order_acquire));
}
